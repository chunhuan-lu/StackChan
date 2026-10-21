# Backlog

[English](backlog.md) | **中文**

状态说明：`todo`（待做）· `decide`（待决策）· `doing`（进行中）· `done`（已完成）· `dropped`（已放弃）。最后更新：2026-10-21。

## 已完成

| 编号 | 事项 | 说明 |
|---|---|---|
| B-000 | 工具链、备份与仓库搭建 | ESP-IDF v5.5.4；未修改的 v1.5.1 和 v1.6.0 都编译通过；完整备份了运行官方 1.6.0 的设备的 16 MB flash；fork 与 git 工作流已配置。详见[开发日志 2026-10-20](devlog/2026-10-20.zh-CN.md)。 |

## 接下来

| 编号 | 事项 | 状态 | 说明 |
|---|---|---|---|
| B-001 | 自定义唤醒词"起来干活"（qi lai gan huo） | doing | 已决定：用 MultiNet `mn7_cn`，并把资源分区扩大到 6080 KB。已在 `feat/wake-word` 分支实现；第一次真机测试能正常唤醒（置信度 0.32 / 0.34，阈值 0.2）。命中率、反向测试、误唤醒测试待做。见 [REQ-001](requirements/REQ-001-custom-wake-word.zh-CN.md)。 |
| B-002 | 关闭小智自动 OTA 和服务器推送 OTA | doing | 都在 fork 补丁里，在 `sdkconfig.defaults.sc` 里开启（Kconfig 默认关闭，即官方行为）：`SC_DISABLE_AUTO_OTA`（发现新版本只记日志）和 `SC_DISABLE_REMOTE_OTA`（不注册 MCP 工具 `self.upgrade_firmware` / `self.assets.set_download_url`，忽略推送过来的资源下载地址）。「设置 → 关于」里的手动更新仍然可用。自动 OTA 的代码路径还没被触发（目前没有更新的官方版本）。 |
| B-003 | `fetch_repos.py` 支持一个仓库打多个补丁 | done | `repos.json` 里新增 `extra_patches`；重复运行不会出错；补丁打不上时报错，不再静默跳过；新增 `tools/xiaozhi_fork_patch.py`。已在电脑上测试（已打过补丁 / 源码有改动 / 干净源码三种情况）。 |
| B-004 | Wi-Fi 吞吐与延迟 | todo | 下载和局域网控制期间关闭省电模式；把 Wi-Fi/lwIP 缓冲区放进 PSRAM（`SPIRAM_TRY_ALLOCATE_WIFI_LWIP`），并调大接收缓冲区、TCP 窗口和 UDP 收包队列。 |
| B-005 | 不要在睡眠时持有 LVGL 锁 | todo | 小智忙碌时，`_stackchan_update_task` 会拿着锁执行 `vTaskDelay(100)`（`hal.cpp`）。 |
| B-006 | 恢复热点配网 | todo | 1.6 只允许通过 App（蓝牙）配网，而自编译固件过不了 App 的握手。目前的备用办法：用 nRF Connect 通过蓝牙发送 `setWifi`。 |
| B-007 | 检查音频任务调度 | todo | 只在说话卡顿复发时处理：`opus_codec`（优先级 2）低于 LVGL 和 stackchan 任务（优先级 3）。 |
| B-023 | Claude Code 像素小伙伴皮套 | doing | [REQ-007](requirements/REQ-007-claude-suit.zh-CN.md)，在 `feat/claude-suit` 分支：Claude Code 干活时机器人穿上橙色像素小伙伴皮套（9 种姿态，干活时冒汗），穿上时皮套掉下来、在里面扭一扭，脱下时用带轮廓的长手臂自己拉开拉链；头跟着点头、后仰、摇头（设置 `claude_suit`、`suit_off_min`、`suit_head`、`suit_head_spd`）。已烧录，转场待真机观察。 |
| B-022 | USB 完全控制：烧录、重启、启动 AI Agent | doing | `tools/sc-agent/sc-flash.ps1`：通过 USB-Serial/JTAG 由 esptool 进入下载模式、烧录、等待新的 boot id（卡在 ROM 下载模式时看门狗复位），USB 通道命令 `agent`（协议 v1.2）让主菜单打开 AI Agent。2026-10-21 真机验证通过（复位后 15 秒 AI Agent 就绪）。见 [REQ-003](requirements/REQ-003-claude-code-status.zh-CN.md)。 |
| B-021 | 设置面板与语音设置 | doing | [REQ-006](requirements/REQ-006-settings.zh-CN.md)，在 `feat/settings` 分支：Home 按钮旁的齿轮打开设置面板，共 15 项（活跃程度、作息、光照阈值、睡眠屏幕、灯光亮度、特殊表情间隔，以及表情 / 点击唤醒 / Claude / 插 USB 不待机 开关），保存在 NVS `sc_cfg`，立即生效；MCP 工具 `self.robot.get_settings` / `set_setting`。Kconfig 默认值改为官方行为，本 fork 的选择放在 `sdkconfig.defaults.sc`。等待真机验证。 |
| B-020 | 更生动的表情 | doing | [REQ-005](requirements/REQ-005-faces.zh-CN.md)：FaceArt 画出 16 个矢量表情（温和微笑作为基础脸，每 4–10 分钟偶尔换一个特殊表情，Claude Code 相关表情由会话状态和事件驱动）。等待真机验证。 |
| B-019 | 待机、供电与触摸改进 | doing | [REQ-004](requirements/REQ-004-standby-power-touch.zh-CN.md)，分支 `fix/standby-power`：睡眠亮度分暗/亮两档（5 / 45）并渐变、睡着时点屏幕叫醒、过滤滑动、Home 重启前提示、按 VBUS 判断外部供电并记录供电日志（含 INA226 电池电流）、插 USB 时不进省电、记录复位原因、崩溃转储。等待真机验证。 |
| B-018 | 官方 bug：做完网络测试再进 AI Agent → 重启进入 App 配置页 | todo | 2026-10-21 遇到（[开发日志](devlog/2026-10-21.zh-CN.md)）。「设置 → 网络测试」会直接通过 `WifiManager` 打开并连上 Wi-Fi，退出后不关闭。不重启就进 AI Agent 时，`WifiBoard::TryWifiConnect()` 等待一个早已发生过的 `Connected` 事件 → 60 秒超时 → `StartWifiConfigMode()` → `requestAppSetup()` → 重启进入 APP SETUP 页面。不会丢失任何数据（绑定、`is_configed`、Wi-Fi 密码都在），复位即可恢复。修复思路：在 `TryWifiConnect()` 里把已连接的情况直接当作连接成功（fork 补丁），或者在测试页面关闭时关掉 Wi-Fi。临时办法：做完网络测试先重启。可以考虑给官方提 issue。同一个超时的第二种触发方式（2026-10-21 16:06）：DHCP 要好几分钟的网络（8 秒就关联上，60 秒还没拿到 IP；同一个路由器早些时候用了 3 分钟才给 IP）也会进 App 配网页；可选方案：已关联时多等一会儿。 |

## 以后

| 编号 | 事项 | 状态 | 说明 |
|---|---|---|---|
| B-010 | Claude Code 状态机器人 | doing | 第一期（USB 通道、Claude 层、`tools/sc-agent`、hook）已在 `feat/claude-link` 分支实现，等待真机验证。[REQ-003](requirements/REQ-003-claude-code-status.zh-CN.md)：Claude Code hook → sc-agent（Windows 上的 Python，WSL 里的会话也走它）→ 通过 USB 通道连机器人；第二期经 NAS 上的 Mosquitto 中转（静态 IP、自签 CA）；第三期通过小智 MCP 接入点语音问进度。按项目或会话选择接入；永远不能批准授权请求。 |
| B-011 | 待机行为（第一批） | doing | 由心情驱动的行为调度器：08–23 点作息、天黑睡觉、摸头/拿起/倾斜反应、只动屏幕的小动作、心情灯；静音且安静。已在 `feat/idle-behavior` 分支实现；2026-10-20 第一次真机测试，之后做了调整（触摸静默期、照度阈值、墨尔本时区）。其余验收测试待做。见 [REQ-002](requirements/REQ-002-idle-behaviour.zh-CN.md)。 |
| B-017 | 待机行为（第二批） | todo | 敲桌子检测、跟着音乐点头（需要从小智拿麦克风音量）、倾斜时头部保持水平（IMU 坐标轴已标定：重力在 +Y，俯仰绕 X 轴）、内心状态持久化。 |
| B-012 | 设备端人脸检测与跟踪 | doing | [REQ-008](requirements/REQ-008-face-tracking.zh-CN.md)，在 `feat/face-track` 分支：待机时在 core 1 运行 ESP-DL `human_face_detect` 0.4（esp-dl 内核改名，避开 esp-sr 里的同名版本）；麦克风缓冲，检测不再让唤醒词丢音；眼睛和头跟着人脸、打招呼、防跑偏；设置 `face_track`、`face_head`、`face_box`、`face_probe`，另加 `sleep`；USB 链路 `set` / `head`（协议 v1.3）。检测已在真机验证，跟踪已烧录，待用真人脸观察。 |
| B-013 | 识别桌面物品 | todo | NAS 上用 YOLO（OpenVINO）加反应规则；只在遇到新的或认不出的物品时才调用小型多模态模型。 |
| B-014 | 更多开始和结束对话的方式 | todo | 摸头、轻拍或摇晃（IMU）、语音退出工具、离线命令词（依赖 B-001）。 |
| B-015 | 评估小智 v2.5.x | todo | 参考 artrobin-cn 的分支 `public/work-secretary-v2.5`；该分支为了让唤醒词能用而关闭了回声消除，这个问题要先解决。 |
| B-016 | 自建小智服务器 | todo | 可选；在 NAS 上用 Docker 部署 `xinnan-tech/xiaozhi-esp32-server`。 |
