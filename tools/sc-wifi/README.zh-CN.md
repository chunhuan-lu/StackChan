# sc-wifi：通过蓝牙给 StackChan 添加 Wi-Fi

[English](README.md) | **中文**

`sc_wifi.py` 从电脑通过 BLE 把 Wi-Fi 名称和密码发给机器人。它用的是 StackChan World App 配网时的同一个 BLE 通道，
所以官方 1.6 固件和本 fork 都能用。不需要 App，也不需要配对。

## 已保存的网络会怎样

机器人保存一个最多 **10** 个网络的列表（xiaozhi `SsidManager`，NVS 命名空间 `wifi`）：

- 新网络会先验证：机器人先连上它，拿到 IP 地址后才保存。失败则什么都不改。
- 新网络放在列表最前面，其他网络全部保留。同一个 SSID 再发一次只会更新密码。已存满 10 个时，最旧的那个被挤掉。
- 每次开机，机器人扫描并连接**信号最强的、在范围内的已保存网络**。所以家里、办公室、手机热点都可以存着，
  哪个在就用哪个。

限制：机器人是 ESP32-S3，**只支持 2.4 GHz**。添加时新网络必须在范围内（保存前要先测试连接）。

## 文件

| 文件 | 作用 |
|---|---|
| `sc_wifi.py` | 命令行工具：扫描、连接、发送 `setWifi`、等待结果 |
| `ble_frames.py` | 纯函数：BLE 配置通道的分包格式（与 `firmware/main/hal/hal_ble.cpp` 一致） |
| `test_ble_frames.py` | 单元测试（只用标准库，不需要蓝牙） |
| `requirements.txt` | `bleak`（BLE 库，使用 Windows 自带蓝牙协议栈） |

## 安装（一次）

带蓝牙的 Windows 10/11，Python 3.10 或更新：

```bat
cd <repo>\tools\sc-wifi
python -m venv .venv
.venv\Scripts\python -m pip install -r requirements.txt
```

## 使用

1. 在机器人上：回到主界面，打开 **SETUP > Wi-Fi > Change Wi-Fi**，点 **Next**。屏幕显示
   "Look for me in the app"，此时机器人以 `StackChan` 名称广播。
2. 关掉手机上的 StackChan World App（同一时间只能有一个 BLE 客户端连接）。
3. 在电脑上（用 PowerShell 或 cmd，密码输入提示才能正常工作）：
   ```bat
   .venv\Scripts\python sc_wifi.py --ssid "Office-2.4G"
   ```
   会提示输入密码，输入时不显示。直接回车表示开放网络。也可以用 `--password`，但密码会留在命令历史里。
4. 机器人显示 "Verifying..."。成功时脚本打印 `OK`，机器人约 4 秒后重启，并连接范围内信号最强的已保存网络。

选项：`--address AA:BB:...` 在多台机器人同时处于配网模式时指定一台；`--scan-timeout` 扫描时间（默认 6 秒）；
`--timeout` 等待结果的时间（默认 60 秒，机器人自己 30 秒后放弃）。

退出码：`0` 已保存，`1` 机器人连不上该网络，`2` 输入有误、找不到机器人或缺少 `bleak`。

## 常见问题

| 提示 | 含义 |
|---|---|
| `Found StackChan, but it is not in Wi-Fi setup mode` | 按第 1 步操作；Wi-Fi 的 BLE 服务只在那个界面运行 |
| `No StackChan found` | 机器人太远、电脑蓝牙没开，或手机 App 还连着 |
| `FAILED (authentication_failed)` / `(handshake_failed)` | 密码错误（或信号太弱），什么都没保存 |
| `FAILED (ap_not_found)` / `(not_2g4)` | 看不到该网络：只有 5 GHz、不在范围内或 SSID 写错 |
| `FAILED (connection_timeout)` / `(dhcp_timeout)` | 路由器没完成连接或没分配 IP 地址 |
| `FAILED (busy)` | 上一次尝试还在进行，等 30 秒 |

## 协议（参考）

- 配网服务 `e2e5e5ff-1234-5678-1234-56789abcdef0`，配置特征值
  `e2e5e5e3-1234-5678-1234-56789abcdef0`（带响应写入 + notify）。
- 请求：`{"cmd":"setWifi","data":{"ssid":"...","password":"..."}}`
- 回复（notify）：`{"cmd":"notifyState","data":{"type":T,"state":"..."}}`，`T` = 0 连接中，1 已连接，
  2 失败（另带 `errorCode`、`errorMessage`、`reason`、`ssid`）。
- 超过一个 ATT 载荷（MTU - 3）的值会拆成多包，每包带 10 字节头
  `AA 55 C3 01 | 序号 u16 | 总包数 u16 | 总长度 u16`（大端）。见 `ble_frames.py`。

运行测试：`python test_ble_frames.py`。
