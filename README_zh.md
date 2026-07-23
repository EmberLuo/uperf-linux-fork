# uperf-linux

> 面向 Linux ARM64 游戏设备的用户态性能调度器。

[English README](README.md) | [许可证](LICENSE) | [GitHub](https://github.com/EmberLuo/uperf-linux-fork)

## 概览

`uperf-linux` 是一个由 systemd 管理的守护进程。它读取 JSON 配置，跟踪场景、负载、温度和进程状态，然后通过标准 Linux 接口应用自动 CPU 频率限制、手动 CPU/GPU 频率覆盖、线程调度策略和 cgroup 资源分类。

当前代码的主要运行约束是：

- CPU cpufreq policy 从 `/sys/devices/system/cpu/cpufreq` 自动发现，并用每个 policy 的 `related_cpus` 和配置里的 CPU mask 精确匹配。
- GPU 频率目标使用配置中的 `gpuMinFreq` 和 `gpuMaxFreq` devfreq 路径，用于手动覆盖和恢复处理。
- 电源模式选择以前台为优先：当前活动游戏可以覆盖用户的基础模式；没有前台匹配时按确定性优先级回退。
- 频率写入会先对齐真实 OPP，再写入并读回验证；干净退出、配置重载和崩溃恢复都会尝试恢复原始频率限制。
- 进程/线程调度会周期性从 `/proc` 重新对账，按配置应用亲和性、调度优先级和 cgroup class。
- 控制接口暴露在 system bus 的 `org.uperflinux.Daemon` 上，修改类方法由 Polkit 授权。

## 运行流程

```
systemd Type=dbus service
        |
        v
uperf-linux daemon
  |
  +-- ConfigParser      JSON 加载、验证、SIGHUP/D-Bus 重载
  +-- RuntimeBackend    可注入的 /proc、/sys、单调时钟访问层
  +-- InputMonitor      触摸屏 evdev 事件
  +-- HeavyLoadDetector /proc/stat 负载采样
  +-- ThermalManager    /sys/class/thermal 温度状态
  +-- GameScanner       /proc 进程发现和按应用模式查询
  +-- ModeSelector      用户请求模式 + 前台游戏覆盖
  +-- StateMachine      idle/touch/trigger/gesture/junk/switch/boost 动作
  +-- FreqController    负载需求 -> 频率上下限 -> OPP 对齐 -> sysfs 写入
  +-- TaskScheduler     进程/线程亲和性和调度属性
  +-- CgroupManager     systemd/cgroup 资源 class 对账
  +-- DbusManager       CLI/GUI API、状态上报、Polkit 授权
```

启动时，守护进程会加载并验证配置，恢复上一次进程可能留下的频率状态，发现 CPU/GPU 目标，创建各个检测器和管理器，然后发布初始 `balance` 模式。主循环负责处理 D-Bus、配置重载、负载采样、输入事件、温度状态、场景/模式重算、自动频率控制、游戏扫描和调度/cgroup 对账。

## 可执行程序

- `uperf-linux`：root 守护进程，通常由 systemd 启动。
- `uperfctl`：命令行客户端，用于查看状态、切换模式、选择活动 PID、手动频率覆盖和硬件检测。
- `uperf-wizard`：硬件配置探测器。
- `uperf-gui`：GTK4/libadwaita 图形控制器。

## 支持平台

仓库自带的配置面向 SM8550 设备，例如小米平板 6S Pro：

| 项目 | 当前要求 |
| --- | --- |
| CPU | 三个 cpufreq policy，分别匹配 efficiency、performance、prime 的 CPU mask |
| GPU | `/sys/class/devfreq/3d00000.gpu` devfreq 节点 |
| Governor | 预期使用 `schedutil`；守护进程调 min/max 限制，不替换 governor |
| Cgroups | systemd 管理的 cgroup v2 |
| 输入 | Linux evdev 触摸屏设备 |

其他 ARM64 设备需要提供匹配的 JSON 配置。如果检测到的 CPU 拓扑和 power model 不一致，自动 CPU 频率控制会被关闭，不会猜测写入。

## 构建

### 依赖

Debian/Ubuntu:

```bash
sudo apt install build-essential cmake pkg-config libjson-c-dev \
    libglib2.0-dev libdbus-1-dev libpolkit-gobject-1-dev libsystemd-dev \
    libgtk-4-dev libadwaita-1-dev desktop-file-utils
```

只构建守护进程和 CLI：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_GUI=OFF
cmake --build build -j "$(nproc)"
ctest --test-dir build --output-on-failure
```

包含 GUI 的完整构建：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j "$(nproc)"
ctest --test-dir build --output-on-failure
```

## 安装

从 CMake 构建目录安装：

```bash
sudo cmake --install build
sudo systemctl daemon-reload
sudo systemctl enable --now uperf-linux.service
```

构建并安装 Debian 包：

```bash
cmake --build build --target package
sudo dpkg -i build/uperf-linux-0.1.0-arm64.deb
sudo systemctl enable --now uperf-linux.service
```

安装后的守护进程读取 `/etc/uperf-linux/config.json` 和 `/etc/uperf-linux/perapp_powermode`。

## 命令行

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

`set-freq` 中，cluster `-1` 表示 GPU，`0..2` 表示当前配置里的 CPU 簇，`3` 表示所有 CPU 簇。频率填 `0` 会释放手动覆盖。

`uperfctl` 的退出码是稳定接口：`0` 成功，`1` 用法错误，`2` 客户端参数非法，`3` 守护进程不可用，`4` 守护进程拒绝请求。

## 配置

默认配置是 `config/sm8550.json`，安装后的运行配置是 `/etc/uperf-linux/config.json`。

### Schema

`meta.schemaVersion` 可选。缺失时按 legacy schema `0` 处理；高于当前 `CONFIG_SCHEMA_VERSION` 的配置会被拒绝加载。

### CPU Power Model

每个 CPU 簇当前识别这些字段：

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

可以用 `cpumask` 引用 `modules.sched.cpumask` 中的命名 CPU mask，也可以直接写 `cpus` 数组。当前 power model 是“相对需求到目标频率”的线性性能近似，不做功耗预算。`sweetFreq` 只在 preset 开启 `cpu.limitEfficiency` 时作为频率上限使用。

### Sysfs

CPU cpufreq 路径会自动发现。`modules.sysfs.knob` 当前用于 GPU 频率路径：

```json
"knob": {
  "gpuMaxFreq": "/sys/class/devfreq/3d00000.gpu/max_freq",
  "gpuMinFreq": "/sys/class/devfreq/3d00000.gpu/min_freq"
}
```

### Presets

preset 当前会读取这些运行时调参：

- `cpu.margin`
- `cpu.burst`
- `cpu.limitEfficiency`
- `cpu.baseSampleTime`

示例：

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

### 调度和 cgroup

`modules.sched` 定义 CPU mask、亲和性 profile、优先级 profile，以及进程/线程匹配规则。`modules.cgroup` 把匹配到的 workload 映射到 cgroup class，设置 CPU mask、`cpu.weight` 和 uclamp 限制。守护进程会周期性根据活跃进程重新对账，并在 workload 不再受管理时恢复原始线程策略。

## 测试

CMake 测试覆盖配置解析、模式选择、状态机转换、频率限制计算、恢复快照、sysfs 故障注入、守护进程重载/恢复、D-Bus 接口漂移、调度规则、cgroup 对账和硬件探测辅助逻辑。

```bash
ctest --test-dir build --output-on-failure
```

硬件验证脚本：

```bash
tools/validate-frequency-hardware.sh
```

写入读回验证会临时改变 CPU/GPU 频率上下限，只应在你确认硬件状态允许时运行。


## 许可证

本项目使用 GPL-3.0-or-later。详见 [LICENSE](LICENSE)。
