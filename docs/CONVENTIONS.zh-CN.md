# 开发规范

[English](CONVENTIONS.md) | **中文**

适用范围：本 fork（[chunhuan-lu/StackChan](https://github.com/chunhuan-lu/StackChan)），以官方
[m5stack/StackChan](https://github.com/m5stack/StackChan) v1.6.0 固件为基础，重点开发 **AI Agent（小智）模式**。

## 1. 语言

- 和维护者讨论时用中文。
- 代码、注释、标识符、日志字符串、Kconfig 帮助文本、提交信息：**一律用英文**。
- 文档（需求、backlog、开发日志、设计说明、本规范）：**每份文档两个文件**，`name.md`（英文）和 `name.zh-CN.md`（中文）。
  两个文件在同一次提交里一起更新，开头互相链接。
  `CLAUDE.md` 这类给工具读的说明文件只写英文。

## 2. 分支与提交

| 分支 | 用途 |
|---|---|
| `main` | 官方 `m5stack/StackChan` 的镜像，不在上面直接提交 |
| `dev` | 本 fork 的集成分支 |
| `feat/<主题>`、`fix/<主题>`、`docs/<主题>` | 从 `dev` 拉出的工作分支，编译并验证通过后合回 `dev` |

- 提交信息遵循 [Conventional Commits](https://www.conventionalcommits.org/)：`type(scope): summary`。
  type 包括：`feat`、`fix`、`docs`、`refactor`、`build`、`test`、`chore`；scope 比如：`firmware`、`xiaozhi`、`docs`、`tools` 等。
- 同步官方新版本：
  ```
  git fetch upstream
  git switch main && git merge --ff-only upstream/main && git push origin main
  git switch dev && git merge main      # 然后重新编译，并检查小智补丁是否还能打上
  ```
- 远程仓库：`origin` 是本 fork；`upstream` 是官方仓库（已禁止推送）；`artrobin` 只作参考（已禁止推送）。

## 3. 尽量少改官方代码

- 新功能放在**新文件**里；改官方文件时只加小的、有明确标记的挂接点。
- fork 新增的功能用 Kconfig 开关控制，符号统一以 **`SC_`** 开头（即 `CONFIG_SC_...`），每个功能都能单独关掉。
- 改配置能解决的，优先改 `sdkconfig.defaults`，不改代码。

## 4. 修改 xiaozhi-esp32

- 不修改官方的 `firmware/patches/xiaozhi-esp32.patch`。
- 我们对小智的改动单独放进 **`firmware/patches/xiaozhi-esp32-fork.patch`**，在官方补丁之后打上
  （第一次需要时，给 `fetch_repos.py` 加上多补丁支持）。
- `fetch_repos.py` 会重新检出小智源码：`firmware/xiaozhi-esp32/` 里没存进补丁的改动会丢失。
- 如果小智源码里已经打过旧补丁，`fetch_repos.py` 会静默跳过新补丁。需要先还原：
  `git -C firmware/xiaozhi-esp32 checkout -- .`

## 5. 硬件与设备安全

- 俯仰（Y 轴）舵机保持在 5–85° 之间（固件限制为 3–87°）。电机可能通电时，不要用手强行转动活动部件。
- 闲置时舵机不要动得太频繁（SCS0009 的电位器寿命约 10 万次以上），"看"的动作尽量交给屏幕上的眼睛。
- 刷机只用 `idf.py -p COMx flash`，它不会擦掉 NVS（Wi-Fi、舵机校准、小智设备身份）。
  没有最新的完整 flash 备份时，不执行 `erase-flash`，也不刷整包镜像。
- 不要在 StackChan World App 里解绑设备：自编译固件依赖现有的小智绑定。
- 自编译固件必须关闭小智的自动 OTA，否则官方一发新版就会把它覆盖。

## 6. 验证

- 每次提交都必须能用 **ESP-IDF v5.5.4** 编译通过（`idf.py build`）；v5.5.5 已知会编译失败（官方 issue #108）。
- 需要真机验证的内容，在开发日志里记录测了什么、怎么测的、结果如何。
  没有经过真机验证的，不说"完成"；未测试的部分明确标注为未测试。

## 7. 密钥与隐私

- 不提交令牌、密码、密钥、Wi-Fi 信息、flash 备份镜像、`sdkconfig` 和 `sdkconfig.defaults.local`。
- 本地的密钥类配置放在 `firmware/sdkconfig.defaults.local`（已被 git 忽略）或环境变量里。
- 本地有一个不随仓库提交的 pre-commit 钩子，会拦截暂存内容里常见的密钥格式。

## 8. 平台注意事项

- Windows：本仓库使用 `core.autocrlf=false`；`.gitattributes` 保证 `*.patch` 文件不被转换换行（官方 issue #62）。

## 文档目录

```
docs/
  CONVENTIONS.md / CONVENTIONS.zh-CN.md
  backlog.md / backlog.zh-CN.md
  requirements/<编号>-<名称>.md / .zh-CN.md
  devlog/<YYYY-MM-DD>.md / .zh-CN.md
```
