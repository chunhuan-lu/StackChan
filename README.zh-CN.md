# StackChan — AI Agent 分支

[English](README.md) | **中文**

这是 [m5stack/StackChan](https://github.com/m5stack/StackChan) 的个人分支：机器人固件基于官方 **v1.6.0**
（xiaozhi-esp32 v2.2.4，ESP-IDF v5.5.4），围绕 **AI Agent**（小智）模式做了改造。这不是 M5Stack 的官方版本。官方
README（硬件、App、服务器、致谢）保留为 [README.upstream.md](README.upstream.md)；app、remote、server 目录没有改动。

fork 的功能都由 `SC_*` 开头的 Kconfig 开关控制，开关默认值保持官方行为；本 fork 在
[`firmware/sdkconfig.defaults.sc`](firmware/sdkconfig.defaults.sc) 里把它们打开。

## 烧录前必读

以下改动会影响和官方固件的兼容、烧录方式或其他功能。

| 改动 | 影响 | 怎么办 |
|---|---|---|
| **分区表** [`firmware/partitions_sc.csv`](firmware/partitions_sc.csv)：`assets` 从 4 MB 扩大到 6080 KB（0xA00000-0xFF0000），用来放 MultiNet7 唤醒词模型；`coredump` 从 0xE00000 挪到 flash 最后 64 KB（0xFF0000）。`nvs`、`otadata`、`phy_init`、`ota_0`、`ota_1` 的位置和大小都不变。 | NVS（Wi-Fi、绑定、设置）和两个应用分区都在原位。只更新应用（OTA，或者只写入应用镜像）不会改分区表和 assets：fork 的应用配官方分区，没有 MultiNet7 模型（唤醒词不可用）；官方应用配 fork 分区，找不到官方的唤醒词模型。 | 用 `idf.py -p COMx flash` 烧录（会写入 bootloader、分区表、应用和 assets，保留 NVS）。要换回官方固件，需要完整烧录官方版本（见[换回官方固件](#换回官方固件)）。 |
| **应用分区已用约 95 %**（每个 0x4F0000 = 4.94 MB；带人脸跟踪的固件约 4.7 MB）。 | 其中人脸跟踪（默认关）约占 400 KB，剩余空间很少。 | 再加新功能前要先决定分区方案（比如只保留一个应用分区）。 |
| **唤醒词**：用 MultiNet7 识别中文"起来干活"（*qi lai gan huo*），替代"Hi, Stack Chan"（WakeNet）；阈值 20。 | 它自己就占 core 0 约 75 %。换别的唤醒词需要改 sdkconfig。 | `sdkconfig.defaults.sc` 里的 `CONFIG_CUSTOM_WAKE_WORD*`（[REQ-001](docs/requirements/REQ-001-custom-wake-word.zh-CN.md)）。 |
| **OTA**：关闭了自动固件升级，以及服务器推送固件 / 资源（`SC_DISABLE_AUTO_OTA`、`SC_DISABLE_REMOTE_OTA`）。 | 小智服务器不能替换本固件或它的资源。"设置 → 关于"里的手动更新仍然可用，但它会装上官方固件，覆盖掉本 fork。 | 除非想换回官方，否则不要用手动更新。 |
| **离线优先**（`SC_OFFLINE_FIRST`）：AI Agent 不联网也能进入待机；版本检查和服务器连接在后台进行。 | 没有 Wi-Fi 时不再重启进 App 配网；Wi-Fi 在主菜单的 App 配网里设置（或用 [`tools/sc-wifi`](tools/sc-wifi)）。没有"激活中"和版本号界面。开机后 20 秒内（或者联网完成之前）唤醒词还没启用。没网时发起对话会显示 "No network. Please connect to the internet first."。 | [REQ-009](docs/requirements/REQ-009-offline-first.zh-CN.md)。 |
| **默认时区**：墨尔本（`AEST-10AEDT,M10.1.0,M4.1.0/3`）。 | 作息时间表（23:00-08:00 睡觉）按它计算。 | 在"设置 → 系统"里选自己的时区，或者修改 `CONFIG_SC_DEFAULT_TIMEZONE`。 |
| **AI Agent 待机**由空闲调度器负责，替代官方的随机转头。 | 夜里和黑暗中会睡觉，睡着时点屏幕是叫醒它（而不是开始对话），LED 有心情灯，头部动作由调度器决定。 | 设置面板（Home 旁边的齿轮）；[REQ-002](docs/requirements/REQ-002-idle-behaviour.zh-CN.md)。 |
| **USB 供电**：插着 USB 电源时不进入省电睡眠，也不自动关机。 | 插着电时屏幕一直亮着。 | 设置项 "No standby on USB"；[REQ-004](docs/requirements/REQ-004-standby-power-touch.zh-CN.md)。 |
| **USB-Serial/JTAG** 除了日志，还承载 Claude Code 链路（带帧头的 JSON 行）。 | 电脑上运行 `sc-agent` 时它会占用 COM 口，直接 `idf.py flash` 会提示端口被占用。 | 用 [`tools/sc-agent/sc-idf.ps1`](tools/sc-agent/README.zh-CN.md) 或 `sc-flash.ps1` 烧录。 |
| **麦克风通路**：CoreS3 的 codec 经过 1 秒的 PSRAM 缓冲读取（`SC_MIC_BUFFER_MS`）。 | 其他任务拖慢唤醒词时也不会丢音，但唤醒词最多可能晚约 0.3 秒响应。 | [REQ-008](docs/requirements/REQ-008-face-tracking.zh-CN.md)。 |
| **内部 RAM 很紧**：最低约 6.6 KB（加载唤醒词模型的时候）。 | 申请内部 RAM 的代码可能失败。 | 尽量在 PSRAM 里申请内存；可以参考开发日志里的内存日志做法来测量。 |
| **组件**：新增 esp-dl 3.2.4 和 human_face_detect 0.4.0；编译时给 esp-dl 的 TIE728 卷积内核改名（[`firmware/CMakeLists.txt`](firmware/CMakeLists.txt)），避免和 esp-sr 里的旧版同名内核冲突。`dependencies.lock` 里 esp_codec_dev 从 1.5.4 升到 1.5.11，esp_sccb_intf 从 0.0.8 升到 1.0.0，dl_fft 从 0.3.1 升到 0.8.0，i2c_bus、uart-uhci、iot_usbh_cdc 也有升级。 | 音频 codec 和摄像头驱动比官方 1.6.0 新（已在真机测试）。 | 编译时保留这个锁文件。 |
| **小智的改动**放在单独的补丁 [`firmware/patches/xiaozhi-esp32-fork.patch`](firmware/patches/xiaozhi-esp32-fork.patch) 里（application、设备状态机、背光、MCP 工具、资源打包脚本），由 `fetch_repos.py` 在官方补丁之后打上。 | 升级小智版本时需要重新整理这个补丁。 | `python firmware/tools/xiaozhi_fork_patch.py` 会根据 `firmware/xiaozhi-esp32` 的改动重新生成。 |
| **设置**存放在 NVS 命名空间 `sc_cfg`（31 项），也开放给语音助手（`self.robot.get_settings`、`self.robot.set_setting`）。 | 设置的修改标记是 32 位：再加 1 项就得扩容。 | [REQ-006](docs/requirements/REQ-006-settings.zh-CN.md)。 |

## 功能

| 功能 | 文档 |
|---|---|
| 自定义唤醒词"起来干活"（MultiNet7） | [REQ-001](docs/requirements/REQ-001-custom-wake-word.zh-CN.md) |
| 空闲调度器：作息时间、黑暗中睡觉、摸头、拿起和倾斜的反应、心情灯 | [REQ-002](docs/requirements/REQ-002-idle-behaviour.zh-CN.md) |
| 通过 USB 在机器人上显示 Claude Code 状态：会话灯、表情、事件反应；电脑端程序和 hooks | [REQ-003](docs/requirements/REQ-003-claude-code-status.zh-CN.md) |
| 待机、电源和触摸修复 | [REQ-004](docs/requirements/REQ-004-standby-power-touch.zh-CN.md) |
| 画出来的表情（微笑、特殊表情、Claude Code 表情） | [REQ-005](docs/requirements/REQ-005-faces.zh-CN.md) |
| 设置面板（Home 旁边的齿轮）和语音设置 | [REQ-006](docs/requirements/REQ-006-settings.zh-CN.md) |
| Claude Code 工作时的像素小伙伴皮套，带转场和头部动作 | [REQ-007](docs/requirements/REQ-007-claude-suit.zh-CN.md) |
| 人脸跟踪：眼睛和头跟着你、打招呼、丢失后寻找（默认关） | [REQ-008](docs/requirements/REQ-008-face-tracking.zh-CN.md) |
| 离线优先：只有对话才需要网络 | [REQ-009](docs/requirements/REQ-009-offline-first.zh-CN.md) |
| USB 完全控制：烧录、重启、启动 AI Agent、修改设置、转头 | [REQ-003](docs/requirements/REQ-003-claude-code-status.zh-CN.md)、[sc-agent](tools/sc-agent/README.zh-CN.md) |

## 编译和烧录

```bash
cd firmware
python fetch_repos.py            # 拉取官方依赖仓库，再依次打上官方和本 fork 的小智补丁
idf.py build                     # ESP-IDF v5.5.4
idf.py -p COMx flash             # 保留 NVS：Wi-Fi、绑定和设置都还在
```

- 先应用 `sdkconfig.defaults`（官方），再应用 `sdkconfig.defaults.sc`（本 fork），如果有
  `sdkconfig.defaults.local` 最后再应用它（不进 git，放自己的覆盖配置）。
- 默认值只在生成 `sdkconfig` 时生效：改了 defaults 文件或 `main/Kconfig.projbuild` 之后，要执行
  `idf.py reconfigure`（或者删掉 `firmware/sdkconfig`）。
- 人脸跟踪：在 `sdkconfig.defaults.local` 里加 `CONFIG_SC_FACE_PROBE=y`（会再增加约 400 KB，注意上面的分区说明）。
- 不要提交 `sdkconfig` 和 `sdkconfig.defaults.local`。没有完整备份时不要 `erase-flash`：NVS 里有 Wi-Fi 设置和设备绑定。

## Kconfig 开关

在 `idf.py menuconfig` 的 "StackChan fork options" 菜单里。"官方"是 Kconfig 默认值，"fork"是
`sdkconfig.defaults.sc` 里的值。

| 开关 | 官方 | fork | 作用 |
|---|---|---|---|
| `SC_OFFLINE_FIRST` | n | y | 不联网也能用 AI Agent（[REQ-009](docs/requirements/REQ-009-offline-first.zh-CN.md)） |
| `SC_DISABLE_AUTO_OTA` | n | y | 不自动升级固件 |
| `SC_DISABLE_REMOTE_OTA` | n | y | 不接受服务器推送的固件 / 资源 |
| `SC_DEFAULT_TIMEZONE` | GMT0 | 墨尔本 | 在"设置 → 系统"里选择时区之前使用 |
| `SC_IDLE_DIRECTOR` | n | y | 空闲调度器（[REQ-002](docs/requirements/REQ-002-idle-behaviour.zh-CN.md)） |
| `SC_IDLE_FACES` | y | y | 画出来的表情（[REQ-005](docs/requirements/REQ-005-faces.zh-CN.md)） |
| `SC_EXT_POWER_NO_STANDBY` | n | y | USB 供电时不待机（[REQ-004](docs/requirements/REQ-004-standby-power-touch.zh-CN.md)） |
| `SC_TOUCH_FIXES` | n | y | AI Agent 触摸修复 |
| `SC_USB_LINK` | n | y | 通过 USB 的 Claude Code 链路（[REQ-003](docs/requirements/REQ-003-claude-code-status.zh-CN.md)） |
| `SC_CLAUDE_SUIT` | y | y | 像素小伙伴皮套（[REQ-007](docs/requirements/REQ-007-claude-suit.zh-CN.md)） |
| `SC_SETTINGS_PANEL` | n | y | 设置面板和语音设置（[REQ-006](docs/requirements/REQ-006-settings.zh-CN.md)） |
| `SC_MIC_BUFFER_MS` | 0 | 1000 | 麦克风缓冲 |
| `SC_FACE_PROBE` | n | n | 人脸跟踪（[REQ-008](docs/requirements/REQ-008-face-tracking.zh-CN.md)） |

作息时间、各种阈值、颜色和速度（`SC_IDLE_*`、`SC_CLAUDE_SUIT_*`）是设置面板的默认值；完整列表和范围见
[REQ-006](docs/requirements/REQ-006-settings.zh-CN.md)。

## 工具

- [`tools/sc-agent`](tools/sc-agent/README.zh-CN.md)：电脑端程序（Windows，Python）。把 Claude Code 的 hooks 通过
  USB 传给机器人（会话灯、表情、皮套），还有状态栏脚本、`sc-flash.ps1`（一步完成烧录、重启、启动 AI Agent）、
  `sc-idf.ps1`（先释放端口再执行 idf.py），以及一个只监听 127.0.0.1 的 HTTP 接口（`/status`、`/agent`、`/setting`、
  `/head` 等）。
- [`tools/sc-wifi`](tools/sc-wifi)：通过蓝牙添加 Wi-Fi 网络（自编译的固件过不了 App 的配对握手）。

## 文档

- [需求文档](docs/requirements) REQ-001 到 REQ-009，每份都有中英文。
- [待办](docs/backlog.zh-CN.md)和[开发日志](docs/devlog)（记录真机上测试了什么、怎么测的）。
- [开发约定](docs/CONVENTIONS.zh-CN.md)：分支、提交、Kconfig、文档。

## 换回官方固件

完整烧录官方版本（比如用 M5Burner），这样分区表和资源也会一并写入；只更新应用的话，本 fork 的分区表和资源会留下来。
NVS 位置不变，所以 Wi-Fi 和绑定一般都还在；fork 的设置（`sc_cfg`）会被直接忽略。操作前先做一次完整的 flash 备份。

## 许可和致谢

固件采用 MIT 许可（见 [firmware/LICENSE](firmware/LICENSE)，© M5Stack）；本 fork 的改动沿用同样的许可。感谢 M5Stack、
StackChan 社区和 xiaozhi-esp32 项目；详见 [README.upstream.md](README.upstream.md)。
