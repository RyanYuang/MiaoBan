# Oye BLE + Protobuf 设备协议（v1）

固件通过 **NimBLE GATT** 承载 **Protobuf（nanopb）** 消息，用于手机配对、Wi‑Fi 配网、读取 OTA 元数据与用户绑定信息。固件大包升级仍走 **HTTP OTA**（`Ota::CheckVersion` / `StartUpgrade`），不经 BLE 传固件。

**手机 App 开发**请阅读：[oye-ble-mobile-app_zh.md](./oye-ble-mobile-app_zh.md)（扫描、配对、分片、命令与完整业务流程）。

## 启用方式

- menuconfig：`WiFi Configuration Method` → **Oye BLE (Protobuf)**（`CONFIG_USE_OYE_BLE_PROVISIONING`）
- 4.3C 板：[`config.json`](../main/boards/waveshare/esp32-s3-touch-lcd-4.3c/config.json) 已默认开启并关闭 Hotspot

## GATT

| 项 | UUID |
|----|------|
| Service | `6fa50001-0000-1000-8000-00805f9b34fb` |
| `cmd`（Write / Write No Rsp） | `6fa50002-0000-1000-8000-00805f9b34fb` |
| `rsp`（Notify） | `6fa50003-0000-1000-8000-00805f9b34fb` |

广播名：`Oye-` + MAC 后缀（如 `Oye-34fb`）。

## 分片

大于 MTU 的载荷使用 `oye.device.v1.Chunk`（`seq` / `total` / `data`）分包；手机对 `cmd` 写入分片，设备对 `rsp` Notify 分片。收齐后解码 `Envelope`。

## 命令（`Envelope.cmd`）

| CMD | 值 | 说明 | 需 Bond |
|-----|-----|------|---------|
| `CMD_GET_DEVICE_INFO` | 1 | 设备信息 | 否 |
| `CMD_GET_OTA_INFO` | 2 | OTA 缓存元数据 | 否 |
| `CMD_GET_USER_INFO` | 3 | 激活/Token 状态 | 否 |
| `CMD_SET_WIFI` | 10 | 下发 SSID/密码 | 是 |
| `CMD_SET_USER_TOKEN` | 11 | 写入 NVS `user.access_token` | 是 |
| `CMD_REFRESH_OTA` | 12 | 联网时 HTTP `CheckVersion` | 否 |
| `CMD_START_PAIRING` | 20 | 占位（广播已由配网模式开启） | 否 |
| `CMD_UNPAIR` | 21 | 清除 user NVS 与 BLE bond | 是 |

协议源文件：[`proto/oye/device/v1/device.proto`](../proto/oye/device/v1/device.proto)

重新生成 C 代码：

```bash
./proto/generate.sh
```

## 推荐 App 流程

1. 扫描并连接 `Oye-*`，完成 LE 配对（Bond）
2. `CMD_GET_DEVICE_INFO`
3. `CMD_SET_WIFI`（Bond 后）
4. 等待设备 Wi‑Fi 连接（设备侧会关闭 BLE 广播）
5. 再次连接或事先缓存：`CMD_REFRESH_OTA` → `CMD_GET_OTA_INFO` / `CMD_GET_USER_INFO`
6. `CMD_SET_USER_TOKEN` 绑定账号

## 与 HTTP OTA 的关系

- `CheckVersion` 成功后写入 `OtaSnapshot`（NVS 命名空间 `ota_cache`）
- BLE 只读快照；`CMD_REFRESH_OTA` 在 Wi‑Fi 已连接时触发一次 HTTP 检查
- HTTP 请求若存在 `user.access_token`，自动加 `Authorization` 头

## 4.3C 实机验证清单

- [ ] 无已存 Wi‑Fi 冷启动：出现 BLE 广播，无 Hotspot `Xiaozhi-*`
- [ ] 手机 Bond 后 `SET_WIFI` 可连路由器
- [ ] Wi‑Fi 连接后 BLE 停止（日志 `Oye BLE service stopped`）
- [ ] 联网后 `REFRESH_OTA` / `GET_OTA_INFO` 与串口 `Ota` 一致
- [ ] `SET_USER_TOKEN` 后下次 OTA 请求带 Authorization
- [ ] `UNPAIR` 后需重新配对才能 `SET_WIFI`
- [ ] 贴纸聊天/语音对话功能无回归
