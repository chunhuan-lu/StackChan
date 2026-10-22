# REQ-009 离线优先

[English](REQ-009-offline-first.md) | **中文**

状态：在 `feat/face-track` 上实现，2026-10-22 已烧录；联网时的启动已在真机验证，没网时的表现还要实际试一下 · 待办：B-024

## 目标

维护者几乎只用不需要联网的功能（表情、人脸跟踪、Claude Code 状态、设置），语音助手很少用。AI Agent 模式不应该依赖
网络：只有对话需要网络，这时用一句简短的英文提示说明（"No network. Please connect to the internet first."）。其他的都
不需要（不提示去 App、不进配网流程）：维护者知道怎么配置 Wi-Fi。

## 之前

- fork 的所有功能（空闲调度器、表情、人脸跟踪、设置齿轮、脸上的 Claude Code 状态）都要等 xiaozhi 进入待机才启动，而
  进入待机之前必须先连上 Wi-Fi、完成 OTA 版本检查并连上服务器。
- 没保存 Wi-Fi：打开 AI Agent 1.5 秒后重启进 App 配网。保存的 Wi-Fi 找不到：60 秒后重启进 App 配网。有 Wi-Fi 没有外网：
  停在"激活中"的报错表情，反复重试最长约 85 分钟，除了重启没有别的办法。
- 待机时和服务器断开，报错表情会一直留在屏幕上，直到服务器恢复。
- 作息时间表只认网络对时，虽然 RTC 一直在走时。

## 行为（`CONFIG_SC_OFFLINE_FIRST`，Kconfig 默认关 = 官方行为，fork 打开）

- **立即待机**：先加载资源（字体和唤醒词模型），xiaozhi 直接从 Starting 进入 Idle（开机约 5 秒，播放就绪提示音），不等 Wi-Fi。
- **网络在后台**：Wi-Fi 连上后，版本检查和服务器连接在一个激活任务里进行，不改变状态、不显示任何东西、不播提示音。版本
  检查失败时试两次（间隔 5 秒），之后服务器连接使用上次成功检查时保存的配置。服务器要求绑定时仍然进入 App 配网，和官方一致。
- **不再重启进配网**：60 秒超时后继续寻找已保存的网络（扫描间隔逐渐变长）；没保存网络就保持离线。Wi-Fi 在主菜单的 App
  配网里设置。
- **待机时的错误**（重连、后台启动）只写日志。
- **对话**（唤醒词、点屏幕、开始收听）：没有 Wi-Fi 时显示提示 5 秒并播放提示音，然后继续待机；有 Wi-Fi 但还没连上服务器时，
  先连接，连好后再打开对话。连不上服务器的对话也显示同样的提示，而不是报错界面。
- **时间**：系统时间在 2025 年或之后，调度器就信任它；开机时 RTC 会提供这个时间（之前由网络对过时，靠电池保持）。日志
  里会注明 "RTC" 或 "synced"。Wi-Fi 连上时重新启动 SNTP。

## 内部 RAM

联网启动一次性需要很多内部 RAM：HTTPS 版本检查峰值约 27 KB，MQTT 任务要 6 KB 栈（esp-mqtt 不能放到 PSRAM）。官方流程在
待机之前做这些；改成先待机后，唤醒词模型（约 12 KB，加载瞬间 27 KB）和人脸检测模型已经先加载了，第一次真机测试就失败了：
`mqtt_client: Error create mqtt task`，内部 RAM 最低只剩 1.3 KB。现在：

- 从开机到第一次联网完成（或 20 秒没有 Wi-Fi），以及之后每次后台联网期间，`hal_bridge::network_busy()` 为真。这期间人脸
  检测暂停并卸载模型。
- 唤醒词的第一次启动也等它（模型加载的峰值），并且在人脸检测加载模型之前进行。
- 后台联网在连接服务器之前先释放 OTA 对象（约 9 KB）。

真机测量（内部 RAM 剩余，开机以来的最低值）：

| | 官方顺序 | 先待机，第一版 | 现在 |
|---|---|---|---|
| 联网期间 | 32 KB | 1.3 KB（MQTT 任务没建起来） | 23 KB |
| 整体最低 | 约 6.2 KB（唤醒词模型加载） | 1.3 KB | 6.6 KB（唤醒词模型加载） |

## 文件

| 文件 | 改动 |
|---|---|
| `patches/xiaozhi-esp32-fork.patch` | `application.*`：启动即待机、后台激活、对话前检查、待机时不报错、推迟唤醒词、联网忙标志；`device_state_machine.cc`：Starting → Idle |
| `main/hal/board/stackchan.cc` | Wi-Fi 超时或没保存 Wi-Fi 时不再重启进 App 配网 |
| `main/hal/board/hal_bridge.h`、`stackchan_display.cc` | `show_notice()`（会自动消失的对话气泡）、`set_network_busy()` / `network_busy()` |
| `main/hal/sc/face_probe.cpp` | 联网期间暂停（并卸载模型） |
| `main/stackchan/sc/idle_director.cpp` | 信任 RTC 时间 |
| `main/Kconfig.projbuild`、`sdkconfig.defaults.sc` | `SC_OFFLINE_FIRST`（菜单 "Network, OTA and time"），fork 打开 |

## 真机测试

1. 有 Wi-Fi：AI Agent 启动约 5 秒后出现表情；再过约 10 秒，日志里出现 "Activation done" 和 "MQTT: Connected to
   endpoint"；唤醒词可用。
2. 关掉路由器（或离开信号范围）：机器人照样进入待机，表情和人脸跟踪正常，不会重启进 App 配网；说"起来干活"或点屏幕，
   显示 "No network. Please connect to the internet first." 5 秒。
3. 重新打开路由器：几分钟内机器人自己重新连上；下一次对话正常。
