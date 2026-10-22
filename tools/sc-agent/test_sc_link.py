"""Tests for the USB link framing (sc_link.py). Stdlib only."""

import sys

sys.dont_write_bytecode = True

import json  # noqa: E402
import unittest  # noqa: E402
import zlib  # noqa: E402

from sc_link import MAX_HOST_JSON, crc32_hex, encode_frame, parse_line, split_line  # noqa: E402


def make_line(json_text: str, crc: str | None = None) -> bytes:
    data = json_text.encode("utf-8")
    return b"@@sc" + data + b"*" + (crc or crc32_hex(data)).encode("ascii") + b"\r\n"


class CrcTest(unittest.TestCase):
    def test_known_vector(self):
        self.assertEqual(crc32_hex(b"123456789"), "CBF43926")

    def test_matches_zlib(self):
        data = '{"t":"ack","id":7,"ok":true}'.encode()
        self.assertEqual(int(crc32_hex(data), 16), zlib.crc32(data))


class EncodeTest(unittest.TestCase):
    def test_exact_format(self):
        frame = encode_frame({"t": "ping", "id": 1})
        body = b'{"t":"ping","id":1}'
        self.assertEqual(frame, b"@@sc" + body + b"*" + crc32_hex(body).encode() + b"\n")

    def test_utf8_not_escaped(self):
        frame = encode_frame({"t": "event", "id": 2, "text": "项目: done"})
        self.assertIn("项目".encode("utf-8"), frame)
        self.assertEqual(frame.count(b"\n"), 1)

    def test_newline_in_text_stays_on_one_line(self):
        frame = encode_frame({"t": "event", "id": 3, "text": "a\nb"})
        self.assertEqual(frame.count(b"\n"), 1)
        self.assertEqual(parse_line(frame)["text"], "a\nb")

    def test_size_limit(self):
        base = len(json.dumps({"t": "x", "pad": ""}, separators=(",", ":")))
        exact = {"t": "x", "pad": "a" * (MAX_HOST_JSON - base)}
        self.assertEqual(parse_line(encode_frame(exact)), exact)
        with self.assertRaises(ValueError):
            encode_frame({"t": "x", "pad": "a" * (MAX_HOST_JSON - base + 1)})

    def test_magic_inside_string_is_escaped(self):
        obj = {"t": "state", "id": 4, "sessions": [{"slot": 0, "name": "x@@scy", "st": "idle"}]}
        frame = encode_frame(obj)
        self.assertEqual(frame.count(b"@@sc"), 1)
        self.assertEqual(parse_line(frame), obj)


class ParseTest(unittest.TestCase):
    def test_round_trip(self):
        obj = {"t": "state", "id": 9, "sessions": [{"slot": 0, "name": "StackChan", "st": "work"}]}
        self.assertEqual(parse_line(encode_frame(obj)), obj)
        self.assertEqual(parse_line(encode_frame(obj).decode("utf-8")), obj)

    def test_crlf_line_ending(self):
        self.assertEqual(parse_line(make_line('{"t":"ack","id":1,"ok":true}')), {"t": "ack", "id": 1, "ok": True})

    def test_log_and_ansi_prefix(self):
        line = b"\x1b[0;32mI (1234) app: hello\x1b[0m " + make_line('{"t":"ack","id":5,"ok":true}')
        text, frame = split_line(line)
        self.assertEqual(frame, {"t": "ack", "id": 5, "ok": True})
        self.assertEqual(text, "I (1234) app: hello ")

    def test_last_magic_wins_over_stale_fragment(self):
        stale = b'@@sc{"t":"ack","id":3,"o'  # frame cut off by a reset, glued to the next one
        line = b"\x1b[0;33mW (99) x\x1b[0m" + stale + make_line('{"t":"ack","id":4,"ok":true}')
        self.assertEqual(parse_line(line), {"t": "ack", "id": 4, "ok": True})

    def test_star_inside_json(self):
        obj = {"t": "event", "id": 6, "text": "a*b*c"}
        self.assertEqual(parse_line(encode_frame(obj)), obj)

    def test_bad_crc_rejected(self):
        self.assertIsNone(parse_line(make_line('{"t":"ack","id":1,"ok":true}', crc="00000000")))

    def test_flipped_byte_rejected(self):
        frame = bytearray(encode_frame({"t": "ack", "id": 1, "ok": True}))
        frame[10] ^= 0x01
        self.assertIsNone(parse_line(bytes(frame)))

    def test_malformed(self):
        good = make_line('{"t":"ack"}').rstrip()
        for line in (
            b"plain log line",
            b"@@sc",
            b'@@sc{"t":"ack"}',  # no CRC
            good[:-1],  # CRC too short
            good + b"x",  # garbage after the CRC
            make_line("[1,2]"),  # not an object
            make_line("{bad json}"),
            b"@@sc\xff\xfe*" + crc32_hex(b"\xff\xfe").encode(),  # not UTF-8
        ):
            with self.subTest(line=line):
                self.assertIsNone(parse_line(line))

    def test_plain_line_is_log_text(self):
        self.assertEqual(split_line(b"\x1b[0;31mE (1) boom\x1b[0m\r\n"), ("E (1) boom", None))

    def test_too_long_rejected(self):
        text = json.dumps({"t": "x", "pad": "a" * MAX_HOST_JSON}, separators=(",", ":"))
        self.assertIsNone(parse_line(make_line(text)))
        self.assertIsNone(parse_line(make_line('{"t":"ack","pad":"aaaa"}'), max_json=10))


if __name__ == "__main__":
    unittest.main()
