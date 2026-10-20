# REQ-001 自定义唤醒词"起来干活"

[English](REQ-001-custom-wake-word.md) | **中文**

状态：已在 `feat/wake-word` 分支实现，**尚未在真机上验证** · 对应 backlog：B-001、B-002、B-003

## 目标

在 AI Agent 模式下，用中文"起来干活"（拼音 `qi lai gan huo`）唤醒机器人，取代原来的 "Hi, Stack Chan"。

## 背景

- 现在的唤醒词用的是 WakeNet 模型（`wn9_histackchan_tts3`，289 KB）。WakeNet 只有乐鑫训练过的那些固定唤醒词，"起来干活"不在其中。
- 任意短语需要用 MultiNet，也就是通用的中文命令词识别模型。`mn7_cn` 有 2617 KB。
- 4 MB 的资源分区里已经放了约 1.8 MB 的中文字体，放不下 `mn7_cn`。
- 小智在运行时选择唤醒方式：资源分区里有 MultiNet 模型就用 `CustomWakeWord`，没有就用 WakeNet（见 `audio_service.cc`）。
  编译脚本会根据 `sdkconfig` 把模型和唤醒词配置一起打包进资源分区。

## 决策

用 MultiNet7（`CONFIG_SR_MN_CN_MULTINET7_QUANT`），同时扩大资源分区。未采用的方案：改用现成的 WakeNet 唤醒词（不是想要的词）；
在原有分区里放 `mn5q8_cn`（模型较旧，分区几乎被占满）；找乐鑫付费定制 WakeNet（要花钱、周期长）。

## 改动

| 范围 | 改动 |
|---|---|
| 分区表 | 新增 `firmware/partitions_sc.csv`：资源分区从 4096 KB 扩到 6080 KB，coredump 挪到 `0xFF0000`。NVS、otadata、phy_init、ota_0、ota_1 都不变。 |
| 配置 | 新增 `firmware/sdkconfig.defaults.sc`，在 `sdkconfig.defaults` 之后加载：指定分区表文件、自定义唤醒词、MultiNet7、关闭 WakeNet、`SC_DISABLE_AUTO_OTA`。 |
| Kconfig | `main/Kconfig.projbuild`：新增菜单 "StackChan fork options"，包含 `SC_DISABLE_AUTO_OTA`（默认开启）。 |
| 小智 | `patches/xiaozhi-esp32-fork.patch`：开启 `SC_DISABLE_AUTO_OTA` 时，`CheckNewVersion()` 发现新版本只记日志、不安装；资源打包脚本改为按 UTF-8 读取 `sdkconfig`（原来按 Windows 代码页读，中文显示文本会变成乱码）。 |
| 工具 | `fetch_repos.py`：一个仓库可以打多个补丁；重复运行不会出错（在 `.git` 里记录已打的补丁组）；补丁打不上时报错，不再静默跳过。`tools/xiaozhi_fork_patch.py`：从小智的工作目录重新生成 fork 补丁。 |

## 验收标准（真机）

1. 用 `idf.py -p COMx flash` 刷机后，不需要重新配网就能连上 Wi-Fi（NVS 保留），进入 AI Agent 模式时也不会跳到 App 配网页面（现有的小智绑定仍然有效）。
2. 启动日志显示资源分区挂载无报错，自定义唤醒词和 MultiNet 模型加载成功。
3. 在安静的房间里，距离约 1 米说"起来干活"，10 次里至少唤醒 8 次。
4. 在旁边正常聊天或播放视频 30 分钟，没有误唤醒（记录误唤醒次数）。
5. 对话和以前一样正常（音色、智能体设置）。
6. OTA 服务器返回更新的版本时，日志显示 "automatic OTA disabled"，并且不会安装（要等官方发布更新版本后才能验证）。

## 风险

- 用 MultiNet 做唤醒，准确率不如专门的 WakeNet 模型，可能需要调整 `CUSTOM_WAKE_WORD_THRESHOLD`（默认 20）。
- "起来干活"是日常用语，正常说话时可能误唤醒。
- MultiNet 比 WakeNet 占用更多 CPU 和 PSRAM（只在待机时运行）。
- 分区布局和官方不一致了（反正官方 OTA 也已经关闭）。

## 回退

恢复完整的 flash 备份（`write_flash 0 flash_full_16MB.bin`），分区表、固件、资源和 NVS 都会回到备份时的状态。
也可以用 `idf.py flash` 刷一个未修改的官方源码编译的固件。
