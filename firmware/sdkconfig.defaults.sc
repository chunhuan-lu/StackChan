# StackChan fork overrides, applied after sdkconfig.defaults (see CMakeLists.txt).
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

# Do not let the OTA server replace self-built firmware (patches/xiaozhi-esp32-fork.patch).
CONFIG_SC_DISABLE_AUTO_OTA=y

# Idle behaviour director (REQ-002): silent, quiet, awake 08:00-23:00 local time.
CONFIG_SC_DEFAULT_TIMEZONE="CST-8"
CONFIG_SC_IDLE_DIRECTOR=y
CONFIG_SC_IDLE_WAKE_HOUR=8
CONFIG_SC_IDLE_SLEEP_HOUR=23
CONFIG_SC_IDLE_HEAD_MOVES_PER_HOUR=20
