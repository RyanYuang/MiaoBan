# Oye BLE + Protobuf 设备协议（v1）

固件通过 **NimBLE GATT** 承载 **Protobuf（nanopb）** 消息，用于手机配对、Wi‑Fi 配网、读取 OTA 元数据与用户绑定信息。固件大包升级仍走 **HTTP OTA**（`Ota::CheckVersion` / `StartUpgrade`），不经 BLE 传固件。

| 文档 | 读者 |
|------|------|
| 本文 | 固件 / MCU（GATT、命令、分片） |
| [oye-ble-mobile-app_zh.md](./oye-ble-mobile-app_zh.md) | iOS / Android App |
| [oye-mcu-http-api_zh.md](./oye-mcu-http-api_zh.md) | MCU 联网后 HTTP/WebSocket（对话、会议、TTS、声纹） |

## 启用方式

- menuconfig：`WiFi Configuration Method` → **Oye BLE (Protobuf)**（`CONFIG_USE_OYE_BLE_PROVISIONING`）
- 具体板级配置见固件仓库 `boards/*/config.json`

## GATT

| 项 | UUID |
|----|------|
| Service | `6fa50001-0000-1000-8000-00805f9b34fb` |
| `cmd`（Write / Write No Rsp） | `6fa50002-0000-1000-8000-00805f9b34fb` |
| `rsp`（Notify） | `6fa50003-0000-1000-8000-00805f9b34fb` |

广播名：`Oye-` + MAC 后缀（如 `Oye-34fb`）。

## 分片

大于 MTU 的载荷使用 `oye.device.v1.Chunk`（`seq` / `total` / `data`）分包；手机对 `cmd` 写入分片，设备对 `rsp` Notify 分片。收齐后解码 `Envelope`。

App 侧默认每片 `data` ≤ **180 字节**；固件侧单 Chunk 消息 `data` 上限 **512 字节**。

## 命令（`Envelope.cmd`）

| CMD | 值 | 说明 | 需 Bond |
|-----|-----|------|---------|
| `CMD_GET_DEVICE_INFO` | 1 | 设备信息 | 否 |
| `CMD_GET_OTA_INFO` | 2 | OTA 缓存元数据 | 否 |
| `CMD_GET_USER_INFO` | 3 | 激活/Token 状态 | 否 |
| `CMD_GET_WIFI` | 4 | 当前 Wi‑Fi 状态（`WifiInfo`，不含密码） | 否 |
| `CMD_SET_WIFI` | 10 | 下发 SSID/密码 | 是 |
| `CMD_SET_USER_TOKEN` | 11 | 写入 NVS `user.access_token` | 是 |
| `CMD_REFRESH_OTA` | 12 | 联网时 HTTP `CheckVersion` | 否 |
| `CMD_START_PAIRING` | 20 | 占位（广播已由配网模式开启） | 否 |
| `CMD_UNPAIR` | 21 | 清除 user NVS 与 BLE bond | 是 |

**协议唯一源文件**（App 与文档以此为准）：

[`Flutter/oyeo2app/proto/oye/device/v1/device.proto`](../Flutter/oyeo2app/proto/oye/device/v1/device.proto)

固件仓库内重新生成 C 代码（路径以固件工程为准）：

```bash
./proto/generate.sh
```

## 推荐流程（固件视角）

1. 无 Wi‑Fi 或配网模式：开启 BLE 广播
2. App 连接并完成 LE Bond
3. 响应 `GET_DEVICE_INFO` / `GET_WIFI`
4. Bond 后处理 `SET_WIFI` → Station 连路由器 → **关闭 BLE**
5. 联网后 HTTP：`CheckVersion` / OTA；App 可经 BLE（若仍广播）发 `REFRESH_OTA`、`SET_USER_TOKEN`
6. `SET_USER_TOKEN` 后，设备 HTTP 请求自动带 `Authorization: Bearer <token>`

## 与 HTTP OTA / 云端 API 的关系

| 能力 | 通道 |
|------|------|
| 配网、写 token | BLE（本文） |
| OTA 检查与下载 | HTTP（固件 `Ota` 模块） |
| 对话、会议、TTS、声纹 | HTTP/WebSocket，见 [oye-mcu-http-api_zh.md](./oye-mcu-http-api_zh.md) |

- `CheckVersion` 成功后写入 `OtaSnapshot`（NVS `ota_cache`）
- BLE `GET_OTA_INFO` 只读快照；`REFRESH_OTA` 在 Wi‑Fi 已连接时触发一次 HTTP 检查
- HTTP 请求若存在 `user.access_token`，自动加 `Authorization` 头

## 实机验证清单

- [ ] 无已存 Wi‑Fi 冷启动：出现 BLE 广播 `Oye-*`
- [ ] `GET_WIFI` 返回 `connected` / `saved_ssid` 与实机一致（不含密码）
- [ ] 手机 Bond 后 `SET_WIFI` 可连路由器
- [ ] Wi‑Fi 连接后 BLE 停止（日志如 `Oye BLE service stopped`）
- [ ] 联网后 `REFRESH_OTA` / `GET_OTA_INFO` 与 HTTP OTA 一致
- [ ] `SET_USER_TOKEN` 后 HTTP 请求带 Authorization
- [ ] `UNPAIR` 后需重新 Bond 才能 `SET_WIFI`
- [ ] 贴纸/语音等业务无回归

协议变更时请先改 `device.proto`，再同步本文档与 [oye-ble-mobile-app_zh.md](./oye-ble-mobile-app_zh.md)。
