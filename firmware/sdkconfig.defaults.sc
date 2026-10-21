# StackChan fork overrides, applied after sdkconfig.defaults (see CMakeLists.txt).
# Kconfig defaults keep the upstream behaviour; this file turns on the fork features.
# Defaults only apply when sdkconfig is generated: delete firmware/sdkconfig (or run `idf.py fullclean`)
# after changing this file.

# Larger assets partition for the MultiNet model.
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_sc.csv"

# Custom wake word recognised by MultiNet7 (Chinese) instead of the WakeNet "Hi, Stack Chan" model.
# CUSTOM_WAKE_WORD is pinyin separated by spaces. CUSTOM_WAKE_WORD_DISPLAY is the wake word text sent to
# the server, kept in Chinese on purpose (user-facing data, not code).
# THRESHOLD: 1-99, lower is more sensitive.
CONFIG_USE_CUSTOM_WAKE_WORD=y
CONFIG_CUSTOM_WAKE_WORD="qi lai gan huo"
CONFIG_CUSTOM_WAKE_WORD_DISPLAY="起来干活"
CONFIG_CUSTOM_WAKE_WORD_THRESHOLD=20
CONFIG_SR_MN_CN_MULTINET7_QUANT=y
CONFIG_SR_WN_WN9_HISTACKCHAN_TTS3=n

# Do not let the OTA server replace self-built firmware or assets (patches/xiaozhi-esp32-fork.patch):
# no automatic upgrade, no server-pushed firmware / assets via MCP.
CONFIG_SC_DISABLE_AUTO_OTA=y
CONFIG_SC_DISABLE_REMOTE_OTA=y

# Idle behaviour director (REQ-002): silent, quiet, awake 08:00-23:00 local time.
CONFIG_SC_DEFAULT_TIMEZONE="AEST-10AEDT,M10.1.0,M4.1.0/3"
CONFIG_SC_IDLE_DIRECTOR=y
CONFIG_SC_IDLE_WAKE_HOUR=8
CONFIG_SC_IDLE_SLEEP_HOUR=23
CONFIG_SC_IDLE_HEAD_MOVES_PER_HOUR=20
# Sleep backlight (REQ-004): dimmest step in a dark room, 2.8 V with the lights on.
CONFIG_SC_IDLE_SLEEP_BACKLIGHT=5
CONFIG_SC_IDLE_SLEEP_BACKLIGHT_LIT=45

# Standby, power and touch fixes (REQ-004).
CONFIG_SC_EXT_POWER_NO_STANDBY=y
CONFIG_SC_TOUCH_FIXES=y

# Claude Code status link over USB (REQ-003, phase 1).
CONFIG_SC_USB_LINK=y

# Drawn faces (REQ-005); a special idle face every 4-10 minutes.
CONFIG_SC_IDLE_FACES=y
CONFIG_SC_IDLE_FACE_MIN_MINUTES=4
CONFIG_SC_IDLE_FACE_MAX_MINUTES=10

# Pixel buddy suit while Claude Code works, head moving along (REQ-007).
CONFIG_SC_CLAUDE_SUIT=y
# Custom colour tuned by eye on the CoreS3 LCD to look like the Claude orange (#D97757) on a monitor.
CONFIG_SC_CLAUDE_SUIT_COLOR=3
CONFIG_SC_CLAUDE_SUIT_CUSTOM_R=255
CONFIG_SC_CLAUDE_SUIT_CUSTOM_G=84
CONFIG_SC_CLAUDE_SUIT_CUSTOM_B=87
CONFIG_SC_CLAUDE_SUIT_OFF_MINUTES=2
CONFIG_SC_CLAUDE_SUIT_HEAD=y
CONFIG_SC_CLAUDE_SUIT_HEAD_SPEED=600

# Settings panel (gear next to Home) and voice settings (REQ-006). The values above are its defaults.
CONFIG_SC_SETTINGS_PANEL=y

# Core dumps to the existing 64 KB coredump partition (read with idf.py coredump-info). Coredump
# log strings would cost ~5 KB of internal RAM, so they are off.
CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y
CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF=y
CONFIG_ESP_COREDUMP_CHECKSUM_CRC32=y
# CONFIG_ESP_COREDUMP_LOGS is not set
