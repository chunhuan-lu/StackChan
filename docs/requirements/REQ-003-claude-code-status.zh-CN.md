# REQ-003 Claude Code 状态机器人

[English](REQ-003-claude-code-status.md) | **中文**

状态：**设计草稿**，尚未实现 · 对应 backlog：B-010

## 目标

机器人显示维护者电脑上各个 Claude Code 会话在做什么，在某个会话需要输入时把维护者叫回来；既不拖慢 Claude Code，
也不依赖机器人当时连的是哪个网络。

## 维护者的决定（2026-10-21）

- 机器人跟着维护者在家和工位之间来回；工位 Wi-Fi 不稳定，而且经常和电脑不在同一个局域网。
- 只有一台电脑跑 Claude Code，经常同时开多个会话，大多在 **WSL** 里，偶尔在 Windows 上直接跑。
- 电脑上可以常驻一个后台程序；用 Python（不要黑盒的 exe），通过 .bat / bash 脚本启动。
- 只有选定的会话接入：项目里放标记文件，**或者**启动时设环境变量。
- 工作时间不出声。
- 气泡里可以显示项目名和事件。
- NAS 有静态公网 IP，路由器可以做端口转发（没有域名）。

## 架构

```
Claude Code hook（WSL 或 Windows）
   │  sc-hook：先判断是否接入，再用 curl.exe 发到 127.0.0.1（在 Windows 侧运行，WSL 里也一样）
   ▼
sc-agent（Windows，Python，登录时启动）
   ├─ 会话表 + 合并限速，决定机器人显示什么
   ├─ USB 通道（COM 口，同时把串口日志滚动存文件）──────────────▶ 插在这台电脑上的机器人
   └─ 通过 TLS 连 NAS 上的 Mosquitto（静态 IP，自签 CA）◀──────── 连着任意 Wi-Fi 的机器人（第二期）

语音（第三期）：机器人 → 小智云 → MCP 接入点 ◀── sc-agent 主动连接
```

### Hook（Claude Code 这边）

- 在每个环境的用户设置里配置一次（WSL 的 `~/.claude/settings.json`、Windows 的
  `%USERPROFILE%\.claude\settings.json`），全部设成 `async: true` 的命令 hook，永远不会卡住会话。
- 一个很小的脚本 `sc-hook`（bash，WSL 和 Git Bash 都能用）：没设 `SC_LINK=1`、项目里也没有 `.claude/stackchan`
  标记文件时直接退出；否则把 hook 的 JSON 带上令牌交给 `curl.exe`。在 WSL 里 `curl.exe` 通过互操作在 Windows 上运行，
  所以 `127.0.0.1` 能直接连到 sc-agent，不用改 WSL 的网络设置。
- 用到的事件：`SessionStart`、`UserPromptSubmit`、`PostToolUseFailure`、`Notification`（`permission_prompt`、
  `idle_prompt`、`agent_needs_input`、`elicitation_dialog`）、`Stop`、`StopFailure`、`SubagentStart` / `SubagentStop`、
  `PreCompact` / `PostCompact`、`SessionEnd`。可选：用状态栏脚本的包装上报上下文用量。

### sc-agent（电脑这边）

- Python 3，独立的虚拟环境（`pyserial`；第二期加 `paho-mqtt`）；`start-sc-agent.bat` 用 `pythonw` 在后台运行；
  可选加一个登录时自动启动的计划任务。只监听 `127.0.0.1`，令牌存在用户目录的文件里。
- 会话表按 `session_id` 区分：项目名（文件夹名，截到 12 个字符）、状态、最近事件、上下文用量。
  收到 `SessionEnd` 或 30 分钟没有事件就移除。
- 给机器人发可重复应用的完整**状态快照**（最多每秒 5 次，需要叫人时立即发），以及用于一次性动画的短**事件**消息。
- 连接方式：机器人插在这台电脑上时走 USB（按 VID:PID 303A:1001 找串口），否则走 MQTT（第二期），都没有就不发。
- 独占 COM 口：把所有非协议行写进滚动日志文件（取代 `tools/serial_log.py`），并通过包装 `idf.py` 的脚本在刷机时释放、刷完再接管。
- 出声的静音时段：工作日 08:00–18:00 和机器人的夜间（23:00–08:00）一律不出声；可配置。

### 机器人（固件这边）

- **USB 通道**（`SC_USB_LINK`）：在原生 USB 口上，和普通日志并行传带校验码的 JSON 行 `@@sc{...}*CRC32`；
  用 usb_serial_jtag 驱动接收，回复用一次 `write()` 写到 `/dev/secondary`；任务栈放在 PSRAM；约占 1 KB 内部内存。
  一问一答，电脑端负责重发。
- 待机行为里新增 **Claude 层**，做成文件级单例（因为每次对话前后待机行为都会重建）。有会话时它独占 LED 1–5 / 7–11
  （LED 0 / 6 继续给小智的状态灯），也可以接管表情；它是这些 LED 唯一的写入者。
- 命令只接受白名单（state、event、ping）；文字限制长度；转头使用待机行为的舵机配额和 5–85° 的俯仰限制；
  任何命令都不会转发给 MCP、设置或 OTA。
- 对话期间只有灯跟着变；表情和气泡等对话结束再显示。夜里只亮灯，不转头、不出声。
- **永远不提供**通过机器人批准 Claude Code 授权请求的途径。

### 第二期：中转

- NAS 上用 Docker 跑 Mosquitto 2，路由器把 8883 端口转发给它的 TLS 监听，服务器证书由我们自己的 CA 签发；
  机器人内置这张 CA 证书（`cert_pem`），sc-agent 用同一个 CA 文件。每个客户端单独的密码和话题权限
  （机器人：读 `sc/<id>/cmd`、写 `sc/<id>/state`；sc-agent 相反）。只暴露这一个端口。
- 机器人只在 AI Agent 空闲时连接（对话时暂停，暂停前发布保留的离线状态），心跳间隔小于 60 秒。
  额外内部内存约 5–8 KB，待实测；不够的话，把 esp-mqtt 的任务栈通过打补丁的组件放进 PSRAM。

### 第三期：语音问进度

- sc-agent 连接小智的 MCP 接入点，提供只读工具 `claude_status`（"3 个会话：StackChan 在干活，docs 在等你"）。
  离开电脑的只有项目名和状态，绝不包含提示词、输出或代码。

## 事件对应

| Claude Code | 机器人 | 气泡 |
|---|---|---|
| 发了消息 / 在干活 | 思考脸，该会话的灯蓝色呼吸 | – |
| 工具失败（`PostToolUseFailure`） | 冒汗 3 秒，灯红色闪 | `<项目>: failed` |
| 需要输入（`Notification`） | 抬头看用户，灯琥珀色闪；静音时段以外可轻提示一声 | `<项目>: needs you` |
| 完成（`Stop`） | 点头加开心，灯变绿 | `<项目>: done` |
| API 出错（`StopFailure`） | 难过，灯红色 | `<项目>: error` |
| 子代理在跑 | 灯环转圈 | – |
| 上下文超过 80% / 压缩 | 吃撑的表情 / 打哈欠 | `<项目>: compacting` |

LED 布局：最多 5 个会话，每个会话一颗灯，左边（1–5）和右边（7–11）对称显示。

## 分期

| 阶段 | 内容 |
|---|---|
| P0 | REQ-004 的修复（待机、供电、触摸） |
| P1 | USB 通道 + Claude 层（固件）、sc-agent 的 USB 部分、hook；功能：叫你回来、心情跟着 Claude 走、多会话灯、上下文 |
| P2 | NAS 上的中转 |
| P3 | 通过小智 MCP 接入点语音问进度 |

## 待定

- 静音规则的具体工作时间（默认工作日 08:00–18:00）。
- 第三期的语音播报能不能说项目名，因为它会经过小智云。
- 原型阶段要验证：大量日志时 USB 通道的丢帧率；刷机时 Windows 释放串口；WSL 调用 `curl.exe` 的延迟；MQTT 客户端占用的内存。
