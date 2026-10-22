#!/usr/bin/env bash
# sc-run - run a command and show it on the StackChan robot (REQ-003): working while it runs, then the result.
#
#   sc-run <command> [arguments...]          e.g.  sc-run pytest -q    sc-run idf.py build    sc-run ./deploy.sh
#
# Install as ~/.local/bin/sc-run (chmod +x). Works in WSL and in Git Bash. stdin, stdout and stderr stay on the
# terminal; the output is also written to a temporary file, and its last 16 KB go to sc-agent together with the
# exit code (sc-agent turns test counts such as "42 passed" into the robot's bubble). sc-run exits with the
# command's exit code and prints nothing of its own. No opt-in needed: using sc-run is the opt-in.
# Settings: SC_TOKEN_FILE (default ~/.sc-agent/token), SC_PORT (default 47123).

if [ $# -eq 0 ]; then
  echo "usage: sc-run <command> [arguments...]" >&2
  exit 2
fi

# --- sc-agent connection: keep this block identical in sc-hook.sh and sc-run.sh (test_scripts.py checks it) ---
token_file="${SC_TOKEN_FILE:-$HOME/.sc-agent/token}"
token=""
[ -r "$token_file" ] && token=$(tr -d ' \r\n' < "$token_file")
# Windows curl.exe reaches sc-agent on the Windows loopback, also from WSL through interop. WSL with systemd can
# lose its interop registration (curl.exe then fails with "Exec format error"); WSL's own curl reaches the Windows
# loopback too when networkingMode=mirrored is set in .wslconfig.
interop=0
for entry in /proc/sys/fs/binfmt_misc/WSLInterop*; do
  [ -e "$entry" ] && interop=1
done
curl_bin=""
if [ -n "${WSL_DISTRO_NAME:-}" ] && [ "$interop" = 0 ] && command -v curl >/dev/null; then
  curl_bin=curl  # WSL without working interop: needs mirrored networking
elif command -v curl.exe >/dev/null; then
  curl_bin=curl.exe
elif [ -x /mnt/c/Windows/System32/curl.exe ]; then
  curl_bin=/mnt/c/Windows/System32/curl.exe  # WSL with appendWindowsPath=false
elif command -v curl >/dev/null; then
  curl_bin=curl  # works from WSL only with mirrored networking
fi
# --- end of the shared block ---

post() {  # post <json>: send one event to sc-agent; silent, gives up after 2 s
  [ -n "$token" ] && [ -n "$curl_bin" ] || return 0
  printf '%s' "$1" | "$curl_bin" -s -m 2 --noproxy '*' -X POST -H "X-SC-Token: $token" \
    -H "Content-Type: application/json" --data-binary @- "http://127.0.0.1:${SC_PORT:-47123}/hook" >/dev/null 2>&1
}

json_string() {  # json_string <text>: print text as a JSON string (control characters must be removed before)
  local s=$1
  s=${s//\\/\\\\}
  s=${s//\"/\\\"}
  s=${s//$'\t'/\\t}
  s=${s//$'\n'/\\n}
  printf '"%s"' "$s"
}

clean() {  # drop ANSI escape sequences and control characters other than tab and newline
  LC_ALL=C sed $'s/\x1b\\[[0-9;?]*[ -/]*[@-~]//g' | LC_ALL=C tr -d '\000-\010\013-\037\177'
}

command_text=$(printf '%s' "$*" | clean)
cwd_text=$(printf '%s' "$PWD" | clean)
session="run-$(printf '%s' "$PWD" | md5sum | cut -c1-8)"
fields="\"session_id\":\"$session\",\"cwd\":$(json_string "$cwd_text"),\"command\":$(json_string "$command_text")"

log=$(mktemp "${TMPDIR:-/tmp}/sc-run.XXXXXX") || log=/dev/null
trap 'rm -f "$log"' EXIT

post "{\"hook_event_name\":\"ScRunStart\",$fields}" &
start_pid=$!

# The command gets Ctrl+C, a hang-up or TERM itself; sc-run waits for it and still reports the end.
trap 'true' INT TERM HUP
if [ -t 0 ] && [ -t 1 ] && [ "$log" != /dev/null ] && script --version 2>/dev/null | grep -q util-linux; then
  # A pseudo-terminal keeps colours and interactive programs working; script(1) also writes the log.
  SHELL="$BASH" script -q -e -c "$(printf '%q ' "$@")" "$log"
  code=$?
else
  if [ -t 1 ]; then
    export FORCE_COLOR=1 CLICOLOR_FORCE=1 PY_COLORS=1  # output goes through tee: ask tools to keep colours
  fi
  "$@" 2>&1 | tee "$log"
  code=${PIPESTATUS[0]}
fi
trap - INT TERM HUP

output=$(tail -c 16384 "$log" | clean)
wait "$start_pid" 2>/dev/null  # ScRunStart must not overtake ScRunEnd
post "{\"hook_event_name\":\"ScRunEnd\",$fields,\"exit_code\":$code,\"output\":$(json_string "$output")}"
exit "$code"
