"""Send Wi-Fi credentials to StackChan over BLE, without forgetting the networks it already knows.

Uses the BLE channel of the StackChan World App setup (no App, no pairing):

  1. On the robot: SETUP > Wi-Fi > Change Wi-Fi > Next. It now advertises "StackChan" with the setup service.
  2. Run:  python sc_wifi.py --ssid "MyWiFi"      (the password is asked for; or pass --password)

The robot connects to the new network first and saves it only after it got an IP address. Saved networks are a
list of up to 10 (xiaozhi SsidManager): the new one goes to the front, the others stay. At boot the robot scans
and joins the strongest saved network in range. The robot reboots about 4 s after a successful setup.
"""

from __future__ import annotations

import argparse
import asyncio
import getpass
import json
import sys

from ble_frames import Reassembler, encode_message

DEVICE_NAME = "StackChan"
SETUP_SERVICE_UUID = "e2e5e5ff-1234-5678-1234-56789abcdef0"  # STACKCHAN_SVC_UUID_BASE_ALT, App setup mode
NORMAL_SERVICE_UUID = "e2e5e5e0-1234-5678-1234-56789abcdef0"  # STACKCHAN_SVC_UUID_BASE, other BLE apps
CONFIG_CHAR_UUID = "e2e5e5e3-1234-5678-1234-56789abcdef0"  # STACKCHAN_CHR_CONFIG_UUID

# notifyState "type" values sent by WifiConfigServer (firmware/main/hal/hal_ble.cpp)
STATE_CONNECTING = 0
STATE_CONNECTED = 1
STATE_FAILED = 2


def build_set_wifi(ssid: str, password: str) -> bytes:
    command = {"cmd": "setWifi", "data": {"ssid": ssid, "password": password}}
    return json.dumps(command, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def check_credentials(ssid: str, password: str) -> str | None:
    """Return an error text for values the firmware would reject (same limits as StackChanWifiStation)."""
    if not 1 <= len(ssid.encode("utf-8")) <= 32:
        return "SSID must be 1-32 bytes (UTF-8)"
    if len(password.encode("utf-8")) > 64:
        return "password must be at most 64 bytes"
    if "\0" in ssid or "\0" in password:
        return "SSID and password must not contain NUL characters"
    return None


async def find_robot(address: str | None, scan_timeout: float):
    from bleak import BleakScanner

    print(f"Scanning for {DEVICE_NAME} ({scan_timeout:.0f} s) ...")
    found = await BleakScanner.discover(timeout=scan_timeout, return_adv=True)
    in_setup, elsewhere = [], []
    for device, adv in found.values():
        if address and device.address.lower() != address.lower():
            continue
        uuids = {uuid.lower() for uuid in adv.service_uuids}
        if SETUP_SERVICE_UUID in uuids:
            in_setup.append(device)
        elif NORMAL_SERVICE_UUID in uuids or (adv.local_name or device.name) == DEVICE_NAME:
            elsewhere.append(device)

    if len(in_setup) == 1:
        return in_setup[0]
    if len(in_setup) > 1:
        print("More than one StackChan is in Wi-Fi setup mode; pick one with --address:")
        for device in in_setup:
            print(f"  {device.address}")
        return None
    if elsewhere:
        print("Found StackChan, but it is not in Wi-Fi setup mode.")
    else:
        print("No StackChan found.")
    print("On the robot open SETUP > Wi-Fi > Change Wi-Fi > Next, then run this script again.")
    return None


async def send_wifi(device, ssid: str, password: str, timeout: float) -> bool:
    from bleak import BleakClient

    loop = asyncio.get_running_loop()
    messages: asyncio.Queue[dict] = asyncio.Queue()
    reassembler = Reassembler()

    def on_notify(_sender, data: bytearray) -> None:
        message = reassembler.feed(bytes(data))
        if message is None:
            return
        try:
            loop.call_soon_threadsafe(messages.put_nowait, json.loads(message))
        except ValueError:
            print(f"Ignoring a notification that is not JSON: {message!r}")

    print(f"Connecting to {device.address} ...")
    async with BleakClient(device) as client:
        await client.start_notify(CONFIG_CHAR_UUID, on_notify)
        for packet in encode_message(build_set_wifi(ssid, password), client.mtu_size - 3):
            await client.write_gatt_char(CONFIG_CHAR_UUID, packet, response=True)
        print(f"Sent Wi-Fi \"{ssid}\"; waiting for the robot to verify it ...")

        deadline = loop.time() + timeout
        while True:
            remaining = deadline - loop.time()
            try:
                message = await asyncio.wait_for(messages.get(), max(remaining, 0))
            except asyncio.TimeoutError:
                print(f"No result within {timeout:.0f} s. Check the robot screen.")
                return False
            if message.get("cmd") != "notifyState":
                continue
            state = message.get("data", {})
            kind = state.get("type")
            if kind == STATE_CONNECTING:
                print("Robot is connecting ...")
            elif kind == STATE_CONNECTED:
                print(f"OK: the robot joined \"{ssid}\" and saved it. Older networks are kept. It reboots in a few s.")
                return True
            elif kind == STATE_FAILED:
                code = state.get("errorCode", "unknown")
                text = state.get("errorMessage", "")
                print(f"FAILED ({code}): {text}")
                print("Nothing was saved; the old networks are unchanged. Fix it and run the script again.")
                return False


async def run(args: argparse.Namespace) -> int:
    password = args.password
    if password is None:
        password = getpass.getpass(f"Password for \"{args.ssid}\" (empty for an open network): ")
    error = check_credentials(args.ssid, password)
    if error:
        print(f"Invalid input: {error}")
        return 2
    device = await find_robot(args.address, args.scan_timeout)
    if device is None:
        return 2
    return 0 if await send_wifi(device, args.ssid, password, args.timeout) else 1


def main() -> int:
    parser = argparse.ArgumentParser(description="Add a Wi-Fi network to StackChan over BLE (keeps saved networks).")
    parser.add_argument("--ssid", required=True, help="network name (2.4 GHz only)")
    parser.add_argument("--password", help="network password; asked for when omitted (keeps it out of the history)")
    parser.add_argument("--address", help="BLE address of the robot, when more than one is in setup mode")
    parser.add_argument("--scan-timeout", type=float, default=6.0, help="BLE scan time in seconds (default 6)")
    parser.add_argument("--timeout", type=float, default=60.0, help="time to wait for the result (default 60)")
    args = parser.parse_args()
    try:
        return asyncio.run(run(args))
    except ImportError:
        print("Missing dependency: run  python -m pip install -r requirements.txt")
        return 2
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
