# REQ-004 待机、供电与触摸改进

[English](REQ-004-standby-power-touch.md) | **中文**

状态：已在 `fix/standby-power` 分支实现（从 `feat/idle-behavior` 拉出），**尚未真机验证** · 对应 backlog：B-019

## 背景

维护者在 2026-10-21 用了一晚 REQ-001/REQ-002 的固件后反馈：

1. 机器人睡觉时屏幕太暗。
2. 插着 USB、并且确认不是靠电池供电时，不要进入待机。
3. 长时间待机后设备重启了，日志里有 Wi-Fi 重连和加密连接报错，看起来像崩溃。

## 调查结论（看源码和原理图，并经过第二轮复核）

- **重启**：不是崩溃。`[HAL] warm reboot request to app index: 0` 只会由 AI Agent 的 Home 按钮触发
  （从屏幕底边上滑，再点 Home）。离开 AI Agent 一定会重启，因为小智没法在运行中关闭；`esp_restart()` 会先关
  Wi-Fi，于是出现重连和 TLS/MQTT 报错。没有崩溃标志，复位原因是 0xc（软件复位）。维护者确认当时点了 Home。
  顺带发现：待机行为不响应屏幕触摸，所以点一下睡着的暗脸会开始一次对话；从底边上滑也可能触发开始/结束对话
  （没有可滚动对象时，LVGL 在拖动后也会发出 CLICKED）。
- **背光**：屏幕暗是待机行为的夜间睡眠。`CONFIG_SC_IDLE_SLEEP_BACKLIGHT=5` 已经是最暗的可见档：AXP2101 用 DLDO1
  给背光供电，只有 9 档、每档 100 mV，设置值 1 到 12 都是 2.5 V。把 5 改成 10 不会有任何变化。
- **供电**：
  - 插着 USB 时，官方的省电计时器（5 分钟后瞌睡脸、10 分钟后关机）本来就是关闭的，只要 PMIC 没有报告电池在放电。
    维护者看到的"待机"其实是夜间睡眠。
  - 固件里的"外部供电"标志实际含义是"电池没有在放电"，从来没读过 VBUS。
  - 硬件（`docs/schematics` 里的原理图）：**舵机永远直接由电池供电**，经过机身里的升压电路，插着 USB 也一样。
    USB 给 CoreS3 主板供电，并给电池充电（最大 700 mA）。所以"完全不靠电池"在这台机器上做不到。
  - 机身里有一颗 INA226（0x41），接在电池回路的 10 mΩ 采样电阻上，能测到包括舵机在内的真实电池电流。固件原来没有用它。

## 维护者的决定（2026-10-21）

- 插着 USB 时：**照常保留夜间作息**（23:00–08:00 以及天黑时睡觉），但绝不进入省电睡眠、也不自动关机。
- 开灯时的睡眠亮度：**45**（2.8 V）。房间黑时不变（5，最暗档）。
- Home：**不需要长按确认**；重启前给出提示。

## 改动

| 方面 | 改动 |
|---|---|
| 睡眠亮度 | 分两档：`SC_IDLE_SLEEP_BACKLIGHT`（房间黑时，5）和新的 `SC_IDLE_SLEEP_BACKLIGHT_LIT`（45，环境光高于 `SC_IDLE_BRIGHT_LUX` 时）；低于 `SC_IDLE_DARK_LUX` 持续 10 秒后切回暗档。不会超过平时的亮度。每 `SC_IDLE_BACKLIGHT_FADE_MS`（300 ms）渐变一档 100 mV；醒来时立即恢复。日志 `sleep backlight N (x lux)`。 |
| 睡着时点屏幕 | 把它叫醒，而不是开始对话（夜里过 60 秒缓冲后会再次睡着）。唤醒词不受影响。 |
| 滑动 | 按下点在底部 20 像素内、或移动超过 30 像素的触摸，不再触发开始/结束对话（日志 `Screen swipe ignored`）。屏幕点击会记日志。 |
| Home 按钮 | 按钮滑入、滑出过程中的点击被忽略。返回主页会重启设备时（AI Agent 和大多数应用；「设置」只有在改了需要重启的项目后才会），由 LVGL 任务绘制全屏的"Going home..."，300 毫秒后再返回主页。遮罩归 Home 指示器所有，随它一起删除。手势和点击都记日志。 |
| 触摸芯片 | FT6336 报告超过 2 个触点、或坐标超出 320x240 的采样按读取错误处理丢弃（保持上一次的状态）；连续 5 次异常才报告手指抬起。限频警告。 |
| 复位原因 | 开机时记录（`reset reason: N`：3 软件复位，4 崩溃，5–7 看门狗，9 欠压）。 |
| 崩溃转储 | 写入已有的 64 KB `coredump` 分区，用 `idf.py coredump-info` 读取。关闭崩溃转储的日志字符串（否则要多占约 5 KB 内部内存）。 |
| 外部供电 | `SC_EXT_POWER_NO_STANDBY`：外部供电改用 AXP2101 的 VBUS 有效标志（REG00[5]），并做防抖（接上 2 秒、拔掉 5 秒才算数）。只要 USB 有电，即使电池在帮忙供电，省电计时器也保持关闭。AI Agent 设置里的"充电时允许自动关机"仍然有效。 |
| 供电日志 | 开机时、每次变化时、以及每 10 分钟：`Power: USB/battery (VBUS mV), PMIC battery charging/idle/discharging, %, cell mV mA -> power save on/off`。插着 USB 但 PMIC 报告放电时，每分钟警告一次。电池电压和电流来自 INA226（电流正负方向待真机确认）。 |
| 状态栏 | 插着 USB 时电池图标显示闪电；只有在充电时电量条才是绿色。 |
| 退出省电 | 用电池时，官方的瞌睡脸保持睡眠亮度，不再跳回全亮；省电结束时（比如插上 USB）立即重启待机行为，不用等到下一次对话。 |
| 背光（小智 fork 补丁） | `Backlight::SetBrightness` 在有待执行的渐变时也会改目标值：原来同一个任务里连续两次调用（先恢复、再调暗），第二次会被忽略。 |

## 配置项（`StackChan fork options` 菜单）

| 配置项 | 默认值 |
|---|---|
| `SC_IDLE_SLEEP_BACKLIGHT` | 5（房间黑时） |
| `SC_IDLE_SLEEP_BACKLIGHT_LIT` | 45（开着灯时） |
| `SC_IDLE_BACKLIGHT_FADE_MS` | 300 |
| `SC_EXT_POWER_NO_STANDBY` | y |
| `SC_TOUCH_FIXES` | y |

## 文件

| 文件 | 改动 |
|---|---|
| `main/stackchan/sc/idle_director.*` | 分档睡眠亮度和渐变、点屏幕叫醒 |
| `main/hal/board/stackchan_display.*` | 过滤滑动、记录点击、点屏幕叫醒的入口、重写 `SetPowerSaveMode` |
| `main/apps/common/home_indicator/home_indicator.*`、`main/apps/app_setup/app_setup.cpp` | 日志、滑动过程中忽略点击、只在会重启时显示"Going home..." |
| `patches/xiaozhi-esp32-fork.patch` | `boards/common/backlight.cc`：有待执行的渐变时也改目标值 |
| `main/hal/board/stackchan.cc` | 读 VBUS、防抖的外部供电判断、供电日志、FT6336 采样检查 |
| `main/hal/sc/ina226.*`、`main/hal/sc/hal_power.cpp` | 新增：INA226 读取、`Hal::isOnExternalPower()` |
| `main/hal/hal.h`、`main/hal/board/hal_bridge.h` | `isOnExternalPower()` |
| `main/apps/common/status_bar/status_bar.cpp` | 插着 USB 时显示闪电 |
| `main/main.cpp` | 记录复位原因 |
| `main/Kconfig.projbuild`、`sdkconfig.defaults.sc` | 上面的配置项、崩溃转储 |

## 已知限制

- 插着 USB 时舵机也用电池；频繁转头会让电池在接近满电的状态下反复充放。
- INA226 电流的正负方向还没确认，日志里打印的是带符号的原始值。
- 背光的光可能漏进前面板玻璃后面的环境光传感器。如果在亮度 45 时漏光让读数一直高于 `SC_IDLE_DARK_LUX`，
  房间关灯后睡着的机器人就不会变暗（在验收测试 1 里检查）。
- 崩溃转储要预留约 1.8 KB 的内部内存栈；对话结束后要检查剩余内部内存。

## 验收标准（真机）

1. 夜里开着灯：睡着的脸渐变到 45（`sleep backlight 45`）；关灯后约 10–20 秒变暗到 5；再开灯回到 45。
2. 睡着时点屏幕 → 醒来（`screen tap while asleep`），不开始对话；夜里约 60 秒后再次睡着。
3. 醒着时点屏幕仍然能开始/结束对话；从底边上滑只会出现 Home 按钮（`Screen swipe ignored`）。
4. 点 Home → 能看到"Going home..."，然后重启；下次开机日志是 `reset reason: 3`。
5. 用 CoreS3 的 USB-C 和机身的 USB-C、接电脑和接充电器，开机日志都显示 `Power: USB (VBUS ~5000 mV)`；
   拔掉后约 5 秒内变成 `Power: battery`，插回后约 2 秒内变成 `Power: USB`；状态栏在插着 USB 时显示闪电。
6. 用电池：待机 5 分钟后瞌睡脸保持暗；插上 USB 后待机行为恢复（`Power save ended: restart idle director`）。
7. 记录 INA226 的电池电流：静止充电时、以及舵机转动时（确认正负方向）。
8. 没有退化：唤醒词、对话、待机行为、对话后的剩余内部内存。
