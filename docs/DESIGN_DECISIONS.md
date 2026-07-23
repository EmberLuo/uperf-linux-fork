# v1 Design Decisions

This document records behaviors that are **intentional for v1** — choices that
look surprising until you know the reasoning, and that a future rewrite (e.g. in
Rust) must preserve unless it deliberately decides otherwise. It exists so that
"is this a bug or a decision?" has a written answer anchored in the current
implementation and tests.

Each entry states the behavior, why it was chosen, and where it lives in the
code so the decision can be revisited with full context.

---

## 1. Power-mode selection is foreground-first

**Behavior.** When one or more games are detected, the effective global power
mode is chosen as follows (see `mode_selector_select` in
[src/mode_selector.c](../src/mode_selector.c) and the contract comment in
[src/include/mode_selector.h](../src/include/mode_selector.h)):

1. If an *active* process is identified (its PID **and** `/proc/<pid>/stat`
   start-time both match a detected game), that foreground game wins. Its
   per-app mode applies, or — when its per-app mode is `MODE_BALANCE` — the
   user's `requested` baseline applies (balance means "no per-app override").
2. Otherwise (no active id, the active id matches no candidate, or the
   foreground process is not a detected game) the selector falls back to a
   deterministic global scan with a fixed priority, independent of scan order:
   `performance/fast` > `powersave` > `requested`.

**Why.** The foreground workload is what the user is looking at, so it should
decide the mode. The order-independent fallback keeps the result deterministic
when no foreground game is identified, which is what makes the selector usable
as a golden-test oracle. `MODE_FAST` normalizes to `MODE_PERFORMANCE` in the
result.

**Consequence to be aware of — background games do not force a mode when a
foreground game is active.** If a detected game is running in the background
while a *different* foreground game is active, the background game's requested
mode does **not** override the foreground selection. This is deliberate: the
foreground process owns the mode. It is **kept as-is for v1**, documented here
rather than changed, because changing it would make a background process able to
pull the whole device into performance mode against the foreground workload's
interest.

---

## 2. cgroup unit resolution: active/game rules win over background siblings

**Behavior.** When several process rules resolve to the same systemd unit, they
are coalesced and the winner is chosen by a score
(`active` > `game` > `cpu_weight`) — see the coalescing loop in
[src/cgroup_manager.c](../src/cgroup_manager.c). An active or game-tagged rule
outranks a background sibling that shares the same unit.

**Why.** Multiple threads/processes of one application can match different
rules, but a systemd unit gets exactly one set of resource limits. Letting the
active/game rule win ensures the unit is tuned for the workload the user cares
about rather than for an incidental background helper in the same unit.

---

## 3. Config schema is versioned; unknown newer schemas are rejected at load

**Behavior.** `meta.schemaVersion` is an optional integer. An absent version is
treated as legacy (`0`) and accepted. A version newer than the build understands
(`CONFIG_SCHEMA_VERSION`, defined in [src/include/config.h](../src/include/config.h))
is **rejected at load time** in `parse_meta`, not merely warned about;
`config_validate` re-checks the range as defense in depth.

**Why.** If the schema has changed incompatibly, continuing to parse fields whose
meaning may have shifted would silently misinterpret the config. Failing fast is
safer than loading a misread config. Bump `CONFIG_SCHEMA_VERSION` whenever an
incompatible schema change lands.

---

## 4. Frequency writes use `O_TRUNC`; frequency state is restored on exit

**Behavior.** `write_sysfs_value` opens knobs with `O_WRONLY | O_TRUNC` (see
[src/sysfs_writer.c](../src/sysfs_writer.c)), matching the semantics of
`echo value > knob`. On a real sysfs node this is a harmless no-op; on the
regular-file backing used by the injectable runtime backend it prevents a
shorter value written over a longer one from leaving trailing digits (e.g.
writing `600000` over `1000000` must read back `600000`, not `6000000`).

The daemon records the hardware's original frequency limits and restores them on
clean shutdown and before swapping in a reloaded config; a config reload that is
rejected leaves the previously-applied targets untouched.

**Why.** Without `O_TRUNC`, frequency restore could silently corrupt the value
written back to the hardware. This was a real latent bug caught by the
config-reload recovery test; the guarantee is now covered by
`test_daemon_reload` and the fault-injection tests.

---

## 5. D-Bus interface XML has a single source of truth, guarded against drift

**Behavior.** The introspection XML the daemon registers is exposed via
`dbus_manager_introspection_xml()` and is compared, member-by-member, against
the shipped `config/dbus-interface.xml` by `test_dbus_xml_guard`. The two must
describe the same properties, methods, and signals.

**Why.** The daemon's registered interface and the documented/installed
interface description used to be duplicated, so they could silently diverge. The
guard test fails the build if they drift apart.

---

## 6. Privileged D-Bus calls are authorized by Polkit, not by the bus policy

**Behavior.** The system-bus policy in `config/org.uperflinux.Daemon.conf`
*allows* privileged method calls (`SetMode`, `SetManualFreq`, `ReloadConfig`, …)
to reach the daemon, but reaching the daemon does not grant the operation: the
daemon authorizes each caller with Polkit (`config/org.uperflinux.policy`).
Normal mode changes use the `org.uperflinux.control` action (allowed for active
local sessions); manual frequency overrides and config reloads use
`org.uperflinux.admin` (`auth_admin_keep`). State inspection
(`Get`/`GetAll`/`Introspect`) is read-only.

**Why.** Bus policy is a coarse gate; per-call authorization with proper admin
prompts belongs in Polkit. Unit tests cover the method-to-action mapping and the
rejected-call path through the daemon's injectable authorization hook.

---

## 7. `uperfctl` exit codes are distinct and documented

**Behavior.** `uperfctl` returns: `0` success, `1` usage error, `2` bad argument
value (rejected client-side), `3` daemon unavailable (could not reach the bus),
`4` request rejected by the daemon. See the `enum` and `print_usage` in
[src/cli.c](../src/cli.c).

**Why.** Scripts wrapping `uperfctl` need to tell "the daemon isn't running"
apart from "the daemon said no" and from "I passed a bad argument". A single
`0/1` result could not express that.
