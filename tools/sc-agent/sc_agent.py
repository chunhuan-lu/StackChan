"""sc-agent: bridge between Claude Code hooks and the StackChan robot over USB (REQ-003, phase 1).

Runs on Windows. Receives hook events from sc-hook.sh on 127.0.0.1, keeps the session table and drives the robot
through the USB link protocol v1 (sc_link.py). It also owns the robot's COM port: every non-protocol line goes to
a daily serial log, and the port can be released for flashing (sc-idf.ps1).

    pythonw sc_agent.py             background; logs to %USERPROFILE%\\.sc-agent\\agent.log
    python  sc_agent.py --console   foreground, also logs to the console; Ctrl+C stops
"""

from __future__ import annotations

import argparse
import hmac
import json
import logging
import os
import re
import secrets
import signal
import socket
import socketserver
import sys
import threading
import time
from collections import deque
from dataclasses import dataclass, field
from datetime import datetime, timedelta
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from logging.handlers import RotatingFileHandler
from pathlib import Path
from typing import Any, Callable, Protocol, TextIO
from urllib.parse import parse_qs, urlsplit

from results import CommandDetector
from sc_link import encode_frame, split_line
from sessions import QuietHours, SessionTable, quiet_hours_from_config

AGENT_VERSION = "0.1.0"
DEFAULT_HTTP_PORT = 47123
ROBOT_VID = 0x303A
ROBOT_PID = 0x1001
MAX_HOOK_BODY = 8 * 1024 * 1024  # PostToolUseFailure carries the tool input, which can be large
MAX_LINE = 16 * 1024  # serial data without a newline beyond this is logged as one line
EVENT_MAX_AGE = 10.0  # seconds; older robot events are dropped instead of being shown late
COMMAND_MAX_AGE = 60.0  # seconds; a command (e.g. start the AI Agent) not sent by then is dropped

log = logging.getLogger("sc.agent")


# --- configuration and data directory ------------------------------------------------------------------------


class ConfigError(Exception):
    pass


@dataclass(frozen=True)
class Config:
    http_port: int = DEFAULT_HTTP_PORT
    usb_vid: int = ROBOT_VID
    usb_pid: int = ROBOT_PID
    serial_port: str | None = None  # fixed COM port instead of the VID:PID search
    serial_number: str | None = None  # USB serial number of the robot: preferred, never skipped as silent
    test_patterns: tuple[str, ...] = ()  # extra regexes for test commands (see results.py)
    quiet: QuietHours = field(default_factory=QuietHours)
    serial_log_days: int = 30  # 0 keeps serial logs forever
    release_timeout_s: float = 1800.0  # re-acquire a released port after this long; 0 = wait for /acquire


_CONFIG_KEYS = {
    "http_port", "usb_vid", "usb_pid", "serial_port", "serial_number", "quiet_days", "quiet_work", "quiet_night",
    "serial_log_days", "release_timeout_s", "test_patterns",
}


def default_data_dir() -> Path:
    return Path.home() / ".sc-agent"


def load_config(path: Path) -> Config:
    """Read config.json (all keys optional; keys starting with "_" are comments)."""
    if not path.exists():
        return Config()
    try:
        data = json.loads(path.read_text(encoding="utf-8-sig"))  # -sig: tolerate a BOM from Notepad
        if not isinstance(data, dict):
            raise ValueError("top level must be a JSON object")
        unknown = sorted(key for key in data if key not in _CONFIG_KEYS and not key.startswith("_"))
        if unknown:
            log.warning("config.json: ignoring unknown keys %s", ", ".join(unknown))
        http_port = int(data.get("http_port", DEFAULT_HTTP_PORT))
        if not 1 <= http_port <= 65535:
            raise ValueError(f"http_port {http_port} is not in 1..65535")
        port = data.get("serial_port")
        serial_number = data.get("serial_number")
        patterns = data.get("test_patterns", [])
        if not isinstance(patterns, list) or not all(isinstance(item, str) for item in patterns):
            raise ValueError("test_patterns must be a list of regular expressions")
        for pattern in patterns:
            try:
                re.compile(pattern)
            except re.error as exc:
                raise ValueError(f"test_patterns: {pattern!r}: {exc}") from exc
        return Config(
            http_port=http_port,
            usb_vid=_hex_or_int(data.get("usb_vid", ROBOT_VID)),
            usb_pid=_hex_or_int(data.get("usb_pid", ROBOT_PID)),
            serial_port=str(port) if port else None,
            serial_number=str(serial_number) if serial_number else None,
            test_patterns=tuple(patterns),
            quiet=quiet_hours_from_config(data),
            serial_log_days=int(data.get("serial_log_days", 30)),
            release_timeout_s=float(data.get("release_timeout_s", 1800.0)),
        )
    except (OSError, ValueError, TypeError) as exc:
        raise ConfigError(f"{path}: {exc}") from exc


def _hex_or_int(value: Any) -> int:
    return int(value, 16) if isinstance(value, str) else int(value)


def load_or_create_token(path: Path) -> str:
    """Return the shared secret for X-SC-Token, creating it on first run. The token is never logged."""
    try:
        token = path.read_text(encoding="utf-8-sig").strip()
        if token:
            return token
    except FileNotFoundError:
        pass
    token = secrets.token_hex(16)
    path.write_text(token, encoding="utf-8")
    if os.name != "nt":
        os.chmod(path, 0o600)
    log.info("created a new token in %s", path)
    return token


class SerialLog:
    """Daily file logs/serial-YYYYMMDD.log with every non-protocol line from the robot, prefixed with local time."""

    def __init__(self, directory: Path, keep_days: int = 30) -> None:
        self._dir = directory
        self._keep_days = keep_days
        self._lock = threading.Lock()
        self._day = ""
        self._file: TextIO | None = None
        self._failed = False

    def write(self, text: str) -> None:
        now = datetime.now()
        with self._lock:
            try:
                day = now.strftime("%Y%m%d")
                if day != self._day or self._file is None:
                    self._file = self._open(day)
                self._file.write(f"[{now:%H:%M:%S}.{now.microsecond // 1000:03d}] {text}\n")
            except OSError as exc:
                if not self._failed:
                    log.error("cannot write the serial log: %s", exc)
                    self._failed = True

    def marker(self, text: str) -> None:
        self.write(f"--- {text}")

    def close(self) -> None:
        with self._lock:
            if self._file is not None:
                self._file.close()
            self._file, self._day = None, ""

    def _open(self, day: str) -> TextIO:
        if self._file is not None:
            self._file.close()
            self._file = None
        self._dir.mkdir(parents=True, exist_ok=True)
        file = open(self._dir / f"serial-{day}.log", "a", encoding="utf-8", buffering=1)
        self._day = day
        self._remove_old()
        return file

    def _remove_old(self) -> None:
        if self._keep_days <= 0:
            return
        cutoff = (datetime.now() - timedelta(days=self._keep_days)).strftime("%Y%m%d")
        for path in self._dir.glob("serial-*.log"):
            stamp = path.stem[len("serial-") :]
            if len(stamp) == 8 and stamp.isdigit() and stamp < cutoff:
                try:
                    path.unlink()
                except OSError:
                    pass


# --- serial backend ------------------------------------------------------------------------------------------


class SerialPort(Protocol):
    def read(self, size: int) -> bytes: ...

    def write(self, data: bytes) -> int | None: ...

    def close(self) -> None: ...


@dataclass(frozen=True)
class PortInfo:
    device: str  # e.g. "COM12"
    serial_number: str | None = None  # USB serial number (the chip's MAC on the ESP32-S3)
    pinned: bool = False  # configured explicitly: never skipped for not answering


class SerialBackend(Protocol):
    write_timeout_error: type[BaseException]

    def find_ports(self) -> list[PortInfo]: ...

    def open(self, name: str) -> SerialPort: ...


class PySerialBackend:
    """Real serial access through pyserial, imported here so the rest of the module works without it."""

    def __init__(self, vid: int, pid: int, fixed_port: str | None = None) -> None:
        import serial
        import serial.tools.list_ports

        self._serial = serial
        self._list_ports = serial.tools.list_ports
        self._vid, self._pid, self._fixed_port = vid, pid, fixed_port
        self.write_timeout_error: type[BaseException] = serial.SerialTimeoutException

    def find_ports(self) -> list[PortInfo]:
        """Candidate ports: the configured serial_port, or every port with the robot's VID:PID."""
        if self._fixed_port:
            return [PortInfo(self._fixed_port, pinned=True)]
        found = [
            PortInfo(p.device, p.serial_number or None)
            for p in self._list_ports.comports()
            if p.vid == self._vid and p.pid == self._pid
        ]
        return sorted(found, key=lambda p: p.device)

    def open(self, name: str) -> SerialPort:
        # Never pass the port to the constructor and never touch DTR/RTS after open(): on the
        # USB-Serial/JTAG port those lines reset the chip.
        port = self._serial.Serial()
        port.port = name
        port.baudrate = 115200
        port.timeout = 0.05
        port.write_timeout = 0.5
        port.dtr = False
        port.rts = False
        port.open()
        return port


# --- USB link ------------------------------------------------------------------------------------------------


@dataclass(frozen=True)
class LinkTiming:
    ack_timeout: float = 0.5  # per attempt
    retries: int = 2  # extra attempts with the same id
    ping_interval: float = 2.0  # keepalive when nothing else was sent
    state_interval: float = 0.2  # at most 5 state snapshots per second
    discovery_interval: float = 1.0  # port search / reopen period
    hello_interval: float = 2.0  # hello retry period while the robot does not answer
    silent_hellos: int = 5  # unanswered hellos before an unknown port is closed ...
    port_cooldown: float = 60.0  # ... and skipped for this long


class UsbLink:
    """Owns the robot's COM port: discovery, a reader thread, the stop-and-wait sender, release/acquire.

    Discovery prefers the robot's USB serial number (configured, or remembered from the last port that answered
    hello). A port that never answered hello in this run is closed after a few unanswered hellos and skipped for
    a while, so another ESP32-S3 board with the same VID:PID is not held forever.

    Lock order: self._cond may be held while calling state_provider (which takes the session table lock);
    nothing calls into the link while holding the table lock.
    """

    def __init__(
        self,
        backend: SerialBackend,
        state_provider: Callable[[], dict[str, Any]],
        serial_log: SerialLog,
        *,
        timing: LinkTiming = LinkTiming(),
        release_timeout_s: float = 1800.0,
        robot_serial: str | None = None,
    ) -> None:
        self._backend = backend
        self._state_provider = state_provider
        self._serial_log = serial_log
        self._timing = timing
        self._release_timeout_s = release_timeout_s
        self._agent_name = f"sc-agent/{AGENT_VERSION}"
        self._write_lock = threading.Lock()
        self._thread: threading.Thread | None = None
        # Link thread only:
        self._problem = ""  # last reported discovery problem, to log each one once
        self._hello_warned = False
        self._hello_failures = 0  # unanswered hellos on the current port
        self._cooldown: dict[str, float] = {}  # device -> monotonic time until which it is skipped
        self._candidates = 0  # number of matching ports at the last selection
        self._config_serial = robot_serial
        self._cond = threading.Condition()
        # Everything below is guarded by self._cond.
        self._stopping = False
        self._released = False
        self._released_at = 0.0
        self._busy = False  # the link thread is opening or serving a port
        self._port: SerialPort | None = None
        self._port_name: str | None = None
        self._port_info: PortInfo | None = None
        self._robot_serial = robot_serial  # preferred serial number: configured or last answering port
        self._known: set[str] = set()  # serial numbers (or devices) that answered hello in this run
        self._closing = False
        self._port_error = False
        self._abandon = False  # the current port stays silent: close it and try another one
        self._linked = False
        self._next_hello_at = 0.0
        self._state_dirty = True
        self._dirty_since = 0.0  # when the state became dirty
        self._force_state = True
        self._last_state: dict[str, Any] | None = None
        self._confirmed_count: int | None = None  # sessions in the last state the robot acknowledged
        self._count_warned = False
        self._last_state_at = 0.0
        self._last_tx_at = 0.0
        self._events: deque[tuple[float, dict[str, Any]]] = deque(maxlen=16)
        self._commands: deque[tuple[float, dict[str, Any]]] = deque(maxlen=4)
        self._next_id = 1
        self._pending_id: int | None = None
        self._ack: dict[str, Any] | None = None
        self._robot: dict[str, Any] = {}
        self._stats = {"tx": 0, "retries": 0, "timeouts": 0, "acks": 0, "nacks": 0, "bad_frames": 0}

    # --- public API (any thread) ---

    def start(self) -> None:
        self._thread = threading.Thread(target=self._run, name="usb-link", daemon=True)
        self._thread.start()

    def stop(self, timeout: float = 3.0) -> None:
        with self._cond:
            self._stopping = True
            self._cond.notify_all()
        if self._thread is not None:
            self._thread.join(timeout)

    def notify_state(self) -> None:
        with self._cond:
            if not self._state_dirty:
                self._dirty_since = time.monotonic()
                self._state_dirty = True
            self._cond.notify_all()

    def queue_event(self, event: dict[str, Any]) -> None:
        with self._cond:
            self._events.append((time.monotonic(), dict(event)))
            self._cond.notify_all()

    def queue_command(self, command: dict[str, Any]) -> None:
        """A robot command such as {"t": "agent"}; sent once linked, dropped after COMMAND_MAX_AGE."""
        with self._cond:
            self._commands.append((time.monotonic(), dict(command)))
            self._cond.notify_all()

    def release(self, timeout: float = 5.0) -> tuple[bool, str | None]:
        """Close the port and pause until acquire(); returns (closed in time, robot port name)."""
        with self._cond:
            was_open = self._port is not None
            self._released = True
            self._released_at = time.monotonic()
            self._cond.notify_all()
            done = self._cond.wait_for(lambda: not self._busy, timeout)
            name = self._port_name if was_open else None
        if name is None:
            try:
                ports = self._backend.find_ports()
                with self._cond:
                    name = min(ports, key=self._rank).device if ports else None
            except Exception:  # only informational for the caller
                name = None
        log.info("port released (%s)%s", name or "robot not present", "" if done else ", but still busy")
        return done, name

    def acquire(self) -> None:
        with self._cond:
            was_released = self._released
            self._released = False
            self._cond.notify_all()
        if was_released:
            log.info("port acquire requested")

    def status(self) -> dict[str, Any]:
        with self._cond:
            if self._released:
                link = "released"
            elif self._port is None:
                link = "searching"
            elif self._linked:
                link = "linked"
            else:
                link = "connecting"
            info = self._port_info if self._port is not None else None
            return {
                "link": link,
                "port": info.device if info else None,
                "serial": info.serial_number if info else None,
                "robot": dict(self._robot),
                "stats": dict(self._stats),
            }

    # --- link thread ---

    def _run(self) -> None:
        while True:
            with self._cond:
                if self._stopping:
                    return
                if self._released and not self._auto_acquire_due():
                    self._cond.wait(1.0)
                    continue
                self._busy = True
            try:
                info, port = self._open_robot_port()
                if info is not None and port is not None:
                    self._serve(info, port)
            except Exception:  # keep the link alive whatever happens
                log.exception("USB link error")
            finally:
                with self._cond:
                    self._busy = False
                    self._cond.notify_all()
            self._sleep(self._timing.discovery_interval)

    def _auto_acquire_due(self) -> bool:  # self._cond held
        if self._release_timeout_s <= 0 or time.monotonic() - self._released_at < self._release_timeout_s:
            return False
        log.warning("port released for %.0f s without /acquire, taking it back", self._release_timeout_s)
        self._released = False
        return True

    def _sleep(self, seconds: float) -> None:
        with self._cond:
            self._cond.wait_for(lambda: self._stopping, seconds)

    def _report(self, problem: str) -> None:
        if problem != self._problem:
            log.info("%s", problem)
            self._problem = problem

    @staticmethod
    def _key(info: PortInfo) -> str:
        return info.serial_number or info.device

    def _rank(self, info: PortInfo) -> tuple[int, str]:
        """Sort key: the robot's serial number first, then ports that answered in this run, then by name."""
        if self._robot_serial and info.serial_number == self._robot_serial:
            return 0, info.device
        return (1 if self._key(info) in self._known else 2), info.device

    def _trusted(self, info: PortInfo) -> bool:
        """Known robot ports are held even while silent (firmware without the link, a reboot, ...)."""
        if info.pinned or self._key(info) in self._known:
            return True
        return bool(self._config_serial) and info.serial_number == self._config_serial

    def _open_robot_port(self) -> tuple[PortInfo | None, SerialPort | None]:
        ports = self._backend.find_ports()
        self._candidates = len(ports)
        if not ports:
            self._report("robot USB port not found, waiting for it")
            return None, None
        now = time.monotonic()
        with self._cond:
            ready = [p for p in ports if self._trusted(p) or self._cooldown.get(p.device, 0.0) <= now]
            ready.sort(key=self._rank)
            preferred = {p.device for p in ready if self._trusted(p) or self._rank(p)[0] == 0}
        if not ready:
            self._report(f"no robot answers on {', '.join(p.device for p in ports)}; retrying after the cool-down")
            return None, None
        for info in ready:
            try:
                port = self._backend.open(info.device)
            except (OSError, ValueError) as exc:
                self._report(f"cannot open {info.device}: {exc}")
                if info.device in preferred:
                    return None, None  # the robot's port is busy: wait for it instead of taking another board
                continue
            self._problem = ""
            return info, port
        return None, None

    def _serve(self, info: PortInfo, port: SerialPort) -> None:
        name = info.device
        with self._cond:
            self._port, self._port_name, self._port_info = port, name, info
            self._closing = self._port_error = self._linked = self._abandon = False
            self._next_hello_at = 0.0
            self._confirmed_count = None
        self._hello_warned = False
        self._hello_failures = 0
        log.info("opened %s%s", name, f" (serial {info.serial_number})" if info.serial_number else "")
        self._serial_log.marker(f"port {name} opened")
        reader = threading.Thread(target=self._read_loop, args=(port,), name="usb-reader", daemon=True)
        reader.start()
        try:
            self._protocol_loop(port)
        finally:
            with self._cond:
                say_goodbye = self._stopping and self._linked and not self._port_error
                self._closing = True
                self._linked = False
            if say_goodbye:
                self._send_clear(port)
            reader.join(2.0)
            try:
                port.close()
            except Exception:  # closing a vanished port may fail; it is gone either way
                pass
            with self._cond:
                abandoned = self._abandon and not (self._released or self._stopping)
                if self._released:
                    reason = "released"
                elif self._stopping:
                    reason = "stopping"
                else:
                    reason = "silent" if abandoned else "lost"
                self._port = None
                self._events.clear()
                self._cond.notify_all()
            log.info("closed %s (%s)", name, reason)
            self._serial_log.marker(f"port {name} closed ({reason})")
            if abandoned:
                self._cool_down(info)

    def _cool_down(self, info: PortInfo) -> None:
        seconds = self._timing.port_cooldown
        self._cooldown[info.device] = time.monotonic() + seconds
        if self._candidates <= 1:
            log.warning("%s, the only matching port, does not answer hello (no StackChan with the USB link?); "
                        "leaving it alone for %.0f s", info.device, seconds)
        else:
            log.info("%s does not answer hello; trying the other ports, back to it in %.0f s", info.device, seconds)

    def _protocol_loop(self, port: SerialPort) -> None:
        while True:
            msg = self._next_message()
            if msg is None:
                return
            try:
                ack = self._request(port, msg)
            except ValueError as exc:  # frame too long; cannot happen with the size-limited model
                log.error("cannot send %s: %s", msg["t"], exc)
                continue
            with self._cond:
                if self._stopping or self._released or self._port_error:
                    return
                self._after_request(msg, ack)

    def _next_message(self) -> dict[str, Any] | None:
        """Block until something is due: hello, state (debounced), event, or ping. None = leave the port."""
        timing = self._timing
        with self._cond:
            while True:
                if self._stopping or self._released or self._port_error or self._abandon:
                    return None
                now = time.monotonic()
                if not self._linked:
                    if now >= self._next_hello_at:
                        return {"t": "hello", "v": 1, "agent": self._agent_name}
                    self._cond.wait(self._next_hello_at - now)
                    continue
                while self._events and now - self._events[0][0] > EVENT_MAX_AGE:
                    log.debug("dropping stale event %s", self._events.popleft()[1].get("ev"))
                while self._commands and now - self._commands[0][0] > COMMAND_MAX_AGE:
                    log.warning("dropping command %s: robot not linked in time", self._commands.popleft()[1]["t"])
                state_wanted = self._force_state or self._state_dirty
                # A change waits state_interval before the snapshot is taken (an act that flips back meanwhile sends
                # nothing) and states keep state_interval apart (at most 5 per second). Events go out right away,
                # after the pending state, so the state is not debounced then.
                state_due = max(self._dirty_since, self._last_state_at) + timing.state_interval
                state_allowed = self._force_state or bool(self._events) or now >= state_due
                if state_wanted and state_allowed:
                    msg = self._take_state(now)
                    if msg is not None:
                        return msg
                    continue
                if self._events:
                    return {"t": "event", **self._events.popleft()[1]}
                if self._commands:
                    return self._commands.popleft()[1]
                if now - self._last_tx_at >= timing.ping_interval:
                    return {"t": "ping"}
                deadline = self._last_tx_at + timing.ping_interval
                if state_wanted:
                    deadline = min(deadline, state_due)
                self._cond.wait(max(0.01, deadline - now))

    def _take_state(self, now: float) -> dict[str, Any] | None:  # self._cond held
        snapshot = self._state_provider()
        force = self._force_state
        self._state_dirty = self._force_state = False
        if not force and snapshot == self._last_state:
            return None
        self._last_state, self._last_state_at = snapshot, now
        return {"t": "state", **snapshot}

    def _request(self, port: SerialPort, msg: dict[str, Any]) -> dict[str, Any] | None:
        """Send one message stop-and-wait: up to 1 + retries attempts with the same id. Returns the ack or None."""
        with self._cond:
            msg_id = self._next_id
            self._next_id += 1
        frame = encode_frame({"t": msg["t"], "id": msg_id, **{k: v for k, v in msg.items() if k != "t"}})
        with self._cond:
            self._pending_id, self._ack = msg_id, None
        try:
            for attempt in range(1 + self._timing.retries):
                if attempt:
                    with self._cond:
                        self._stats["retries"] += 1
                try:
                    with self._write_lock:
                        port.write(frame)
                except self._backend.write_timeout_error:
                    log.debug("write timeout for %s id %d", msg["t"], msg_id)
                    continue
                except Exception as exc:  # serial.SerialException: port gone
                    self._fail_port(f"write failed: {exc}")
                    return None
                with self._cond:
                    self._stats["tx"] += 1
                    self._last_tx_at = time.monotonic()
                    self._cond.wait_for(
                        lambda: self._ack is not None or self._stopping or self._released or self._port_error,
                        self._timing.ack_timeout,
                    )
                    if self._ack is not None:
                        return self._ack
                    if self._stopping or self._released or self._port_error:
                        return None
            with self._cond:
                self._stats["timeouts"] += 1
            return None
        finally:
            with self._cond:
                self._pending_id = None

    def _after_request(self, msg: dict[str, Any], ack: dict[str, Any] | None) -> None:  # self._cond held
        kind = msg["t"]
        now = time.monotonic()
        if ack is None:
            if kind == "hello":
                self._hello_unanswered(now)
            else:
                log.warning("no ack for %s, restarting the link with hello", kind)
                self._linked = False
                self._next_hello_at = now
            return
        self._hello_failures = 0
        self._check_robot(kind, ack)
        if not ack.get("ok", False):
            self._stats["nacks"] += 1
            log.warning("robot rejected %s: %s", kind, ack.get("err", "?"))
            if kind == "hello":
                self._next_hello_at = now + self._timing.hello_interval
            return
        if kind == "agent":
            log.info("robot: AI Agent %s", "started" if ack.get("ai") else "start requested")
        elif kind == "set":
            self._robot.setdefault("settings", {})[msg["k"]] = ack.get("val")
            log.info("robot: setting %s = %s", msg["k"], ack.get("val"))
        elif kind == "state":
            self._confirmed_count = len(msg["sessions"])
            if isinstance(ack.get("n"), int) and ack["n"] != self._confirmed_count:
                log.warning("robot kept %s of %d sessions", ack["n"], self._confirmed_count)
        elif kind == "hello":
            self._robot.update(fw=ack.get("fw"), up=ack.get("up"))
            self._linked = True
            self._force_state = True  # (re)connect: resend the current state
            self._confirmed_count = None
            self._hello_warned = False
            info = self._port_info
            if info is not None:
                self._known.add(self._key(info))
                if info.serial_number and info.serial_number != self._robot_serial:
                    self._robot_serial = info.serial_number
                    log.info("robot serial number %s", info.serial_number)
            log.info("linked: robot fw %s, boot %s, up %s s", ack.get("fw"), ack.get("boot"), ack.get("up"))

    def _hello_unanswered(self, now: float) -> None:  # self._cond held
        self._next_hello_at = now + self._timing.hello_interval
        self._hello_failures += 1
        if not self._hello_warned:
            log.info("robot does not answer hello (firmware without the USB link?), retrying")
            self._hello_warned = True
        info = self._port_info
        if info is not None and self._hello_failures >= self._timing.silent_hellos and not self._trusted(info):
            self._abandon = True

    def _check_robot(self, kind: str, ack: dict[str, Any]) -> None:  # self._cond held
        """Every ack may carry the robot's boot id and its number of used slots ("n"); older firmware sends
        neither. A new boot id or a slot count that does not match the last acknowledged state means the robot
        lost the sessions (restart without USB re-enumeration, staleness timeout): resend the state."""
        boot = ack.get("boot")
        if boot is not None:
            if self._robot.get("boot") not in (None, boot):
                log.info("robot restarted (boot id changed), resending the state")
                self._force_state = True
            self._robot["boot"] = boot
        count = ack.get("n")
        if kind in ("state", "hello", "clear") or not isinstance(count, int) or self._confirmed_count is None:
            return
        if count == self._confirmed_count:
            self._count_warned = False
            return
        if not self._count_warned:
            log.info("robot shows %d sessions, agent sent %d: resending the state", count, self._confirmed_count)
            self._count_warned = True
        self._force_state = True

    def _send_clear(self, port: SerialPort) -> None:
        """Best effort on shutdown: tell the robot to drop the sessions now instead of after 30 s."""
        with self._cond:
            msg_id = self._next_id
            self._next_id += 1
        try:
            with self._write_lock:
                port.write(encode_frame({"t": "clear", "id": msg_id}))
        except Exception:
            pass

    def _fail_port(self, reason: str) -> None:
        with self._cond:
            if not self._port_error and not self._closing:
                log.info("port error: %s", reason)
            self._port_error = True
            self._cond.notify_all()

    # --- reader thread ---

    def _read_loop(self, port: SerialPort) -> None:
        buffer = b""
        while True:
            with self._cond:
                if self._closing or self._port_error:
                    break
            try:
                chunk = port.read(4096)
            except Exception as exc:  # serial.SerialException when the device disappears
                self._fail_port(f"read failed: {exc}")
                break
            if not chunk:
                continue
            lines = (buffer + chunk).split(b"\n")
            buffer = lines.pop()
            if len(buffer) > MAX_LINE:
                lines.append(buffer)
                buffer = b""
            for line in lines:
                self._handle_line(line)
        if buffer:
            self._handle_line(buffer)

    def _handle_line(self, line: bytes) -> None:
        text, frame = split_line(line)
        if frame is None:
            if "@@sc" in text:
                with self._cond:
                    self._stats["bad_frames"] += 1
            self._serial_log.write(text)
            return
        if text.strip():  # log output that preceded the frame on the same line
            self._serial_log.write(text)
        if frame.get("t") != "ack":
            log.debug("ignoring frame type %r from the robot", frame.get("t"))
            return
        with self._cond:
            self._stats["acks"] += 1
            if isinstance(frame.get("st"), str):
                self._robot["st"] = frame["st"]
            if isinstance(frame.get("ai"), bool):  # firmware with the "agent" command
                self._robot["ai"] = frame["ai"]
            if self._pending_id is not None and frame.get("id") == self._pending_id:
                self._ack = frame
                self._cond.notify_all()


# --- HTTP endpoint -------------------------------------------------------------------------------------------


class AgentHTTPServer(ThreadingHTTPServer):
    daemon_threads = True
    # On Windows SO_REUSEADDR lets a second process bind the same port. Bind exclusively instead, so a second
    # agent fails to start and no other program can take over the port.
    allow_reuse_address = os.name != "nt"

    def __init__(self, address: tuple[str, int], agent: Agent) -> None:
        self.agent = agent
        super().__init__(address, HookRequestHandler)

    def server_bind(self) -> None:
        if os.name == "nt" and hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        socketserver.TCPServer.server_bind(self)  # skips HTTPServer's reverse DNS lookup
        host, port = self.server_address[:2]
        self.server_name, self.server_port = str(host), int(port)

    def handle_error(self, request: Any, client_address: Any) -> None:
        log.exception("HTTP handler error")


SETTING_KEY = re.compile(r"[a-z0-9_]{1,15}")


def setting_command(body: bytes) -> dict[str, Any] | None:
    """The robot command for POST /setting {"key": "sleep", "value": 0}: a settings panel key and an int (or bool);
    without "value" the robot only reports the current value. None if the body is not like that."""
    try:
        payload = json.loads(body.decode("utf-8", "replace"))
    except ValueError:
        return None
    if not isinstance(payload, dict):
        return None
    key = payload.get("key")
    if not isinstance(key, str) or not SETTING_KEY.fullmatch(key):
        return None
    command: dict[str, Any] = {"t": "set", "k": key}
    if "value" in payload:
        value = payload["value"]
        if isinstance(value, bool):
            value = int(value)
        if not isinstance(value, int):
            return None
        command["v"] = value
    return command


def head_command(body: bytes) -> dict[str, Any] | None:
    """The robot command for POST /head {"yaw": 20, "pitch": 30, "speed": 200}: degrees (yaw -128..128 like the voice
    tool, pitch 5..85; the robot clamps), at least one of them; speed 50..1000, optional. None if not like that."""
    try:
        payload = json.loads(body.decode("utf-8", "replace"))
    except ValueError:
        return None
    if not isinstance(payload, dict):
        return None
    command: dict[str, Any] = {"t": "head"}
    for field, key in (("yaw", "yaw"), ("pitch", "pitch"), ("speed", "spd")):
        if field in payload:
            value = payload[field]
            if isinstance(value, bool) or not isinstance(value, int):
                return None
            command[key] = value
    return command if "yaw" in command or "pitch" in command else None


class HookRequestHandler(BaseHTTPRequestHandler):
    server: AgentHTTPServer
    server_version = f"sc-agent/{AGENT_VERSION}"
    sys_version = ""
    timeout = 5  # seconds per socket operation, so a stalled client cannot hold a handler thread

    def do_POST(self) -> None:
        path = urlsplit(self.path).path
        if path not in ("/hook", "/release", "/acquire", "/agent", "/setting", "/head", "/shutdown"):
            self._send(404, {"error": "not found"})
            return
        if not self._authorized(self.headers.get("X-SC-Token")):
            return
        body = self._read_body(required=(path in ("/hook", "/setting", "/head")))
        if body is None:
            return
        agent = self.server.agent
        if path == "/hook":
            try:
                payload = json.loads(body.decode("utf-8", "replace"))
            except ValueError:
                payload = None
            if not isinstance(payload, dict):
                self._send(400, {"error": "body must be a JSON object"})
                return
            agent.on_hook(payload)
            self._send(200, {"ok": True})
        elif path == "/release":
            ok, port = agent.link.release()
            self._send(200 if ok else 503, {"ok": ok, "port": port})
        elif path == "/acquire":
            agent.link.acquire()
            self._send(200, {"ok": True})
        elif path == "/agent":
            log.info("AI Agent start requested over HTTP")
            agent.link.queue_command({"t": "agent"})
            self._send(200, {"ok": True})
        elif path == "/setting":
            command = setting_command(body)
            if command is None:
                self._send(400, {"error": 'body must be {"key": "<setting key>", "value": <int or bool, optional>}'})
                return
            log.info("setting %s requested over HTTP", command["k"])
            agent.link.queue_command(command)  # the robot's answer shows up in /status as robot_settings
            self._send(200, {"ok": True})
        elif path == "/head":
            command = head_command(body)
            if command is None:
                self._send(400, {"error": 'body must be {"yaw": <deg>, "pitch": <deg>, "speed": <50-1000>}, one angle at least'})
                return
            log.info("head move requested over HTTP: %s", {k: v for k, v in command.items() if k != "t"})
            agent.link.queue_command(command)
            self._send(200, {"ok": True})
        else:
            log.info("shutdown requested over HTTP")
            self._send(200, {"ok": True})
            agent.request_stop()  # the main thread stops the agent; this handler thread only signals

    def do_GET(self) -> None:
        parts = urlsplit(self.path)
        if parts.path != "/status":
            self._send(404, {"error": "not found"})
            return
        token = self.headers.get("X-SC-Token") or parse_qs(parts.query).get("token", [None])[0]
        if self._authorized(token):
            self._send(200, self.server.agent.status())

    def _authorized(self, token: str | None) -> bool:
        if self.server.agent.check_token(token):
            return True
        log.warning("rejected %s %s: missing or wrong token", self.command, urlsplit(self.path).path)
        self._send(403, {"error": "forbidden"})
        return False

    def _read_body(self, required: bool) -> bytes | None:
        length_text = self.headers.get("Content-Length")
        if length_text is None:
            if required:
                self._send(411, {"error": "Content-Length required"})
                return None
            return b""
        try:
            length = int(length_text)
        except ValueError:
            length = -1
        if length < 0 or length > MAX_HOOK_BODY:
            self.close_connection = True
            self._send(413 if length > MAX_HOOK_BODY else 400, {"error": "bad Content-Length"})
            return None
        return self.rfile.read(length)

    def _send(self, code: int, body: dict[str, Any]) -> None:
        data = json.dumps(body).encode("utf-8")
        try:
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass  # the hook's curl gave up (-m 2); nothing to do

    def log_request(self, code: Any = "-", size: Any = "-") -> None:
        log.debug("%s %s -> %s", self.command, urlsplit(self.path).path, code)

    def log_message(self, format: str, *args: Any) -> None:
        # The default writes the raw request line, which may carry ?token=, to stderr. Keep it out of all logs.
        pass


# --- the daemon ----------------------------------------------------------------------------------------------


class Agent:
    """The daemon object: HTTP endpoint, session table, USB link and housekeeping."""

    def __init__(
        self, config: Config, token: str, data_dir: Path, backend: SerialBackend, timing: LinkTiming = LinkTiming()
    ) -> None:
        self.config = config
        self.table = SessionTable(config.quiet, detector=CommandDetector(config.test_patterns))
        self.serial_log = SerialLog(data_dir / "logs", config.serial_log_days)
        self.link = UsbLink(
            backend,
            self.table.snapshot,
            self.serial_log,
            timing=timing,
            release_timeout_s=config.release_timeout_s,
            robot_serial=config.serial_number,
        )
        self._token = token.encode("utf-8")
        self.stop_requested = threading.Event()  # set by POST /shutdown or a signal; main() then calls stop()
        self._stop = threading.Event()
        self._threads: list[threading.Thread] = []
        self.http: AgentHTTPServer | None = None

    @property
    def http_port(self) -> int:
        assert self.http is not None
        return int(self.http.server_address[1])

    def start(self) -> None:
        # Bind first: when another agent already runs this raises OSError before the COM port is touched.
        self.http = AgentHTTPServer(("127.0.0.1", self.config.http_port), self)
        self._spawn(self.http.serve_forever, "http")
        self._spawn(self._housekeeping, "housekeeping")
        self.link.start()
        log.info("sc-agent %s listening on 127.0.0.1:%d", AGENT_VERSION, self.http_port)

    def stop(self) -> None:
        self._stop.set()
        self.link.stop()
        if self.http is not None:
            self.http.shutdown()
            self.http.server_close()
        for thread in self._threads:
            thread.join(2.0)
        self.serial_log.close()
        log.info("sc-agent stopped")

    def request_stop(self) -> None:
        self.stop_requested.set()

    def check_token(self, candidate: str | None) -> bool:
        return candidate is not None and hmac.compare_digest(candidate.encode("utf-8"), self._token)

    def on_hook(self, payload: dict[str, Any]) -> None:
        log.debug("hook %s %s", payload.get("hook_event_name"), str(payload.get("session_id", ""))[:8])
        result = self.table.handle(payload)
        if result.state_changed:
            self.link.notify_state()
        for event in result.events:
            self.link.queue_event(event)

    def status(self) -> dict[str, Any]:
        link = self.link.status()
        robot = link["robot"]
        return {
            "agent": AGENT_VERSION,
            "link": link["link"],
            "port": link["port"],
            "serial": link["serial"],
            "robot_st": robot.get("st"),
            "robot_ai": robot.get("ai"),
            "robot_settings": dict(robot.get("settings", {})),
            "boot": robot.get("boot"),
            "fw": robot.get("fw"),
            "sessions": self.table.snapshot()["sessions"],
            "stats": link["stats"],
        }

    def _housekeeping(self) -> None:
        while not self._stop.wait(10.0):
            if self.table.expire():
                self.link.notify_state()

    def _spawn(self, target: Callable[[], None], name: str) -> None:
        thread = threading.Thread(target=target, name=name, daemon=True)
        thread.start()
        self._threads.append(thread)


# --- entry point ---------------------------------------------------------------------------------------------


def setup_logging(data_dir: Path, console: bool, verbose: bool) -> None:
    root = logging.getLogger()
    root.setLevel(logging.DEBUG if verbose else logging.INFO)
    formatter = logging.Formatter("%(asctime)s %(levelname)-7s %(name)s: %(message)s")
    handlers: list[logging.Handler] = [
        RotatingFileHandler(data_dir / "agent.log", maxBytes=1_000_000, backupCount=3, encoding="utf-8")
    ]
    if console:
        handlers.append(logging.StreamHandler(sys.stderr))
    for handler in handlers:
        handler.setFormatter(formatter)
        root.addHandler(handler)


def _log_uncaught(exc_type: type[BaseException], exc: BaseException, tb: Any) -> None:
    log.critical("uncaught exception", exc_info=(exc_type, exc, tb))


def _log_thread_exception(args: threading.ExceptHookArgs) -> None:
    log.error("uncaught exception in thread %s", args.thread.name if args.thread else "?",
              exc_info=(args.exc_type, args.exc_value, args.exc_traceback))  # type: ignore[arg-type]


def _ensure_std_streams() -> None:
    # Under pythonw sys.stdout / sys.stderr are None and any stray write would raise.
    for name in ("stdout", "stderr"):
        if getattr(sys, name) is None:
            setattr(sys, name, open(os.devnull, "w", encoding="utf-8"))


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="StackChan Claude Code status agent")
    parser.add_argument("--console", action="store_true", help="also log to the console (foreground debugging)")
    parser.add_argument("--verbose", action="store_true", help="debug logging")
    parser.add_argument("--data-dir", help="data directory (default: %%USERPROFILE%%\\.sc-agent)")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    _ensure_std_streams()
    data_dir = Path(args.data_dir).expanduser() if args.data_dir else default_data_dir()
    data_dir.mkdir(parents=True, exist_ok=True)
    setup_logging(data_dir, console=args.console, verbose=args.verbose)
    sys.excepthook = _log_uncaught
    threading.excepthook = _log_thread_exception
    try:
        return _run(data_dir)
    except Exception:
        log.exception("sc-agent failed")
        return 1


def _run(data_dir: Path) -> int:
    try:
        config = load_config(data_dir / "config.json")
    except ConfigError as exc:
        log.error("invalid config: %s", exc)
        return 2
    token = load_or_create_token(data_dir / "token")
    try:
        backend = PySerialBackend(config.usb_vid, config.usb_pid, config.serial_port)
    except ImportError:
        log.error("pyserial is not installed; run setup.bat")
        return 2
    agent = Agent(config, token, data_dir, backend)
    try:
        agent.start()
    except OSError as exc:
        log.error("cannot listen on 127.0.0.1:%d (%s); is another sc-agent running?", config.http_port, exc)
        return 1

    for name in ("SIGTERM", "SIGBREAK"):
        if hasattr(signal, name):
            signal.signal(getattr(signal, name), lambda signum, frame: agent.request_stop())
    try:
        while not agent.stop_requested.is_set():
            time.sleep(0.5)
    except KeyboardInterrupt:
        log.info("Ctrl+C, stopping")
    finally:
        agent.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
