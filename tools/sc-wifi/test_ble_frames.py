"""Tests for the BLE config framing (ble_frames.py) and message building (sc_wifi.py). Stdlib only."""

import sys

sys.dont_write_bytecode = True

import json  # noqa: E402
import unittest  # noqa: E402

from ble_frames import HEADER_LEN, MAX_MESSAGE_LEN, Reassembler, encode_message, is_fragment  # noqa: E402
from sc_wifi import build_set_wifi, check_credentials  # noqa: E402


class EncodeTest(unittest.TestCase):
    def test_fits_in_one_value(self):
        self.assertEqual(encode_message(b'{"a":1}', 20), [b'{"a":1}'])

    def test_fragment_header(self):
        message = bytes(range(25))
        packets = encode_message(message, 20)
        self.assertEqual(len(packets), 3)  # 10 payload bytes per packet
        self.assertEqual(packets[0][:HEADER_LEN], bytes.fromhex("AA55C301" "0000" "0003" "0019"))
        self.assertEqual(packets[2][:HEADER_LEN], bytes.fromhex("AA55C301" "0002" "0003" "0019"))
        self.assertTrue(all(len(packet) <= 20 for packet in packets))
        self.assertEqual(b"".join(packet[HEADER_LEN:] for packet in packets), message)

    def test_limits(self):
        with self.assertRaises(ValueError):
            encode_message(b"", 20)
        with self.assertRaises(ValueError):
            encode_message(b"x" * MAX_MESSAGE_LEN, 512)
        with self.assertRaises(ValueError):
            encode_message(b"x" * 20, 10)


class ReassembleTest(unittest.TestCase):
    def test_plain_message_passes_through(self):
        self.assertEqual(Reassembler().feed(b'{"cmd":"x"}'), b'{"cmd":"x"}')

    def test_round_trip(self):
        message = json.dumps({"cmd": "notifyState", "data": {"errorMessage": "x" * 300}}).encode()
        reassembler = Reassembler()
        results = [reassembler.feed(packet) for packet in encode_message(message, 50)]
        self.assertTrue(all(result is None for result in results[:-1]))
        self.assertEqual(results[-1], message)

    def test_missing_packet_drops_message(self):
        packets = encode_message(bytes(40), 20)
        reassembler = Reassembler()
        reassembler.feed(packets[0])
        self.assertIsNone(reassembler.feed(packets[2]))
        self.assertIsNone(reassembler.feed(packets[3]))

    def test_first_packet_restarts(self):
        packets = encode_message(bytes(range(30)), 20)
        reassembler = Reassembler()
        reassembler.feed(packets[0])
        reassembler.feed(packets[1])
        results = [reassembler.feed(packet) for packet in packets]
        self.assertEqual(results[-1], bytes(range(30)))

    def test_short_value_is_not_a_fragment(self):
        self.assertFalse(is_fragment(bytes.fromhex("AA55C301000000010001")))


class SetWifiTest(unittest.TestCase):
    def test_command_shape(self):
        command = json.loads(build_set_wifi("家里 WiFi", "p\"w"))
        self.assertEqual(command, {"cmd": "setWifi", "data": {"ssid": "家里 WiFi", "password": 'p"w'}})

    def test_credentials(self):
        self.assertIsNone(check_credentials("home", ""))
        self.assertIsNone(check_credentials("x" * 32, "y" * 64))
        self.assertIsNotNone(check_credentials("", "password"))
        self.assertIsNotNone(check_credentials("家" * 11, "password"))  # 33 bytes in UTF-8
        self.assertIsNotNone(check_credentials("home", "y" * 65))


if __name__ == "__main__":
    unittest.main()
