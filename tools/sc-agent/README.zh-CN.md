# sc-agent：在 StackChan 上显示 Claude Code 状态

[English](README.md) | **中文**

sc-agent 把这台电脑上各个 Claude Code 会话的状态显示在通过 USB 连接的 StackChan 机器人上，并在会话需要你输入时提醒你。
它实现了 [REQ-003](../../docs/requirements/REQ-003-claude-code-status.zh-CN.md) 第 1 阶段（USB 链路）的电脑端。

```
Claude Code（WSL 或 Windows）── 异步 hook ── sc-hook.sh ── curl.exe ──▶ 127.0.0.1:47123
                                                                         sc-agent（Windows，Python）
                                                   USB 链路（COM 口）◀──┘   └──▶ 串口日志
                                                              StackChan
```

机器人固件必须包含 USB 链路（`SC_USB_LINK`，见 REQ-003）。不应答 `hello` 的端口在试过 5 次后会被关闭，并在 60 秒内不再使用
（见"查找机器人"），已知是机器人的端口除外。

## 文件

| 文件 | 用途 |
|---|---|
| `sc_agent.py` | agent 本体：接收 hook 的 HTTP 接口、会话表、USB 链路、串口日志、烧录时释放端口 |
| `sessions.py` | 纯逻辑：hook 事件到机器人会话/事件的映射、活动、槽位、过期、静音时段 |
| `results.py` | 纯逻辑：识别测试/构建命令并总结其输出（`42 passed`、`build ok`） |
| `sc_link.py` | 纯函数：USB 链路协议 v1 的帧编码/解析 |
| `sc-hook.sh` | Claude Code hook（bash，WSL 和 Git Bash 通用）：检查是否启用，把 hook JSON 发给 agent |
| `sc-run.sh` | `sc-run <命令>`：在机器人上显示任意命令（测试、构建、脚本）及其结果 |
| `sc_statusline.py` | Claude Code 状态栏（模型、上下文占用、费用），同时上报上下文占用 |
| `hooks-settings.json` | 注册 hook 用的 `~/.claude/settings.json` 片段 |
| `merge_hooks.py` | 把 `hooks-settings.json` 合并进 settings 文件（可重复执行，保留 `.bak`） |
| `setup.bat` | 创建（或修复）`.venv` 并安装 `requirements.txt`（pyserial） |
| `start-sc-agent.bat` | 在后台（`pythonw`）启动 agent，加 `--console` 则在前台运行 |
| `sc-idf.ps1` | 在 agent 释放 COM 口后运行 `idf.py` |
| `test_*.py` | 单元测试（只用标准库，不需要串口） |

## 安装

前提：Windows 10/11，Python 3.10 或更新版本（python.org 安装包或 `py` 启动器），`curl.exe`（Windows 自带）。
`<repo>` 指仓库目录，例如 `E:\workbench\M5\src\StackChan`（WSL 中为 `/mnt/e/workbench/M5/src/StackChan`）。

1. 创建虚拟环境（只需一次，升级 Python 后再执行一次）：
   ```bat
   <repo>\tools\sc-agent\setup.bat
   ```
   默认使用 PATH 中的 `py -3` 或 `python`；如需指定，把 `SC_PYTHON` 设为 `python.exe` 的完整路径。
   已有的 `.venv` 如果无法启动或由另一个 Python 版本创建，会被重建（请先停止 agent，它运行在这个 `.venv` 中）。
2. 启动 agent：`start-sc-agent.bat`（后台）或 `start-sc-agent.bat --console`（日志显示在窗口中，Ctrl+C 结束）。
   第一次启动会生成令牌文件 `%USERPROFILE%\.sc-agent\token`。
3. 为 **WSL** 安装 hook（在 WSL 中执行，把 `<you>` 换成 Windows 用户名）：
   ```bash
   mkdir -p ~/.sc-agent ~/.claude
   cp /mnt/c/Users/<you>/.sc-agent/token ~/.sc-agent/token && chmod 600 ~/.sc-agent/token
   cp <repo>/tools/sc-agent/sc-hook.sh ~/.claude/sc-hook.sh && chmod +x ~/.claude/sc-hook.sh
   ```
   在 `~/.claude/settings.json` 中注册 hook（加入 `hooks-settings.json` 的条目，替换旧的 `sc-hook.sh` 条目，
   保留你已有的设置和 hook，写出 `settings.json.bak`；重复执行不会改动任何东西，因此也可用来升级旧的安装）：
   ```bash
   python3 <repo>/tools/sc-agent/merge_hooks.py
   ```
4. 为 **Windows** 安装 hook（Windows 上 Claude Code 用 Git Bash 运行 hook；Git Bash 的 `~` 就是
   `%USERPROFILE%`，所以令牌无需复制）：
   ```powershell
   Copy-Item <repo>\tools\sc-agent\sc-hook.sh $env:USERPROFILE\.claude\sc-hook.sh
   <repo>\tools\sc-agent\.venv\Scripts\python.exe <repo>\tools\sc-agent\merge_hooks.py
   ```
   第二行把 hook 注册到 `%USERPROFILE%\.claude\settings.json`。也可以不用 `merge_hooks.py`，手动复制
   `hooks-settings.json` 中的条目。

   已经在运行的 Claude Code 会话要重启（或在 `/hooks` 中确认）后才会使用新的 hook。
5. 可选，在机器人上显示**上下文占用**：安装状态栏脚本（它也会在 Claude Code 底部显示模型、上下文百分比和费用）。在 WSL 中：
   ```bash
   cp <repo>/tools/sc-agent/sc_statusline.py ~/.claude/sc_statusline.py
   python3 <repo>/tools/sc-agent/merge_hooks.py --statusline
   ```
   在 Windows 上把它复制到 `%USERPROFILE%\.claude\`，并用 `.venv` 中的 Python 运行 `merge_hooks.py --statusline`
   （该解释器的路径会写进命令）。`--statusline` 只在还没有配置 `statusLine` 时才添加；否则只打印应写入你自己状态栏的命令。
   脚本需要 Python 3.8+（只用标准库，PATH 中任意 `python3` 都可以），并直接访问 127.0.0.1 上的 agent，因此在 WSL 中需要
   镜像网络（`networkingMode=mirrored`）；没有镜像网络时状态栏照常显示，只是机器人收不到上下文数值。
6. 可选，为你自己启动的命令安装 **sc-run**（WSL：`~/.local/bin`；Git Bash：`~/bin`，都在 PATH 中）：
   ```bash
   mkdir -p ~/.local/bin && cp <repo>/tools/sc-agent/sc-run.sh ~/.local/bin/sc-run && chmod +x ~/.local/bin/sc-run
   ```

hook 脚本必须保持 LF 换行；本目录的 `.gitattributes` 保证检出时如此。脚本不输出任何内容，总是以 0 退出，最多等待 2 秒。
除 `SessionEnd` 外，所有 hook 都以 `async` 方式注册，不会拖慢会话；`SessionEnd` 同步运行，超时 3 秒，
因为 Claude Code 退出时仍在运行的异步 hook 会被终止，机器人就会一直显示已结束的会话，直到它过期。
`PreToolUse` 和 `PostToolUse` 在每次调用工具时都会运行（各是一个很短的后台 `curl` 调用）。

## 按会话启用

只有选定的会话会和机器人通信：

- 标记文件：在项目根目录（`$CLAUDE_PROJECT_DIR`，未设置时为当前目录）执行 `mkdir -p .claude && touch .claude/stackchan`。
  不想共享的话把它加入 `.gitignore`。
- 或者用 `SC_LINK=1 claude` 启动 Claude Code。`SC_LINK=0` 即使有标记文件也关闭。
- 状态栏脚本使用同样的规则（标记文件位于 `CLAUDE_PROJECT_DIR`、工作区项目目录或当前目录）；`sc-run` 不需要启用。

## 机器人显示什么

| Claude Code hook | 会话状态 | 机器人事件 / 气泡 |
|---|---|---|
| `SessionStart` | `idle`（新会话；已有会话只在 `done`/`err` 或 5 秒无事件后重置） | – |
| `UserPromptSubmit` | `work`，活动 `think` | – |
| `PreToolUse` | 根据工具设置活动（见下文）；从不新建会话，也不改变状态 | – |
| `PostToolUse` | `wait` → `work`（授权请求已被处理）；不新建会话；忽略子代理的工具调用 | 测试/构建命令：`pass`（输出中有失败时为 `fail`），`<项目>: 42 passed` |
| `PostToolUseFailure` | `work`（等待中的会话保持 `wait`） | 测试/构建命令：`fail`，`<项目>: 3 failed`；其他失败：不发事件 |
| `SubagentStart` / `SubagentStop` | 子代理运行期间活动为 `agents` | – |
| `Notification`（`permission_prompt`、`agent_needs_input`、`elicitation_dialog`、`elicitation_url_dialog`） | `wait` | `needs`，`<项目>: needs you`，可选提示音 |
| `Notification`（`idle_prompt`） | `work`/`wait` → `idle`，其他不变；不新建会话 | – |
| `Stop` | `done` | `done`，`<项目>: done` |
| `StopFailure` | `err` | `err`，`<项目>: error` |
| `PreCompact` | 不变 | `compact`，`<项目>: compacting` |
| `SessionEnd` | 删除 | – |
| 状态栏（`sc_statusline.py`） | 已有会话的上下文占用 `ctx` | – |
| `sc-run` 开始 / 结束 | 以文件夹命名的独立会话：`work` + `test`，然后 `done` / `err` | `pass` / `fail`，例如 `<项目>: 7 passed`、`<项目>: exit 2` |

- `idle_prompt`（Claude Code 开始等待提示词约 60 秒后发出）不触发提醒，否则每次回答之后都会再叫一遍"needs you"。
  它只用来纠正停留在 `work`/`wait` 的会话，例如你中断了 Claude（这时不会有 `Stop`）。`PostToolUse` 每次调用工具都会触发
  （后台异步运行，不会拖慢 Claude）；子代理的工具调用（带 `agent_id`）不会结束主会话的 `wait`。
- 异步 hook 可能乱序到达，所以紧跟在工作中会话的其他事件之后的 `SessionStart` 不会重置它；压缩之后的 `SessionStart`
  （`source: compact`）从不重置。
- `<项目>` 是项目文件夹名，最多 12 个字符。最多 5 个会话（槽位 0–4）；同时出现的第 6 个会话会被忽略，直到有空槽位。
  30 分钟没有事件的会话会被清除。
- 只有"needs you"会请求提示音，并且工作日 08:00–18:00 和夜间 23:00–08:00 从不发声（可配置）。机器人自己在夜间和对话中也保持安静。
- hook 只通过本机回环接口把 hook JSON（其中可能包含你的提示词）交给 agent。agent 从不记录或转发它；
  机器人只收到槽位、项目名、状态和简短的气泡文字。
- 机器人永远无法批准 Claude Code 的权限请求：协议里没有这样的消息。

## 活动、测试结果和上下文

- **活动**（状态消息中的 `act`，会话工作时显示）：提示词之后为 `think`；由 `PreToolUse` 决定：
  Edit/Write/MultiEdit/NotebookEdit 为 `edit`，Read/Grep/Glob/LS 为 `read`，WebSearch/WebFetch 为 `web`，Task/Agent 为
  `agents`，Bash 运行测试或构建命令时为 `test`，否则为 `run`；其他工具为 `run`。子代理运行期间（按 `agent_id` 统计
  `SubagentStart`/`SubagentStop`）活动为 `agents`。子代理的工具调用不改变活动。"needs you"、`Stop`、`StopFailure`
  以及 `idle_prompt` / `SessionStart` 的纠正会清除活动。
- **测试和构建命令**：在 Bash 命令行的每一段（以 `;`、`&&`、`||`、`|` 分隔）开头识别，事先去掉环境变量赋值、`cd x &&`、
  `time`、`uv run`、`poetry run`、`npx`、`pnpm` 和 `yarn`：`pytest`、`python -m pytest|unittest`、`tox`、`nox`、`ctest`、
  `make test|check`、`cmake --build`、`ninja`、`npm|pnpm|yarn test`、`jest`、`vitest`、`cargo test|build`、`go test|build`、
  `idf.py build|flash|app-flash`、`mvn test`、`gradle test`、`dotnet test`。可用配置键 `test_patterns` 增加（正则表达式，
  在每一段开头匹配）。列表在 `results.py` 中。
- **结果**：从输出中读取 pytest、unittest、ctest、jest/vitest、cargo test 和 go test（按包计数）的汇总，以输出中最后一个
  汇总为准；否则由退出码决定（`tests ok`、`build ok`、`build failed`，idf.py 构建成功但烧录失败时为 `flash failed`）。
  测试结果不发声。
- **上下文**：状态栏脚本发送 `ctx`（上下文窗口占用 0–100 %）；agent 只用它更新已有会话。
- 状态变化后等待 200 ms 再发送状态消息，因此在这段时间内又变回原样的活动不会发送任何内容；每秒最多 5 次。
  机器人本身也会让每个表情至少保持几秒。

## sc-run

```bash
sc-run pytest -q
sc-run idf.py build
sc-run ./deploy.sh --dry-run
```

显示你自己启动的任意命令：命令运行期间，机器人得到一个以当前文件夹命名的会话（`work`，活动 `test`），结束后变为
`done` 或 `err`，并显示气泡，例如 `firmware: 7 passed`、`firmware: build ok`、`firmware: ok` 或 `firmware: exit 2`。
stdin、stdout 和 stderr 仍在终端上，sc-run 以命令的退出码退出。在 WSL 终端中命令运行在伪终端里（util-linux 的 `script`），
颜色和交互程序都正常；在 Git Bash 中，或 sc-run 自己的输入/输出被重定向时，输出经过 `tee`：这时 stderr 合并到 stdout，
并且在终端上会设置 `FORCE_COLOR`、`CLICOLOR_FORCE` 和 `PY_COLORS`，让大多数工具保留颜色。输出的最后 16 KB（去掉颜色代码）
发给 agent，其他内容不会离开电脑。不需要启用；令牌和 `SC_PORT` 的用法与 `sc-hook.sh` 相同。命令结束后会话保留 10 分钟
（一直没有结束时最多 3 小时，例如终端被关掉）。

## 配置

可选文件 `%USERPROFILE%\.sc-agent\config.json`（修改后重启 agent；以 `_` 开头的键视为注释）。全部键及默认值：

```json
{
  "http_port": 47123,
  "usb_vid": "303A",
  "usb_pid": "1001",
  "serial_port": null,
  "serial_number": null,
  "quiet_days": ["mon", "tue", "wed", "thu", "fri"],
  "quiet_work": "08:00-18:00",
  "quiet_night": "23:00-08:00",
  "serial_log_days": 30,
  "release_timeout_s": 1800,
  "test_patterns": []
}
```

- `http_port`：1–65535。
- `serial_port`：固定端口，例如 `"COM12"`，代替按 VID:PID 查找。即使机器人不应答也一直占用它。
- `serial_number`：机器人的 USB 序列号（即 `/status` 中的 `serial`，芯片的 MAC 地址）。优先使用该端口，即使不应答也一直占用
  （例如固件没有 USB 链路时，用来保留串口日志）。不设置时，agent 记住最近一次应答 `hello` 的端口的序列号。
- `quiet_work` 只在 `quiet_days` 生效，`quiet_night` 每天生效；`null` 或 `""` 关闭对应规则。
- `serial_log_days`：超过这个天数的串口日志会被删除；`0` 表示一直保留。
- `release_timeout_s`：为烧录而释放的端口即使没有收到 `/acquire`，过了这么久也会被收回（应对被强行关掉的烧录脚本）；
  `0` 表示一直等 `/acquire`。
- `test_patterns`：额外的测试命令正则表达式，例如 `["\\./run_tests\\.sh\\b", "bats\\b"]`（JSON 中反斜杠要转义）。
- 修改 `http_port` 后，hook 也要设置 `SC_PORT`，例如在 Claude Code 设置中加 `"env": {"SC_PORT": "47124"}`；
  `sc-idf.ps1` 同样读取 `SC_PORT`。

## 登录时自动启动（任务计划程序）

在 PowerShell 中执行（按需修改 `$dir`）：

```powershell
$dir = "E:\workbench\M5\src\StackChan\tools\sc-agent"
$action = New-ScheduledTaskAction -Execute "$dir\.venv\Scripts\pythonw.exe" -Argument "`"$dir\sc_agent.py`"" -WorkingDirectory $dir
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
Register-ScheduledTask -TaskName "sc-agent" -Action $action -Trigger $trigger -Settings $settings -Description "StackChan Claude Code status agent"
```

`-ExecutionTimeLimit ([TimeSpan]::Zero)` 去掉默认的 72 小时运行上限。如果注册被拒绝，请在管理员 PowerShell 中执行。
删除任务：`Unregister-ScheduledTask sc-agent`。

正常停止 agent（关闭端口并发送 `clear`，机器人立即清除会话）：

```powershell
curl.exe -s --noproxy "*" -X POST -H "X-SC-Token: $(Get-Content $env:USERPROFILE\.sc-agent\token)" http://127.0.0.1:47123/shutdown
```

`Stop-ScheduledTask sc-agent`、任务管理器或 `Stop-Process` 结束后台（`pythonw`）agent 时不会做这些清理：机器人会保留会话，
直到 30 秒的过期超时。控制台模式下 Ctrl+C 也会正常停止。重新启动：`Start-ScheduledTask sc-agent` 或 `start-sc-agent.bat`。
最后手段：

```powershell
Get-CimInstance Win32_Process -Filter "Name='pythonw.exe'" | Where-Object CommandLine -like '*sc_agent.py*' | ForEach-Object { Stop-Process -Id $_.ProcessId }
```

同时只能运行一个 agent：第二个实例无法绑定 HTTP 端口，会记录日志后退出。

## 查找机器人

agent 查找 VID:PID 为 303A:1001 的端口，所有带原生 USB 的 ESP32-S3 都是这个 ID。它先尝试机器人的序列号（`serial_number`，
或最近一次应答 `hello` 的端口），然后是本次运行中应答过的端口，最后按名称尝试其他端口。本次运行中从未应答过的端口在 5 次
`hello` 无应答（约 20 秒）后会被关闭，并在 60 秒内跳过，这样其他 ESP32-S3 板仍然可用；`agent.log` 会记录这一点，如果它是唯一
匹配的端口则记为警告。机器人自己的端口被占用时（例如 `idf.py monitor`），agent 会等待它，而不去占用别的板子。已知的机器人端口、
`serial_port` 和 `serial_number` 从不被跳过。

## 状态

```powershell
curl.exe -s --noproxy "*" -H "X-SC-Token: $(Get-Content $env:USERPROFILE\.sc-agent\token)" http://127.0.0.1:47123/status
```

`link` 的取值：`searching`（没有可用端口）、`connecting`（端口已打开，`hello` 没有应答）、`linked`、`released`。
返回内容还包括端口及其 USB 序列号、最近一次 ack 中的机器人状态（`idle`、`talk`、`other`）、boot id 和固件版本、会话列表
以及链路计数器（`retries`、`timeouts`、`bad_frames` 可用来判断丢帧情况）。

## 用 sc-idf.ps1 烧录

agent 占用着 COM 口，所以直接执行 `idf.py flash` 会报"端口被占用"。在 ESP-IDF PowerShell 中，从 `<repo>\firmware` 执行：

```powershell
..\tools\sc-agent\sc-idf.ps1 flash               # 使用 agent 报告的端口
..\tools\sc-agent\sc-idf.ps1 -p COM12 flash monitor
```

对于使用串口的目标（`flash`、`app-flash`、`bootloader-flash`、`partition-table-flash`、`encrypted-flash`、
`encrypted-app-flash`、`monitor`、`erase-flash`、`erase-otadata`、`read-otadata`、`efuse-*`），脚本先释放端口
（`POST /release`），运行 `idf.py -p <端口> <参数>`，然后即使 `idf.py` 失败或被 Ctrl+C 中断，也会把端口交还（`POST /acquire`）。
其他目标（`build`、`menuconfig` 等）直接运行 `idf.py`。agent 没有运行且没有给出 `-p` 时，`idf.py` 不带 `-p` 运行，自己查找端口。如果 PowerShell 拒绝运行脚本，执行一次
`Set-ExecutionPolicy -Scope CurrentUser RemoteSigned`，或者用
`powershell -ExecutionPolicy Bypass -File ..\tools\sc-agent\sc-idf.ps1 flash`。项目规定：只用 `flash`，绝不 `erase-flash`。

## 日志

- `%USERPROFILE%\.sc-agent\agent.log`：agent 日志（1 MB，保留 3 个旧文件）。从不包含令牌或提示词。
- `%USERPROFILE%\.sc-agent\logs\serial-YYYYMMDD.log`：机器人输出的全部内容（去掉协议行和 ANSI 颜色），每行带本地时间。
  日常使用中它取代 `serial_log.py`；同一时间只能有一个程序占用 COM 口。

## 故障排查

- **机器人什么都不显示**：查看 `/status` 和 `agent.log`。
  - `searching`：机器人没插上；VID:PID 不同（设置 `serial_port`）；或者唯一匹配的端口没有应答 `hello`，正在被跳过 60 秒
    （见"查找机器人"）。
  - `cannot open COMx: ... Access is denied`：其他程序占用着端口（`idf.py monitor`、`serial_log.py`、串口终端）。
    关掉它，或用 `sc-idf.ps1` 烧录。agent 每秒重试一次。
  - `connecting` 并且日志中有"robot does not answer hello"：固件没有 USB 链路。
- **烧录后 `sc-idf.ps1` 没能调用 `/acquire`，机器人一直不亮**（例如脚本窗口被关掉）：agent 在 `/release` 之后
  `release_timeout_s`（30 分钟）会自动收回端口，时间从释放时算起，而不是从烧录结束算起。`POST /acquire`
  （与 `/shutdown` 相同的 `curl.exe` 命令）或重启 agent 可以立即收回。
- **hook 没有效果**：检查是否启用（标记文件或 `SC_LINK=1`）。令牌不一致时 `agent.log` 中会出现
  `rejected POST /hook: missing or wrong token`：重新复制令牌。在 WSL 中手动测试：
  ```bash
  curl.exe -s --noproxy '*' -H "X-SC-Token: $(cat ~/.sc-agent/token)" \
    --data-binary '{"hook_event_name":"SessionStart","session_id":"test","cwd":"/tmp/demo"}' http://127.0.0.1:47123/hook
  ```
  应返回 `{"ok": true}`；之后用同样的命令加 `"hook_event_name":"SessionEnd"` 清理。
- **WSL 中找不到 `curl.exe`**：`/etc/wsl.conf` 中设置了 `[interop] appendWindowsPath=false` 时，`command -v curl.exe`
  找不到它。hook 这时会直接尝试 `/mnt/c/Windows/System32/curl.exe`。
- **interop 被关闭**（`[interop] enabled=false`）：WSL 无法启动 Windows 程序。重新启用并执行 `wsl --shutdown`；
  或者把 WSL 切换到镜像网络（在 `%USERPROFILE%\.wslconfig` 的 `[wsl2]` 下设置 `networkingMode=mirrored`），
  这样 Linux 的 `curl` 能访问 Windows 的 127.0.0.1，hook 会自动改用它。
- **WSL 中出现 `curl.exe: cannot execute binary file: Exec format error`**：互操作失效了，常见于 Ubuntu 20.04 在
  `/etc/wsl.conf` 里设置了 `systemd=true`（`/proc/sys/fs/binfmt_misc/` 里缺少 `WSLInterop` 项）。hook 检测到这一项缺失时，
  会改用 Linux 的 `curl`，在镜像网络下可以正常工作。要修复互操作本身：
  `echo ':WSLInterop:M::MZ::/init:PF' | sudo tee /etc/binfmt.d/WSLInterop.conf`，然后
  `sudo systemctl restart systemd-binfmt`（或在 Windows 上执行 `wsl --shutdown`）。
- **HTTP 端口被占用**：修改 `http_port`（见"配置"）。

## 协议与测试

USB 链路协议 v1 定义在 REQ-003 中：帧格式 `@@sc<json>*<CRC32>\n`；停等式发送，ack 超时 500 ms，用相同 id 最多重试 2 次；
连接时发 `hello`（每次 hello 之后都重发状态）；2 秒内没有发送任何内容时发 `ping`；状态快照每秒最多 5 次；
事件紧跟在待发状态之后立即发送；正常退出时发 `clear`。1.1 版增加了每个会话可选的 `act` 和 `ctx`，以及事件 `pass`
（`fail` 只用于测试和构建命令）。当前固件的每个 ack 都带有机器人的 boot id 和已占用的会话槽位数（`n`）：
boot id 改变（重启但 USB 没有重新枚举）或 `n` 与最近一次确认的状态不符（机器人丢掉了会话）时，agent 会重发状态。
不带这些字段的 ack（旧固件）照常接受。

```bat
python -m unittest discover -s <repo>\tools\sc-agent -v
```

测试只用标准库，模拟机器人，从不打开串口。
