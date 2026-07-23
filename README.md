# uperf-linux

> Userspace performance scheduler for Linux ARM64 gaming devices.

[Chinese README](README_zh.md) | [License](LICENSE) | [GitHub](https://github.com/EmberLuo/uperf-linux-fork)

## Overview

`uperf-linux` is a systemd-managed daemon for Linux ARM64 devices. It reads a
JSON configuration, tracks scene/load/process state, and applies automatic CPU
frequency limits, manual CPU/GPU frequency overrides, and task scheduling policy
through standard Linux interfaces.

The current daemon is built around a small set of runtime contracts:

- CPU cpufreq policies are discovered from `/sys/devices/system/cpu/cpufreq`
  and matched to configured CPU masks by each policy's `related_cpus`.
- GPU frequency targets use the configured `gpuMinFreq` and `gpuMaxFreq`
  devfreq paths for manual overrides and restore handling.
- Power-mode selection is foreground-first: an active detected game may override
  the user's baseline mode, while background games use deterministic fallback
  priority.
- Frequency writes are transactional: requested limits are snapped to real OPPs,
  verified by readback, and restored on clean shutdown or crash recovery.
- Process and thread scheduling are reconciled periodically from `/proc`, using
  configured affinity, scheduler priority, and cgroup classes.
- Control is exposed through `org.uperflinux.Daemon` on the system bus; mutating
  methods are authorized by Polkit.

## Runtime Flow

```
systemd Type=dbus service
        |
        v
uperf-linux daemon
  |
  +-- ConfigParser      JSON config load, validation, SIGHUP/D-Bus reload
  +-- RuntimeBackend    injectable /proc, /sys, and monotonic-clock access
  +-- InputMonitor      touchscreen evdev events
  +-- HeavyLoadDetector /proc/stat load sampling
  +-- ThermalManager    /sys/class/thermal temperature state
  +-- GameScanner       /proc process discovery and per-app mode lookup
  +-- ModeSelector      requested mode + foreground game override
  +-- StateMachine      idle/touch/trigger/gesture/junk/switch/boost actions
  +-- FreqController    demand -> frequency limits -> OPP snap -> sysfs write
  +-- TaskScheduler     process/thread affinity and scheduler attributes
  +-- CgroupManager     systemd/cgroup resource class reconciliation
  +-- DbusManager       CLI/GUI API, stats, Polkit authorization
```

At startup the daemon loads and validates the config, restores any frequency
state left by a previous process, discovers CPU/GPU targets, creates the
detectors and managers, then publishes the initial `balance` mode. The main loop
dispatches D-Bus work, handles reload requests, samples load and input, updates
thermal state, recomputes the active scene/mode, applies automatic frequency
control, scans game processes, and reconciles task/cgroup policy.

## Binaries

- `uperf-linux`: root daemon, normally launched by systemd.
- `uperfctl`: command-line client for status, mode changes, active PID
  selection, manual frequency overrides, and hardware detection.
- `uperf-wizard`: hardware config detector.
- `uperf-gui`: GTK4/libadwaita controller for the daemon.

## Supported Platform

The shipped config targets SM8550 devices such as Xiaomi Pad 6S Pro:

| Area | Current expectation |
| --- | --- |
| CPU | Three cpufreq policies matching configured masks for efficiency, performance, and prime clusters |
| GPU | devfreq node at `/sys/class/devfreq/3d00000.gpu` |
| Governor | `schedutil` is expected; the daemon controls min/max limits rather than replacing the governor |
| Cgroups | systemd-managed cgroup v2 |
| Input | Linux evdev touchscreen devices |

Other ARM64 devices need a matching JSON config. If the detected CPU topology
does not match the configured power model, automatic CPU frequency control is
disabled instead of guessing.

## Build

### Dependencies

Debian/Ubuntu:

```bash
sudo apt install build-essential cmake pkg-config libjson-c-dev \
    libglib2.0-dev libdbus-1-dev libpolkit-gobject-1-dev libsystemd-dev \
    libgtk-4-dev libadwaita-1-dev desktop-file-utils
```

Headless daemon build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_GUI=OFF
cmake --build build -j "$(nproc)"
ctest --test-dir build --output-on-failure
```

Full build with GUI:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j "$(nproc)"
ctest --test-dir build --output-on-failure
```

## Install

Install from the CMake build tree:

```bash
sudo cmake --install build
sudo systemctl daemon-reload
sudo systemctl enable --now uperf-linux.service
```

Build and install a Debian package:

```bash
cmake --build build --target package
sudo dpkg -i build/uperf-linux-0.1.0-arm64.deb
sudo systemctl enable --now uperf-linux.service
```

The installed daemon reads `/etc/uperf-linux/config.json` and
`/etc/uperf-linux/perapp_powermode`.

## CLI Usage

```bash
uperfctl status
uperfctl mode balance
uperfctl mode powersave
uperfctl mode performance
uperfctl game-list
uperfctl active-pid <pid|0>
uperfctl set-freq <cluster> <freq_hz>
uperfctl show-freqs
uperfctl detect
```

For `set-freq`, cluster `-1` is GPU, `0..2` are CPU clusters from the active
config, and `3` applies to all CPU clusters. A frequency of `0` releases the
manual override.

`uperfctl` exit codes are stable: `0` success, `1` usage error, `2` bad client
argument, `3` daemon unavailable, and `4` daemon rejected the request.

## Configuration

The default config is `config/sm8550.json`. The installed runtime copy is
`/etc/uperf-linux/config.json`.

### Schema

`meta.schemaVersion` is optional. Missing means legacy schema `0`; values newer
than the daemon's `CONFIG_SCHEMA_VERSION` are rejected.

### CPU Power Model

Recognized per-cluster fields:

```json
{
  "efficiency": 350,
  "nr": 1,
  "cpumask": "prime",
  "typicalFreq": 2957,
  "sweetFreq": 2218,
  "freeFreq": 739
}
```

Use either `cpumask` to reference a mask under `modules.sched.cpumask`, or
`cpus` to provide the CPU list directly. The current power model maps relative
CPU demand to a target frequency with a linear performance-vs-frequency
approximation. `sweetFreq` is used only when a preset enables
`cpu.limitEfficiency`.

### Sysfs

CPU cpufreq paths are discovered automatically. The `modules.sysfs.knob` map is
currently used for GPU frequency paths:

```json
"knob": {
  "gpuMaxFreq": "/sys/class/devfreq/3d00000.gpu/max_freq",
  "gpuMinFreq": "/sys/class/devfreq/3d00000.gpu/min_freq"
}
```

### Presets

Presets may set these runtime tuning keys:

- `cpu.margin`
- `cpu.burst`
- `cpu.limitEfficiency`
- `cpu.baseSampleTime`

Example:

```json
"presets": {
  "balance": {
    "*": { "cpu.margin": 0.2 },
    "idle": {
      "cpu.baseSampleTime": 0.04,
      "cpu.limitEfficiency": true
    },
    "trigger": { "cpu.margin": 0.4 }
  }
}
```

### Scheduling and cgroups

`modules.sched` defines CPU masks, affinity profiles, priority profiles, and
process/thread rules. `modules.cgroup` maps matched workloads to cgroup classes
with CPU masks, `cpu.weight`, and uclamp limits. The daemon periodically
reconciles the live process set with those rules and restores original thread
policy when a workload is no longer managed.

## Testing

The CMake test suite covers parser validation, mode selection, state-machine
transitions, frequency limit computation, recovery snapshots, sysfs fault
injection, daemon reload/recovery behavior, D-Bus interface drift, scheduler
rules, cgroup reconciliation, and hardware-detection helpers.

```bash
ctest --test-dir build --output-on-failure
```

Hardware validation is available separately:

```bash
tools/validate-frequency-hardware.sh
```

Run write-readback validation only on hardware where temporary CPU/GPU frequency
limit changes are acceptable.

## Design Contracts

Intentional behavior that a rewrite should preserve is documented in
[docs/DESIGN_DECISIONS.md](docs/DESIGN_DECISIONS.md).

## License

This project is licensed under GPL-3.0-or-later. See [LICENSE](LICENSE).
