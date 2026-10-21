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
| B-002 | 关闭小智自动 OTA 和服务器推送 OTA | doing | 都在 fork 补丁里，默认都开启：`SC_DISABLE_AUTO_OTA`（发现新版本只记日志）和 `SC_DISABLE_REMOTE_OTA`（不注册 MCP 工具 `self.upgrade_firmware` / `self.assets.set_download_url`，忽略推送过来的资源下载地址）。「设置 → 关于」里的手动更新仍然可用。自动 OTA 的代码路径还没被触发（目前没有更新的官方版本）。 |
| B-003 | `fetch_repos.py` 支持一个仓库打多个补丁 | done | `repos.json` 里新增 `extra_patches`；重复运行不会出错；补丁打不上时报错，不再静默跳过；新增 `tools/xiaozhi_fork_patch.py`。已在电脑上测试（已打过补丁 / 源码有改动 / 干净源码三种情况）。 |
| B-004 | Wi-Fi 吞吐与延迟 | todo | 下载和局域网控制期间关闭省电模式；把 Wi-Fi/lwIP 缓冲区放进 PSRAM（`SPIRAM_TRY_ALLOCATE_WIFI_LWIP`），并调大接收缓冲区、TCP 窗口和 UDP 收包队列。 |
| B-005 | 不要在睡眠时持有 LVGL 锁 | todo | 小智忙碌时，`_stackchan_update_task` 会拿着锁执行 `vTaskDelay(100)`（`hal.cpp`）。 |
| B-006 | 恢复热点配网 | todo | 1.6 只允许通过 App（蓝牙）配网，而自编译固件过不了 App 的握手。目前的备用办法：用 nRF Connect 通过蓝牙发送 `setWifi`。 |
| B-007 | 检查音频任务调度 | todo | 只在说话卡顿复发时处理：`opus_codec`（优先级 2）低于 LVGL 和 stackchan 任务（优先级 3）。 |
| B-020 | 更生动的表情 | doing | [REQ-005](requirements/REQ-005-faces.zh-CN.md)：FaceArt 画出 16 个矢量表情（温和微笑作为基础脸，每 4–10 分钟偶尔换一个特殊表情，Claude Code 相关表情由会话状态和事件驱动）。等待真机验证。 |
| B-019 | 待机、供电与触摸改进 | doing | [REQ-004](requirements/REQ-004-standby-power-touch.zh-CN.md)，分支 `fix/standby-power`：睡眠亮度分暗/亮两档（5 / 45）并渐变、睡着时点屏幕叫醒、过滤滑动、Home 重启前提示、按 VBUS 判断外部供电并记录供电日志（含 INA226 电池电流）、插 USB 时不进省电、记录复位原因、崩溃转储。等待真机验证。 |
| B-018 | 官方 bug：做完网络测试再进 AI Agent → 重启进入 App 配置页 | todo | 2026-10-21 遇到（[开发日志](devlog/2026-10-21.zh-CN.md)）。「设置 → 网络测试」会直接通过 `WifiManager` 打开并连上 Wi-Fi，退出后不关闭。不重启就进 AI Agent 时，`WifiBoard::TryWifiConnect()` 等待一个早已发生过的 `Connected` 事件 → 60 秒超时 → `StartWifiConfigMode()` → `requestAppSetup()` → 重启进入 APP SETUP 页面。不会丢失任何数据（绑定、`is_configed`、Wi-Fi 密码都在），复位即可恢复。修复思路：在 `TryWifiConnect()` 里把已连接的情况直接当作连接成功（fork 补丁），或者在测试页面关闭时关掉 Wi-Fi。临时办法：做完网络测试先重启。可以考虑给官方提 issue。 |

## 以后

| 编号 | 事项 | 状态 | 说明 |
|---|---|---|---|
| B-010 | Claude Code 状态机器人 | doing | 第一期（USB 通道、Claude 层、`tools/sc-agent`、hook）已在 `feat/claude-link` 分支实现，等待真机验证。[REQ-003](requirements/REQ-003-claude-code-status.zh-CN.md)：Claude Code hook → sc-agent（Windows 上的 Python，WSL 里的会话也走它）→ 通过 USB 通道连机器人；第二期经 NAS 上的 Mosquitto 中转（静态 IP、自签 CA）；第三期通过小智 MCP 接入点语音问进度。按项目或会话选择接入；永远不能批准授权请求。 |
| B-011 | 待机行为（第一批） | doing | 由心情驱动的行为调度器：08–23 点作息、天黑睡觉、摸头/拿起/倾斜反应、只动屏幕的小动作、心情灯；静音且安静。已在 `feat/idle-behavior` 分支实现；2026-10-20 第一次真机测试，之后做了调整（触摸静默期、照度阈值、墨尔本时区）。其余验收测试待做。见 [REQ-002](requirements/REQ-002-idle-behaviour.zh-CN.md)。 |
| B-017 | 待机行为（第二批） | todo | 敲桌子检测、跟着音乐点头（需要从小智拿麦克风音量）、倾斜时头部保持水平（IMU 坐标轴已标定：重力在 +Y，俯仰绕 X 轴）、内心状态持久化。 |
| B-012 | 设备端人脸检测与跟踪 | todo | ESP-DL `human_face_detect`；缓冲区和任务栈放在 PSRAM；对话时暂停；先做原型验证内存和帧率。 |
| B-013 | 识别桌面物品 | todo | NAS 上用 YOLO（OpenVINO）加反应规则；只在遇到新的或认不出的物品时才调用小型多模态模型。 |
| B-014 | 更多开始和结束对话的方式 | todo | 摸头、轻拍或摇晃（IMU）、语音退出工具、离线命令词（依赖 B-001）。 |
| B-015 | 评估小智 v2.5.x | todo | 参考 artrobin-cn 的分支 `public/work-secretary-v2.5`；该分支为了让唤醒词能用而关闭了回声消除，这个问题要先解决。 |
| B-016 | 自建小智服务器 | todo | 可选；在 NAS 上用 Docker 部署 `xinnan-tech/xiaozhi-esp32-server`。 |
