"""StackChan BLE config channel: fragment framing (pure functions, no BLE I/O).

Mirrors the firmware side in firmware/main/hal/hal_ble.cpp. A message that does not fit into one ATT value is
split into packets that each start with a 10-byte header:

    AA 55 C3 01 | packet index (u16 BE) | packet count (u16 BE) | message length (u16 BE) | payload

Any value that does not start with that header (or is at most 10 bytes long) is a complete message on its own.
"""

from __future__ import annotations

MAGIC = b"\xaa\x55\xc3"
VERSION = 1
HEADER_LEN = 10
MAX_MESSAGE_LEN = 2048  # STACKCHAN_MAX_JSON_LEN in firmware/main/hal/utils/bleprph/bleprph.h


def is_fragment(packet: bytes) -> bool:
    return len(packet) > HEADER_LEN and packet[:3] == MAGIC and packet[3] == VERSION


def encode_message(message: bytes, max_value_len: int) -> list[bytes]:
    """Split one message into ATT values of at most max_value_len bytes (MTU - 3)."""
    if not message:
        raise ValueError("empty message")
    if len(message) >= MAX_MESSAGE_LEN:
        raise ValueError(f"message too long: {len(message)} bytes (limit {MAX_MESSAGE_LEN - 1})")
    if len(message) <= max_value_len:
        return [message]
    chunk = max_value_len - HEADER_LEN
    if chunk <= 0:
        raise ValueError(f"ATT value size {max_value_len} is too small for fragmentation")
    count = (len(message) + chunk - 1) // chunk
    packets = []
    for index in range(count):
        header = MAGIC + bytes([VERSION]) + index.to_bytes(2, "big") + count.to_bytes(2, "big")
        header += len(message).to_bytes(2, "big")
        packets.append(header + message[index * chunk : (index + 1) * chunk])
    return packets


class Reassembler:
    """Collects fragments into messages, with the same rules as BleFragmentAssembler in the firmware."""

    def __init__(self) -> None:
        self._reset()

    def _reset(self) -> None:
        self._buffer = bytearray()
        self._expected_index = 0
        self._count = 0
        self._length = 0
        self._assembling = False

    def feed(self, packet: bytes) -> bytes | None:
        """Return a complete message, or None while fragments are missing or after a broken sequence."""
        if not is_fragment(packet):
            self._reset()
            return packet
        index = int.from_bytes(packet[4:6], "big")
        count = int.from_bytes(packet[6:8], "big")
        length = int.from_bytes(packet[8:10], "big")
        payload = packet[HEADER_LEN:]
        if count == 0 or length == 0 or length > MAX_MESSAGE_LEN:
            self._reset()
            return None
        if index == 0:
            # A first packet always (re)starts a message.
            self._reset()
            self._assembling = True
            self._count = count
            self._length = length
        elif not self._assembling or index != self._expected_index or count != self._count or length != self._length:
            self._reset()
            return None
        if len(self._buffer) + len(payload) > self._length:
            self._reset()
            return None
        self._buffer += payload
        self._expected_index += 1
        if self._expected_index < self._count:
            return None
        message = bytes(self._buffer) if len(self._buffer) == self._length else None
        self._reset()
        return message
