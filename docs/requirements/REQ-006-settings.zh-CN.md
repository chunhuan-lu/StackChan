# REQ-006 设置面板与语音设置

[English](REQ-006-settings.md) | **中文**

状态：已在 `feat/settings` 分支实现，**尚未真机验证** · 对应 backlog：B-021

## 目标

fork 新增的功能（待机导演、表情、睡眠屏幕、Claude Code 联动、插 USB 不待机）的数值都是编译时写死的。维护者希望把它们做成
设置项：有合适的默认值和最大最小值，能在机器人上修改，也方便别人 fork 这个分支或提 PR：

1. 入口：沿用现有的底部上划（Home 弹出），旁边加一个齿轮按钮。不新增手势。
2. 保留所有候选设置项。
3. 支持用语音修改。
4. Kconfig 默认值按官方行为；本 fork 自己的选择放在 `sdkconfig.defaults.sc`。

## 配置模型

| 层 | 内容 | 谁来改 |
|---|---|---|
| Kconfig（`main/Kconfig.projbuild`，菜单 "StackChan fork options"） | 功能开关（`SC_*`，默认 `n` = 官方行为）、默认值和范围 | 编译固件的人（`idf.py menuconfig`、`sdkconfig.defaults*`） |
| `firmware/sdkconfig.defaults.sc` | 本 fork 的选择：功能全开、墨尔本时区、睡眠屏幕 5 / 45 | fork 维护者 |
| NVS 命名空间 `sc_cfg` | 在设备上（面板或语音）改过的值，总是限制在范围内 | 用户；「Reset to defaults」恢复为 Kconfig 的值 |

「Activity（活跃程度）」就是官方的待机动作等级，仍然存在官方的位置（小智配置）里，所以设置 App 和面板显示的永远是同一个值。
它只在启用待机导演时列出（官方的待机动作只在 AI Agent 启动时读一次等级），并且每次启动 AI Agent 时重新读取。
没有开启 `SC_SETTINGS_PANEL` 时使用 Kconfig 的值，NVS 里残留的值会被忽略。

## 设置项

只列出已编译进固件的功能对应的设置项。

| 键 | 面板标签 | 范围（步长） | 默认值（Kconfig） | 依赖 |
|---|---|---|---|---|
| `activity` | Activity | Off / Low / Medium / High | 官方设置 | `SC_IDLE_DIRECTOR` |
| `head_moves` | Head moves / hour | 0–120（5） | `SC_IDLE_HEAD_MOVES_PER_HOUR` 20 | `SC_IDLE_DIRECTOR` |
| `wake_hour` | Wake up at | 0–23 点 | `SC_IDLE_WAKE_HOUR` 8 | `SC_IDLE_DIRECTOR` |
| `sleep_hour` | Go to sleep at | 0–23 点 | `SC_IDLE_SLEEP_HOUR` 23 | `SC_IDLE_DIRECTOR` |
| `dark_lux` | Dark below | 0–100 lux | `SC_IDLE_DARK_LUX` 3 | `SC_IDLE_DIRECTOR` |
| `bright_lux` | Light above | 1–200 lux | `SC_IDLE_BRIGHT_LUX` 8 | `SC_IDLE_DIRECTOR` |
| `sleep_bl` | Sleep screen, dark | 0–100 %（5） | `SC_IDLE_SLEEP_BACKLIGHT` 5 | `SC_IDLE_DIRECTOR` |
| `sleep_bl_lit` | Sleep screen, lit | 0–100 %（5） | `SC_IDLE_SLEEP_BACKLIGHT_LIT` 45 | `SC_IDLE_DIRECTOR` |
| `led_max` | Light brightness | 0–255（4） | `SC_IDLE_LED_MAX` 24 | `SC_IDLE_DIRECTOR` |
| `face_min` | Special face, from | 1–60 分钟 | `SC_IDLE_FACE_MIN_MINUTES` 4 | `SC_IDLE_DIRECTOR` |
| `face_max` | Special face, to | 1–60 分钟 | `SC_IDLE_FACE_MAX_MINUTES` 10 | `SC_IDLE_DIRECTOR` |
| `faces` | Expressive faces | 开 / 关 | `SC_IDLE_FACES` y | `SC_IDLE_DIRECTOR` |
| `tap_wake` | Tap wakes me up | 开 / 关 | 开 | `SC_IDLE_DIRECTOR` + `SC_TOUCH_FIXES` |
| `claude` | Show Claude Code | 开 / 关 | 开 | `SC_IDLE_DIRECTOR` + `SC_USB_LINK` |
| `claude_suit` | Pixel buddy suit | 开 / 关 | `SC_CLAUDE_SUIT` y | `SC_IDLE_DIRECTOR` + `SC_USB_LINK`（REQ-007） |
| `suit_off_min` | Suit off after | 1–30 分钟 | `SC_CLAUDE_SUIT_OFF_MINUTES` 2 | 同上 |
| `suit_head` | Head moves along | 开 / 关 | `SC_CLAUDE_SUIT_HEAD` y | 同上 |
| `suit_head_spd` | Head speed | 100–1000（50） | `SC_CLAUDE_SUIT_HEAD_SPEED` 600 | 同上 |
| `usb_awake` | No standby on USB | 开 / 关 | 开 | `SC_EXT_POWER_NO_STANDBY` |

成对的设置保持一致：「Light above」始终高于「Dark below」，「Special face, to」不会小于「from」（开机时保存的值和新的
Kconfig 默认值对不上时也会纠正）。关掉 `faces` 恢复豆豆眼和原来的
眼部动画（斜眼看灯、无声哼歌、满足眯眼）；关掉 `claude` 停止会话灯、Claude 表情和事件反应（USB 通道照常运行）；关掉
`usb_awake` 恢复官方的省电规则。所有修改立即生效，不需要重启。

## 面板

- AI Agent 待机时：从底部边缘上划 → Home 按钮滑出，右边多一个齿轮 → 点齿轮。
- 全屏列表，按 Idle / Sleep / Faces / Claude Code / Power 分组：数值和选项用 − / + 按钮（按住连续调），开关项用开关。改动立即生效；
  最后一次改动 1 秒后以及关闭面板时写入 NVS（否则按住 + / − 每秒要提交十次 NVS）。
- 「Reset to defaults」需要 3 秒内再点一次确认。✕ 关闭；60 秒没有触摸也会自动关闭。
- 面板打开时用语音改的值，0.5 秒内刷新显示。
- 面板打开期间机器人保持清醒；在睡着的脸上打开面板会把它叫醒。
- 没有任何可设置项编译进来时，不显示齿轮。

## 语音（MCP 工具）

- `self.robot.get_settings`：返回列表，每项有 key、label、当前值、显示文字、min、max、默认值、说明（以及选项 / on_off）。
- `self.robot.set_setting(key, value)`：限制范围、保存，返回实际生效的值；未知的 key 返回错误提示，让模型先调用 `get_settings`。

例子：「睡觉的时候屏幕再暗一点」→ 模型先调用 `get_settings`，再调用 `set_setting("sleep_bl_lit", 30)`。

## 实现

| 文件 | 改动 |
|---|---|
| `main/stackchan/sc/sc_settings.*` | 设置表、原子变量、NVS 存储、范围限制、成对规则、版本计数 |
| `main/stackchan/sc/settings_panel.*` | 画在 `lv_layer_top()` 上的 LVGL 面板 |
| `main/hal/sc/settings_mcp.*` | 两个 MCP 工具 |
| `main/apps/common/home_indicator/*` | 齿轮按钮、`view::set_home_indicator_settings()`（不调用就不生效） |
| `main/hal/hal.cpp`、`main/hal/hal_mcp.cpp` | `SC_SETTINGS_PANEL` 控制的挂钩 |
| `main/stackchan/sc/idle_director.cpp` | 运行时读取设置，不再用 `CONFIG_SC_IDLE_*` |
| `main/hal/board/stackchan.cc`、`stackchan_display.cc` | `usb_awake` 和暗处睡眠亮度改为运行时读取 |
| `main/Kconfig.projbuild` | 分子菜单；默认值 = 官方行为；新增 `SC_IDLE_FACES`、`SC_IDLE_FACE_MIN/MAX_MINUTES`、`SC_SETTINGS_PANEL` |
| `sdkconfig.defaults.sc` | 打开 fork 功能和设置面板 |

已有编译目录的注意事项：`sdkconfig` 会保留原来的值；`idf.py reconfigure` 会从 `sdkconfig.defaults.sc` 补上新增的符号。

## 验收标准（真机）

1. AI Agent 待机时上划：Home 和齿轮一起滑出；齿轮打开面板；Home 仍然能回主页。
2. 每一行都能在范围内调整，重启后保留。
3. 面板里改的 `activity`，在「设置 → AI Agent」里能看到，反过来也一样（离开设置会重启一次）。
4. 立即生效：`led_max` 马上改变心情灯，关掉 `faces` 显示豆豆眼，关掉 `claude` 会话灯熄灭，`sleep_bl_lit` 改变睡眠时的屏幕亮度。
5. 「Reset to defaults」（点两次）恢复 Kconfig 的值。
6. 语音：要求修改某个设置时会调用 `set_setting`，面板和行为随之变化。
7. 所有 `SC_*` 选项关闭编译时，行为和官方一致（没有齿轮，没有工具）。
