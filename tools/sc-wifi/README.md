# sc-wifi: add a Wi-Fi network to StackChan over Bluetooth

**English** | [中文](README.zh-CN.md)

`sc_wifi.py` sends a Wi-Fi name and password from the PC to the robot over BLE. It uses the same BLE channel as the
StackChan World App setup, so it works with the official 1.6 firmware and with this fork. No App and no pairing are
needed.

## What happens to the saved networks

The robot keeps a list of up to **10** networks (xiaozhi `SsidManager`, NVS namespace `wifi`):

- The new network is verified first: the robot connects to it and saves it only after it gets an IP address. If that
  fails, nothing changes.
- A new network goes to the front of the list. All other networks stay. The same SSID again only replaces its
  password. With 10 networks saved, the oldest one is dropped.
- At every boot the robot scans and joins the **strongest saved network in range**. So home, office and phone
  hotspot can all be saved, and the robot uses whichever one is there.

Limits: the robot is ESP32-S3, so **2.4 GHz only**. The new network must be in range while you add it (it is tested
before it is saved).

## Files

| File | Purpose |
|---|---|
| `sc_wifi.py` | Command line tool: scan, connect, send `setWifi`, wait for the result |
| `ble_frames.py` | Pure functions: fragment framing of the BLE config channel (same as `firmware/main/hal/hal_ble.cpp`) |
| `test_ble_frames.py` | Unit tests (stdlib only, no Bluetooth needed) |
| `requirements.txt` | `bleak` (BLE library, uses the Windows Bluetooth stack) |

## Install (once)

Windows 10/11 with Bluetooth, Python 3.10 or newer:

```bat
cd <repo>\tools\sc-wifi
python -m venv .venv
.venv\Scripts\python -m pip install -r requirements.txt
```

## Use

1. On the robot: go back to the launcher, open **SETUP > Wi-Fi > Change Wi-Fi**, tap **Next**. The screen shows
   "Look for me in the app". The robot now advertises as `StackChan`.
2. Close the StackChan World App on your phone (only one BLE client can connect).
3. On the PC (PowerShell or cmd, so the password prompt works):
   ```bat
   .venv\Scripts\python sc_wifi.py --ssid "Office-2.4G"
   ```
   The password is asked for and not echoed. An empty password means an open network. `--password` also works,
   but then it stays in the shell history.
4. The robot says "Verifying...". On success the script prints `OK`, and the robot reboots after about 4 s and joins
   the strongest saved network in range.

Options: `--address AA:BB:...` picks one robot when several are in setup mode; `--scan-timeout` (default 6 s);
`--timeout` waits for the result (default 60 s, the robot gives up after 30 s).

Exit codes: `0` saved, `1` the robot could not join the network, `2` bad input, robot not found or `bleak` missing.

## Troubleshooting

| Message | Meaning |
|---|---|
| `Found StackChan, but it is not in Wi-Fi setup mode` | Do step 1; the BLE server for Wi-Fi only runs on that screen |
| `No StackChan found` | Robot too far away, Bluetooth off on the PC, or the phone App is still connected |
| `FAILED (authentication_failed)` / `(handshake_failed)` | Wrong password (or weak signal); nothing was saved |
| `FAILED (ap_not_found)` / `(not_2g4)` | Network not seen: 5 GHz only, out of range, or wrong SSID |
| `FAILED (connection_timeout)` / `(dhcp_timeout)` | Router did not finish the connection or gave no IP address |
| `FAILED (busy)` | A previous attempt is still running; wait 30 s |

## Protocol (for reference)

- Setup service `e2e5e5ff-1234-5678-1234-56789abcdef0`, config characteristic
  `e2e5e5e3-1234-5678-1234-56789abcdef0` (write with response + notify).
- Request: `{"cmd":"setWifi","data":{"ssid":"...","password":"..."}}`
- Replies (notify): `{"cmd":"notifyState","data":{"type":T,"state":"..."}}` with `T` = 0 connecting, 1 connected,
  2 failed (plus `errorCode`, `errorMessage`, `reason`, `ssid`).
- A value longer than one ATT payload (MTU - 3) is split into packets with the 10-byte header
  `AA 55 C3 01 | index u16 | count u16 | total length u16` (big endian). See `ble_frames.py`.

Run the tests with `python test_ble_frames.py`.
