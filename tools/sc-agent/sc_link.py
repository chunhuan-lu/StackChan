"""StackChan USB link protocol v1: framing helpers (pure functions, no serial I/O).

One frame per line, in both directions:

    @@sc<compact JSON>*<CRC-32 of the JSON bytes, 8 uppercase hex digits>\\n

Every other line is log output. A receiver takes the LAST "@@sc" in a line and the last "*" after it, so log
text, ANSI colour codes or a stale fragment may precede a frame. Contract: docs/requirements/
REQ-003-claude-code-status.md, section "USB link protocol v1".
"""

from __future__ import annotations

import json
import re
import zlib
from typing import Any

MAGIC = b"@@sc"
MAX_HOST_JSON = 768  # host -> device, bytes of JSON
MAX_DEVICE_JSON = 256  # device -> host, bytes of JSON

# CSI escape sequences: colour codes such as "\x1b[0;32m" and cursor control.
_ANSI = re.compile(rb"\x1b\[[0-?]*[ -/]*[@-~]")
_CRC_HEX = re.compile(rb"[0-9A-Fa-f]{8}")


def crc32_hex(data: bytes) -> str:
    """Standard zlib CRC-32 as 8 uppercase hex digits (esp_rom_crc32_le(0, ...) on the device)."""
    return "%08X" % (zlib.crc32(data) & 0xFFFFFFFF)


def strip_ansi(data: bytes) -> bytes:
    return _ANSI.sub(b"", data)


def encode_frame(obj: dict[str, Any], max_json: int = MAX_HOST_JSON) -> bytes:
    """Encode one message as a complete frame line; raises ValueError when the JSON exceeds max_json bytes."""
    text = json.dumps(obj, separators=(",", ":"), ensure_ascii=False)
    # "@" can only occur inside JSON strings, where "@" is an equivalent escape. Breaking up a literal
    # "@@sc" (e.g. in a project name) keeps the receiver's "last @@sc" rule from cutting the frame in two.
    text = text.replace("@@sc", "\\u0040@sc")
    data = text.encode("utf-8")
    if len(data) > max_json:
        raise ValueError(f"frame JSON is {len(data)} bytes, limit is {max_json}")
    return MAGIC + data + b"*" + crc32_hex(data).encode("ascii") + b"\n"


def split_line(line: bytes | str, max_json: int = MAX_HOST_JSON) -> tuple[str, dict[str, Any] | None]:
    """Split one received line into (log text, frame).

    ANSI codes and the line ending are removed. With a valid frame the log text is whatever preceded the frame;
    without one the frame is None and the log text is the whole line.
    """
    raw = line.encode("utf-8") if isinstance(line, str) else bytes(line)
    raw = strip_ansi(raw).rstrip(b"\r\n")
    found = _find_frame(raw, max_json)
    if found is None:
        return raw.decode("utf-8", "replace"), None
    start, frame = found
    return raw[:start].decode("utf-8", "replace"), frame


def parse_line(line: bytes | str, max_json: int = MAX_HOST_JSON) -> dict[str, Any] | None:
    """Return the frame carried by a received line, or None (no frame, bad CRC, bad JSON, too long)."""
    return split_line(line, max_json)[1]


def _find_frame(raw: bytes, max_json: int) -> tuple[int, dict[str, Any]] | None:
    start = raw.rfind(MAGIC)
    if start < 0:
        return None
    body_start = start + len(MAGIC)
    star = raw.rfind(b"*", body_start)
    if star < 0:
        return None
    body = raw[body_start:star]
    crc = raw[star + 1 :].rstrip()
    if not body or len(body) > max_json or not _CRC_HEX.fullmatch(crc):
        return None
    if int(crc, 16) != zlib.crc32(body) & 0xFFFFFFFF:
        return None
    try:
        obj = json.loads(body.decode("utf-8"))
    except ValueError:  # includes UnicodeDecodeError and JSONDecodeError
        return None
    if not isinstance(obj, dict):
        return None
    return start, obj
