# Backlog

[English](backlog.md) | **中文**

状态说明：`todo`（待做）· `decide`（待决策）· `doing`（进行中）· `done`（已完成）· `dropped`（已放弃）。最后更新：2026-10-20。

## 已完成

| 编号 | 事项 | 说明 |
|---|---|---|
| B-000 | 工具链、备份与仓库搭建 | ESP-IDF v5.5.4；未修改的 v1.5.1 和 v1.6.0 都编译通过；完整备份了运行官方 1.6.0 的设备的 16 MB flash；fork 与 git 工作流已配置。详见[开发日志 2026-10-20](devlog/2026-10-20.zh-CN.md)。 |

## 接下来

| 编号 | 事项 | 状态 | 说明 |
|---|---|---|---|
| B-001 | 自定义唤醒词"起来干活"（qi lai gan huo） | doing | 已决定：用 MultiNet `mn7_cn`，并把资源分区扩大到 6080 KB。已在 `feat/wake-word` 分支实现并编译通过，等待真机验证。见 [REQ-001](requirements/REQ-001-custom-wake-word.zh-CN.md)。 |
| B-002 | 关闭小智自动 OTA | doing | 在 fork 补丁里加了 `SC_DISABLE_AUTO_OTA`（默认开启）；和 B-001 一起实现，尚未真机验证。 |
| B-003 | `fetch_repos.py` 支持一个仓库打多个补丁 | done | `repos.json` 里新增 `extra_patches`；重复运行不会出错；补丁打不上时报错，不再静默跳过；新增 `tools/xiaozhi_fork_patch.py`。已在电脑上测试（已打过补丁 / 源码有改动 / 干净源码三种情况）。 |
| B-004 | Wi-Fi 吞吐与延迟 | todo | 下载和局域网控制期间关闭省电模式；把 Wi-Fi/lwIP 缓冲区放进 PSRAM（`SPIRAM_TRY_ALLOCATE_WIFI_LWIP`），并调大接收缓冲区、TCP 窗口和 UDP 收包队列。 |
| B-005 | 不要在睡眠时持有 LVGL 锁 | todo | 小智忙碌时，`_stackchan_update_task` 会拿着锁执行 `vTaskDelay(100)`（`hal.cpp`）。 |
| B-006 | 恢复热点配网 | todo | 1.6 只允许通过 App（蓝牙）配网，而自编译固件过不了 App 的握手。目前的备用办法：用 nRF Connect 通过蓝牙发送 `setWifi`。 |
| B-007 | 检查音频任务调度 | todo | 只在说话卡顿复发时处理：`opus_codec`（优先级 2）低于 LVGL 和 stackchan 任务（优先级 3）。 |

## 以后

| 编号 | 事项 | 状态 | 说明 |
|---|---|---|---|
| B-010 | NAS 上的局域网 bridge，以及给 Claude Code 用的 MCP 工具 | todo | 播报、表情、动作、查询状态；只在选定的项目或会话中启用；按角色分配令牌；外网只通过 Tailscale 访问。 |
| B-011 | 待机行为（第一批） | doing | 由心情驱动的行为调度器：08–23 点作息、天黑睡觉、摸头/拿起/倾斜反应、只动屏幕的小动作、心情灯；静音且安静。已在 `feat/idle-behavior` 分支实现并编译通过，等待真机验证。见 [REQ-002](requirements/REQ-002-idle-behaviour.zh-CN.md)。 |
| B-017 | 待机行为（第二批） | todo | 敲桌子检测、跟着音乐点头（需要从小智拿麦克风音量）、倾斜时头部保持水平（先标定 IMU 坐标轴）、内心状态持久化。 |
| B-012 | 设备端人脸检测与跟踪 | todo | ESP-DL `human_face_detect`；缓冲区和任务栈放在 PSRAM；对话时暂停；先做原型验证内存和帧率。 |
| B-013 | 识别桌面物品 | todo | NAS 上用 YOLO（OpenVINO）加反应规则；只在遇到新的或认不出的物品时才调用小型多模态模型。 |
| B-014 | 更多开始和结束对话的方式 | todo | 摸头、轻拍或摇晃（IMU）、语音退出工具、离线命令词（依赖 B-001）。 |
| B-015 | 评估小智 v2.5.x | todo | 参考 artrobin-cn 的分支 `public/work-secretary-v2.5`；该分支为了让唤醒词能用而关闭了回声消除，这个问题要先解决。 |
| B-016 | 自建小智服务器 | todo | 可选；在 NAS 上用 Docker 部署 `xinnan-tech/xiaozhi-esp32-server`。 |
