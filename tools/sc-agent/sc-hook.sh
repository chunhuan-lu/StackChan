#!/usr/bin/env bash
# sc-hook.sh - forward a Claude Code hook event to sc-agent (StackChan status robot, REQ-003).
#
# Install as ~/.claude/sc-hook.sh (chmod +x) and register it as an async command hook (hooks-settings.json).
# Works in WSL and in Git Bash on Windows. Reads the hook JSON from stdin, never prints anything, always exits 0.
#
# Opt-in per session:
#   SC_LINK=1                                    always on
#   SC_LINK=0                                    always off
#   SC_LINK unset or empty                       on when "$CLAUDE_PROJECT_DIR/.claude/stackchan" exists
#                                                (or "$PWD/.claude/stackchan" when CLAUDE_PROJECT_DIR is unset)
# Settings:
#   SC_TOKEN_FILE  token file (default ~/.sc-agent/token; in WSL copy it once from the Windows profile)
#   SC_PORT        sc-agent HTTP port (default 47123)

exec >/dev/null 2>&1

case "${SC_LINK:-}" in
  1) ;;
  "") [ -e "${CLAUDE_PROJECT_DIR:-$PWD}/.claude/stackchan" ] || exit 0 ;;
  *) exit 0 ;;
esac

token_file="${SC_TOKEN_FILE:-$HOME/.sc-agent/token}"
[ -r "$token_file" ] || exit 0
token=$(tr -d ' \r\n' < "$token_file")
[ -n "$token" ] || exit 0

# Windows curl.exe reaches sc-agent on the Windows loopback, also from WSL through interop. WSL with systemd can
# lose its interop registration (curl.exe then fails with "Exec format error"); WSL's own curl reaches the Windows
# loopback too when networkingMode=mirrored is set in .wslconfig.
interop=0
for entry in /proc/sys/fs/binfmt_misc/WSLInterop*; do
  [ -e "$entry" ] && interop=1
done
if [ -n "${WSL_DISTRO_NAME:-}" ] && [ "$interop" = 0 ] && command -v curl >/dev/null; then
  curl_bin=curl  # WSL without working interop: needs mirrored networking
elif command -v curl.exe >/dev/null; then
  curl_bin=curl.exe
elif [ -x /mnt/c/Windows/System32/curl.exe ]; then
  curl_bin=/mnt/c/Windows/System32/curl.exe  # WSL with appendWindowsPath=false
elif command -v curl >/dev/null; then
  curl_bin=curl  # works from WSL only with mirrored networking
else
  exit 0
fi

payload=$(cat)

# Add "project_dir" so the robot shows the project folder even when the session's cwd moved into a
# subdirectory. Pure bash: only for compact JSON objects starting with {" (as Claude Code sends them).
if [ -n "${CLAUDE_PROJECT_DIR:-}" ]; then
  dir=${CLAUDE_PROJECT_DIR//[[:cntrl:]]/}
  dir=${dir//\\/\\\\}
  dir=${dir//\"/\\\"}
  case "$payload" in
    '{"'*) payload="{\"project_dir\":\"$dir\",${payload#\{}" ;;
  esac
fi

printf '%s' "$payload" | "$curl_bin" -s -m 2 --noproxy '*' -X POST \
  -H "X-SC-Token: $token" -H "Content-Type: application/json" \
  --data-binary @- "http://127.0.0.1:${SC_PORT:-47123}/hook"
exit 0
