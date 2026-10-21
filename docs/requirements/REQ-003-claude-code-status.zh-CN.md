# REQ-003 Claude Code 状态机器人

[English](REQ-003-claude-code-status.md) | **中文**

状态：第一期（USB 通道、Claude 层、sc-agent、hook）已在 `feat/claude-link` 分支实现，**尚未真机验证**；
第二、三期仍是设计 · 对应 backlog：B-010

## 目标

机器人显示维护者电脑上各个 Claude Code 会话在做什么，在某个会话需要输入时把维护者叫回来；既不拖慢 Claude Code，
也不依赖机器人当时连的是哪个网络。

## 维护者的决定（2026-10-21）

- 机器人跟着维护者在家和工位之间来回；工位 Wi-Fi 不稳定，而且经常和电脑不在同一个局域网。
- 只有一台电脑跑 Claude Code，经常同时开多个会话，大多在 **WSL** 里，偶尔在 Windows 上直接跑。
- 电脑上可以常驻一个后台程序；用 Python（不要黑盒的 exe），通过 .bat / bash 脚本启动。
- 只有选定的会话接入：项目里放标记文件，**或者**启动时设环境变量。
- 工作时间不出声：工作日 08:00–18:00（已确认），机器人的夜间也一律不出声。
- 气泡里可以显示项目名和事件。
- NAS 有静态公网 IP，路由器可以做端口转发（没有域名）。
- 第三期的语音播报可以说项目名（已确认）。

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

## USB 通道协议 v1（第一期）

传输：机器人的原生 USB-Serial/JTAG 口（Windows 上 VID 303A、PID 1001），名义波特率 115200。打开前先把 DTR 和 RTS
设为释放状态，之后不要切换（它们会让芯片复位）。

帧格式（双向相同），一行 =

```
@@sc<紧凑 JSON>*<JSON 字节的 CRC-32，8 位大写十六进制>\n
```

- CRC-32 是标准 zlib CRC（`zlib.crc32(json_bytes)`；设备端用 `esp_rom_crc32_le(0, ...)`）。
- JSON 是 UTF-8、单行、内部没有 `\n`。电脑 → 设备最多 768 字节 JSON，设备 → 电脑最多 256 字节。
- 其他所有行都是日志。接收方在一行里找**最后一个** `@@sc` 以及它之后的最后一个 `*`；帧前面可能有日志文字或 ANSI 颜色码。
  设备发出的行以 `\r\n` 结尾。
- 无效帧（CRC 错、JSON 错、超长）直接丢弃。

电脑 → 设备的消息都带 `"t"`（类型）和 `"id"`（uint32，在后台程序一次运行内严格递增）。设备对每条消息回一个 ack；
电脑最多等 500 ms，最多用**相同**的 id 重发两次（设备收到重复的 id 只回 ack，不重复执行）。同一时间只有一条消息在途。

| t | 字段 | 含义 |
|---|---|---|
| `hello` | `v`（1）、`agent`（字符串） | 建立连接。ack 带 `v`、`fw`、`boot`（每次开机随机）、`up`（秒）。`boot` 变了说明机器人重启过：重新发送状态。同时重置设备端的 id 去重。 |
| `ping` | – | 保活，每 2 秒一次。 |
| `state` | `sessions`：最多 5 个 `{slot 0-4, name ≤ 12 个字符, st}` | 完整快照，整体替换。`st`：`idle`、`work`、`wait`、`done`、`err`。空列表 = 关闭 Claude 层。 |
| `event` | `slot`、`ev`、`text`（≤ 40 个字符）、`snd`（可选 bool） | 一次性反应。`ev`：`needs`、`done`、`fail`、`err`、`compact`。`snd` 请求一声短提示音；夜里和对话中机器人仍会拒绝。 |
| `clear` | – | 等同于空的 `state`。 |

ack（设备 → 电脑）：`{"t":"ack","id":N,"ok":true,"st":"idle|talk|other","boot":B,"n":K}`；出错时
`"ok":false,"err":"..."`。`st` 是机器人状态：`idle` = AI Agent 待机，`talk` = 对话中，`other` = 桌面或其他应用。
`boot`（每次开机随机）和 `n`（机器人上已用的会话槽位数）在**每个** ack 里都有：`boot` 变了（重启但 USB 口没断开），
或者 `n` 和电脑端最后发送的状态不一致，电脑端都会重新发送状态。

过期：设备 30 秒内没收到任何有效帧，就清空会话（后台程序停了或拔了线）。

### 协议 v1.1 新增（向后兼容，接收方忽略不认识的字段）

- `state.sessions[i].act`（可选）：正在干活的会话此刻在做什么：`think`（收到任务、还没用工具）、`edit`
  （Edit/Write/NotebookEdit）、`run`（Bash 和其他工具）、`read`（Read/Grep/Glob）、`web`（WebSearch/WebFetch）、
  `test`（测试或编译命令，或 `sc-run`）、`agents`（子代理在跑）。空闲、等你、完成、出错时不带或为空。
- `state.sessions[i].ctx`（可选，0–100）：状态栏脚本上报的上下文用量。
- `event.ev` 新增 `pass`：测试或编译命令成功；`text` 带结果，比如 `StackChan: 42 passed`。
  `fail` 现在只用于测试和编译命令（其他命令失败一律忽略）；知道数量时 text 带上，比如 `StackChan: 3 failed`。

### 协议 v1.2 新增（REQ-006 的固件，向后兼容）

- 电脑 → 设备 `agent`（无字段）：启动 AI Agent，效果和在主菜单点它的图标一样。由主菜单在自己的循环里处理（必须先在
  LVGL 锁里关闭主菜单界面，再拆掉 mooncake；第一次直接在 USB 任务里请求启动小智就崩溃了）。AI Agent 已经在运行时忽略；
  30 秒内主菜单没有接手（开着别的应用、在配网页）的请求会被丢弃。旧固件回复 `"ok":false,"err":"type"`。
- 每个 ack 都带 `ai`（bool）：AI Agent 已启动（已请求或正在运行）。
- `tools/sc-agent/sc-flash.ps1` 通过 `POST /agent` 使用。

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

## 第一期实现说明（2026-10-21，维护者睡觉期间自行决定）

固件（`feat/claude-link`）：
- `main/hal/sc/usb_link.*`：任务 "sc_usb" 在核心 1、优先级 2、6 KB PSRAM 栈；驱动接收缓冲 1 KB；回复用一次 `write()`
  写到 `/dev/secondary`；在 `Hal::init()` 末尾启动，所有模式下都运行（状态只在 AI Agent 待机时显示）。`SC_USB_LINK`（REQ-006 起 Kconfig 默认关闭，在 `sdkconfig.defaults.sc` 里开启）。
- `main/stackchan/sc/claude_status.*`：单例，保存 5 个槽位、事件队列（4 条、最长保留 30 秒）和 30 秒过期规则。
- 待机行为：有会话时由它们占用 LED 1–5 / 7–11（槽位 i 对应 LED 1+i 和 7+i）；短暂的反应效果（被拿起、撸毛）仍然优先。
  颜色：idle 暗白、work 蓝色呼吸、wait 琥珀色闪烁、done 绿色（10 分钟后变暗）、err 红色；工具失败时该槽位红色闪 3 秒。
  睡觉时亮度降到 35%。
- 反应：needs → 疑惑脸、气泡 8 秒、抬头再回位、可选提示音；done → 开心、气泡、轻轻点头；fail → 冒汗、气泡；
  err → 难过、气泡；compact → 打哈欠、气泡。转头使用待机行为的舵机配额（配额用完就不转头，表情和气泡照常）。
  夜间时段（23:00–08:00）无论醒着还是睡着都只用灯。needs 绝不会被丢掉：先请求提示音（在主任务里播放，不在 LVGL 锁里），
  睡着或正在打瞌睡时把它叫醒（醒来本身就是回应动作），并在缓冲期内保持清醒。睡着时其他事件只靠灯。
  忙着（被拿着、倾斜、头被摸、正在做别的反应）时只显示气泡。
- LED 颜色考虑了 PY32 驱动的 RGB565 精度（很暗的非零通道会抬到第一档）。
- 与设计的差异：对话期间待机行为不存在，所以会话灯是熄灭的；事件会等待（最长 30 秒），对话结束后再显示。
- 协议 v1.1（2026-10-21 当天）：`act` 驱动 REQ-005 的表情（干活、思考、搜网页时好奇），子代理在跑时会话灯变青色并加快呼吸，
  `ctx` 超过 80% 时偶尔出现的表情里多半是"吃撑了"，`pass` 显示"完成啦"和结果。Claude 相关反应改用 FaceArt 表情，不再用情绪。

电脑端（`tools/sc-agent/`，见其 README）：
- 忽略 `idle_prompt` 通知（Claude Code 每次回答结束约 60 秒后都会发一次，照单全收会让机器人每次都再叫一遍）；
  授权请求被处理后，`PostToolUse` 把 "wait" 改回 "work"。
- 任何已映射的事件都会重新创建未知的会话（后台程序重启或 30 分钟过期后也能恢复）；已结束的会话在 60 秒内忽略迟到的异步 hook。
- 只有 2 秒没有通信时才发 ping；重试后仍收不到 ack 就当作连接断开（重新 hello，再发一次状态）。
- `/release` 30 分钟后自动重新接管串口（`release_timeout_s`，设为 0 则一直等 `/acquire`）。
- `merge_hooks.py` 把 hook 配置合并进 settings.json（会写一个 `.bak` 备份）；`--statusline` 在没有配置状态栏时安装
  `sc_statusline.py`（显示模型、上下文百分比、花费，并上报上下文用量）。
- v1.1：PreToolUse / SubagentStart / SubagentStop hook 设置 `act`；`results.py` 识别测试和编译命令，解析 pytest、unittest、
  ctest、jest/vitest、cargo、go 和 idf.py 的结果（`pass` / `fail` 带数量）；其他命令失败不再有反应；`sc-run <命令>`
  播报在 Claude Code 之外运行的命令。

## 待定

- 原型阶段要验证：大量日志时 USB 通道的丢帧率；刷机时 Windows 释放串口；WSL 调用 `curl.exe` 的延迟；MQTT 客户端占用的内存。
