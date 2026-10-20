# REQ-001 Custom wake word "起来干活"

**English** | [中文](REQ-001-custom-wake-word.zh-CN.md)

Status: implemented on `feat/wake-word`, **not yet verified on the device** · Backlog: B-001, B-002, B-003

## Goal

Wake the robot in AI Agent mode with the Chinese phrase "起来干活" (pinyin `qi lai gan huo`) instead of "Hi, Stack Chan".

## Background

- The current wake word uses a WakeNet model (`wn9_histackchan_tts3`, 289 KB). WakeNet models exist only for phrases
  trained by Espressif; "起来干活" is not one of them.
- Arbitrary phrases need MultiNet, a general Chinese command recogniser. `mn7_cn` is 2617 KB.
- The 4 MB assets partition already holds a ~1.8 MB Chinese font, so `mn7_cn` does not fit.
- xiaozhi selects the wake word engine at runtime: if the assets contain a MultiNet model it uses `CustomWakeWord`,
  otherwise WakeNet (`audio_service.cc`). The build script packages the model and the wake word config from `sdkconfig`.

## Decision

MultiNet7 (`CONFIG_SR_MN_CN_MULTINET7_QUANT`) plus a larger assets partition. Rejected: an existing WakeNet phrase
(not the desired phrase), `mn5q8_cn` in the old partition (older model, partition almost full), a paid custom WakeNet
(cost and lead time).

## Changes

| Area | Change |
|---|---|
| Partition table | New `firmware/partitions_sc.csv`: assets 4096 KB → 6080 KB, coredump moved to `0xFF0000`. NVS, otadata, phy_init, ota_0 and ota_1 unchanged. |
| Config | New `firmware/sdkconfig.defaults.sc`, loaded after `sdkconfig.defaults`: partition file, custom wake word, MultiNet7, WakeNet off, `SC_DISABLE_AUTO_OTA`. |
| Kconfig | `main/Kconfig.projbuild`: menu "StackChan fork options" with `SC_DISABLE_AUTO_OTA` (default y). |
| xiaozhi | `patches/xiaozhi-esp32-fork.patch`: `CheckNewVersion()` only logs a newer version when `SC_DISABLE_AUTO_OTA` is set; the assets build script reads `sdkconfig` as UTF-8 (the Chinese display text was decoded with the Windows code page). |
| Tooling | `fetch_repos.py`: several patches per repository, idempotent re-runs (patch-stack marker in `.git`), errors instead of silent skips. `tools/xiaozhi_fork_patch.py` regenerates the fork patch from the xiaozhi working tree. |

## Acceptance criteria (on the device)

1. After `idf.py -p COMx flash`, the robot connects to Wi-Fi without provisioning (NVS kept) and enters AI Agent mode
   without being sent to App setup (existing xiaozhi binding still valid).
2. Boot log shows the assets partition mounted without errors and the custom wake word / MultiNet model loaded.
3. "起来干活" spoken at about 1 m in a quiet room wakes the robot in at least 8 of 10 attempts.
4. No false wake-ups during 30 minutes of normal conversation or video sound nearby (record the count).
5. Conversation works as before (voice, agent settings).
6. When the OTA server reports a newer version, the log shows "automatic OTA disabled" and nothing is installed
   (checkable only once a newer official release exists).

## Risks

- MultiNet as a wake word is less accurate than a dedicated WakeNet model; `CUSTOM_WAKE_WORD_THRESHOLD` (default 20)
  may need tuning.
- "起来干活" is an everyday phrase and may trigger during normal talk.
- MultiNet costs more CPU and PSRAM than WakeNet (it only runs while idle).
- The partition layout now differs from upstream (official OTA is disabled anyway).

## Rollback

Restore the full flash backup (`write_flash 0 flash_full_16MB.bin`): partition table, firmware, assets and NVS return
to the backed-up state. Alternatively flash an unmodified upstream build with `idf.py flash`.
