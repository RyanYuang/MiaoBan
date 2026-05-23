# Oye 设备 MCU — 云端 HTTP/WebSocket API 开发指南

| 项 | 值 |
|----|-----|
| 文档版本 | **1.2** |
| 更新日期 | **2026-05-19** |
| API 前缀 | **`/api/v1/mcu`**（设备专用，勿调用 `/meetings` 等 App 路由） |
| 适用固件 | Oye BLE 配网 + HTTP OTA 分支 |
| 协议源 | [`Flutter/oyeo2app/proto/oye/device/v1/device.proto`](../Flutter/oyeo2app/proto/oye/device/v1/device.proto) |

### 变更记录（MCU 请关注）

| 版本 | 日期 | 变更 |
|------|------|------|
| 1.2 | 2026-05-19 | **全部业务路由迁至 `/api/v1/mcu/*`**；语音助手独立 `WS /mcu/voice-chat/ws`；勿再使用 `/meetings/*` |
| 1.1 | 2026-05-19 | 新增 **§9 流式 TTS**；声纹 **register/verify** 与后端对齐；开发环境 Base URL 示例更新 |
| 1.0 | — | 初版：用户 / 对话 / 会议 / 声纹 |

本文档面向 **设备固件（MCU / ESP32 等）** 工程师，说明设备在 **Wi‑Fi 已连接** 且已通过 BLE 获得用户 `access_token` 后，如何调用 Oye 后端实现：

- **用户信息查询**
- **AI 语音/文本对话**（经后端 LLM）
- **会议纪要**（上传录音、实时转写、查询纪要）
- **声纹管理**（注册、校验、特征库）
- **语音合成 TTS**（朗读 AI 回复，SSE 流式 MP3）

BLE 配网与 token 下发见 [oye-ble-mobile-app_zh.md](./oye-ble-mobile-app_zh.md)（App 侧）；GATT 命令定义见 [oye-ble-proto_zh.md](./oye-ble-proto_zh.md)。后端全量接口见 [API_DOCUMENT.md](./API_DOCUMENT.md)。

---

## 1. 前置条件

| 条件 | 说明 |
|------|------|
| 网络 | 设备已 `SET_WIFI` 成功，可访问后端 **HTTPS**（生产环境建议 TLS 1.2+） |
| 鉴权 | NVS 中已有 App 下发的 `access_token`（`CMD_SET_USER_TOKEN`），**不是**设备自行登录 |
| Token 格式 | 与 App 登录一致：`phone_<手机号>.<expire>.<random>` |
| 请求头 | `Authorization: Bearer <access_token>`（注意 `Bearer` 后有一个空格） |
| Content-Type | JSON 接口：`application/json; charset=utf-8`；上传音频：`multipart/form-data` |

**MCU 不应在生产环境实现手机号登录**；token 由手机 App 经 BLE 写入。实验室调试可临时调用 `POST /users/phone-login` 获取 token。

Token 默认有效期约 **7 天**（`expires_in=604800`），过期后需用户再次打开 App 连接设备并重新 `SET_USER_TOKEN`。

---

## 2. 服务地址

| 环境 | Base URL（示例） | MCU API 前缀 |
|------|------------------|----------------|
| 开发 | `http://192.168.11.156:8000`（以局域网实际 IP 为准） | `/api/v1/mcu` |
| 生产 | 由运维配置（如 `https://api.example.com`） | `/api/v1/mcu` |

> MCU 固件中 Base URL 建议由 **NVS 可配置项** 或编译宏注入，勿写死开发机 IP。  
> **仅调用 `/api/v1/mcu/*`**；`/api/v1/meetings`、`/api/v1/chats` 等供手机 App 使用。

完整路径 = `{BaseURL}{MCU前缀}{资源路径}`，例如：

```text
GET https://api.example.com/api/v1/mcu/me
```

健康检查（无需鉴权）：`GET {BaseURL}/health`

---

## 3. 统一响应格式

### 3.1 成功

```json
{
  "code": 0,
  "message": "success",
  "data": { }
}
```

HTTP 状态码一般为 `200`；创建资源常为 `201`；异步任务接受常为 `202`。

### 3.2 业务错误

```json
{
  "code": 4010,
  "message": "缺少有效 Bearer Token",
  "data": null
}
```

| code | 含义 | MCU 建议处理 |
|------|------|----------------|
| `4000` | 参数错误 | 检查请求体/表单 |
| `4010` | 未认证 / Token 无效或过期 | 提示用户在 App 中重新绑定账号 |
| `4030` | 无权限 | 非资源创建者等 |
| `4040` | 资源不存在 | 检查 ID |
| `4090` | 冲突 | 重试或提示用户 |
| `5000` | 服务端错误 | 退避重试 |

### 3.3 分页列表

```json
{
  "code": 0,
  "message": "success",
  "data": {
    "items": [ ],
    "total": 100
  }
}
```

查询参数：`page`（从 1 开始，默认 1）、`page_size`（默认 20，最大 100）。

---

## 4. 鉴权

除 `POST /mcu/phone-login`、`GET /health` 外，**所有 MCU 业务接口**均需：

```http
Authorization: Bearer phone_13800138000.1710000000.xxxxxx
```

与 BLE 写入 NVS 的字符串相同；固件发 HTTP 时若 NVS 中无 `Bearer ` 前缀，需自行拼接。

WebSocket 使用 **Query 参数** `token=`（见 §7.4、§7.5），值与上相同，**不要**加 `Bearer ` 前缀。

---

## 5. 用户信息

### 5.1 获取当前用户 — `GET /mcu/me`

**鉴权**：是  

**用途**：校验 token 是否有效；展示用户昵称、手机号等。

**响应 `data` 字段**：

| 字段 | 类型 | 说明 |
|------|------|------|
| `id` | int | 用户 ID |
| `phone` | string | 手机号 |
| `nickname` | string | 昵称 |
| `avatar_url` | string | 头像 URL，可为空 |
| `created_at` | string | ISO8601 时间 |

**示例**：

```http
GET /api/v1/mcu/me HTTP/1.1
Host: api.example.com
Authorization: Bearer phone_13800138000.1710000000.xxxxxx
```

```json
{
  "code": 0,
  "message": "获取成功",
  "data": {
    "id": 1,
    "phone": "13800138000",
    "nickname": "Ryan",
    "avatar_url": "",
    "created_at": "2026-04-21T08:00:00+00:00"
  }
}
```

### 5.2 更新昵称 — `PATCH /mcu/me`

**请求体**：

```json
{ "nickname": "新昵称" }
```

### 5.3 关联用户（同群）— `GET /mcu/related`

返回与当前用户至少共处于一个群组的用户列表（联系人场景）。`data` 为数组，元素含 `user_id`、`phone`、`nickname`、`shared_groups` 等。

### 5.4 调试：手机号登录 — `POST /mcu/phone-login`

**鉴权**：否（仅开发调试用）

```json
{ "phone": "13800138000" }
```

响应 `data.token.access_token` 即为后续 Bearer token。

---

## 6. AI 对话（文本）

后端对话接口为 **文本入、文本出**。MCU 若要做「语音对话」，需在本机完成 **录音 → ASR（语音识别）→ 文本**，再调用下列 HTTP 接口；或将音频走 §7 会议流式 ASR 得到文本后再发往对话接口。

### 6.1 创建会话 — `POST /mcu/chats/sessions`

```json
{
  "title": "设备助手",
  "type": "personal",
  "group_id": null
}
```

| 字段 | 说明 |
|------|------|
| `title` | 必填，1~200 字符 |
| `type` | `personal`（默认）或 `group` |
| `group_id` | 群聊时填群组 ID |

**响应 `data`**：`id`（session_id）、`title`、`owner_id`、`type`、`created_at`。

### 6.2 发送消息（同步，含 AI 回复）— `POST /mcu/chats/sessions/{session_id}/messages`

```json
{
  "role": "user",
  "content": "今天天气怎么样？"
}
```

**响应 `data`**：

| 字段 | 说明 |
|------|------|
| `user_message` | 用户消息对象 |
| `assistant_message` | AI 回复对象 |

消息对象含 `id`、`session_id`、`role`（`user` / `assistant` / `system`）、`content`、`created_at`。

**MCU 推荐**：使用此同步接口；解析 `assistant_message.content` 后 TTS 播报。

### 6.3 查询历史消息 — `GET /mcu/chats/sessions/{session_id}/messages?page=1&page_size=20`

> MCU 固件 **不提供** `/mcu/chats/.../messages/stream`（SSE 流式）；请用 §6.2 同步接口或 §7.5 语音对话 WebSocket。

- 响应类型：`text/event-stream`（SSE）
- 事件类型：`user_message`、`delta`、`done`
- **MCU 实现成本较高**（需解析 SSE），资源受限设备建议用 §6.2 同步接口。

### 6.5 设备侧语音对话推荐流程

```text
1. POST /mcu/chats/sessions      → 保存 session_id（可持久化）
2. [本地麦克风] → PCM/WAV
3. [本地或云端 ASR] → 用户文本
4. POST /mcu/chats/sessions/{id}/messages → assistant_message.content
5. POST /mcu/tts/stream          → MP3
```

**推荐**：单连接 `WS /mcu/voice-chat/ws`（§7.5）完成 ASR+LLM+TTS，无需拆分 HTTP。

---

## 7. 会议纪要

### 7.1 任务状态

| status | 含义 |
|--------|------|
| `pending` | 已创建，等待处理 |
| `submitting` | 提交 ASR 中 |
| `transcribing` | 转写中 |
| `summarizing` | 生成摘要/纪要中 |
| `done` | 完成 |
| `failed` | 失败，见 `error_message` |

状态流转：`pending → submitting → transcribing → summarizing → done`（任一步可 `failed`）。

**MCU 必须轮询** `GET /mcu/meetings/{id}`，建议间隔 **2~5 秒**，直至 `done` 或 `failed`。

### 7.2 上传会议录音 — `POST /mcu/meetings/upload`

**Content-Type**：`multipart/form-data`

| 表单字段 | 类型 | 必填 | 说明 |
|----------|------|------|------|
| `audio` | file | 是 | mp3 / wav / m4a / aac / flac / ogg / opus，默认上限 **200MB** |
| `title` | string | 否 | 会议标题 |
| `group_id` | int | 否 | 关联群组 |

**示例（伪代码）**：

```http
POST /api/v1/mcu/meetings/upload HTTP/1.1
Authorization: Bearer <token>
Content-Type: multipart/form-data; boundary=----OyeBoundary

------OyeBoundary
Content-Disposition: form-data; name="title"

周会录音
------OyeBoundary
Content-Disposition: form-data; name="audio"; filename="meet.wav"
Content-Type: audio/wav

<binary>
------OyeBoundary--
```

**响应（201）**：`data` 为完整 `MeetingResponse`（含 `id`、`status=pending` 等）。随后轮询详情。

### 7.3 提交已有转写文本 — `POST /mcu/meetings/stream/transcript`

设备若 **自行完成 ASR**（如直连讯飞），可跳过上传音频，直接提交全文生成纪要：

```json
{
  "title": "实时会议",
  "transcript_text": "大家早上好，今天讨论 Q3 复盘……",
  "group_id": null
}
```

**响应（201）**：`status` 进入摘要流程，同样轮询 `GET /meetings/{id}`。

### 7.4 实时流式语音识别（会议纪要）— `WebSocket /mcu/meetings/stream/ws`

**URL**：

```text
wss://{host}/api/v1/mcu/meetings/stream/ws?token=<access_token>
```

**消息顺序**：

1. **文本 JSON（首条）**

```json
{
  "action": "start",
  "title": "设备端会议",
  "save_meeting": true,
  "group_id": null,
  "audio": { "format": "pcm", "rate": 16000, "bits": 16, "channel": 1 }
}
```

| 字段 | 说明 |
|------|------|
| `save_meeting` | `true` 时结束后写入会议记录并异步生成纪要 |
| `audio.format` | `pcm` 或 `wav`，内线码 **pcm_s16le**，采样率建议 **16000**，单声道 |

> **语音助手**（ASR→LLM→TTS）请用 §7.5 专用 WebSocket，勿在本接口传 `mode=voice_chat`。

2. **二进制帧**：PCM 切片，建议每包 **100~200ms**（16kHz × 2 字节 × 0.1s ≈ 3200 字节/包）

3. **文本 JSON（结束）**

```json
{ "action": "end" }
```

**服务端下行（文本 JSON）**：

| type | 说明 |
|------|------|
| `asr` | `{"type":"asr","text":"<累计全文>"}` |
| `info` | 提示信息 |
| `done` | `{"type":"done","text":"...","meeting_id":123}` |
| `error` | `{"type":"error","message":"..."}` |

若 `save_meeting=true`，用 `meeting_id` 轮询 §7.6 直至 `done`。

### 7.5 语音大模型对话（ASR → LLM → TTS）— `WebSocket /mcu/voice-chat/ws`

**URL**：

```text
wss://{host}/api/v1/mcu/voice-chat/ws?token=<access_token>
```

上传 PCM 与 `end` 约定与 §7.4 相同。**ASR** 由 Oye 后端中继 **讯飞 RTASR LLM**（`pcm_s16le` 16kHz）；结束后自动 **LLM + 火山 TTS**，MCU **无需**再调 HTTP `/mcu/tts/stream`。

**首条示例**：

```json
{
  "action": "start",
  "chat_session_id": null,
  "audio": { "format": "pcm", "rate": 16000, "bits": 16, "channel": 1 }
}
```

**下行消息（按典型顺序）**：

| type | 说明 |
|------|------|
| `asr` | 识别过程中累计全文（与会议模式相同） |
| `asr_final` | ASR 结束后的最终用户语句 |
| `llm_start` | 开始请求大模型，`chat_session_id` |
| `llm_text` | 助手完整回复文本，`message_id` |
| `tts_start` | 开始下发语音，`format` 一般为 `mp3` |
| `tts_chunk` | `seq` 递增，`data` 为 **Base64** 编码的 MP3 分片 |
| `tts_done` | TTS 结束，`chunks` 为分片总数 |
| `voice_chat_done` | 整轮结束，含 `user_text`、`assistant_text`、`chat_session_id` |
| `error` | 任一步失败 |

**MCU 播放**：按 `seq` 顺序 Base64 解码 `data` 后拼接或逐包解码播放 MP3；收到 `tts_done` 或 `voice_chat_done` 后可结束本轮 UI。

**注意**：必须发送 **WebSocket 二进制帧** PCM；仅发文本 JSON 不会产生识别结果。需配置服务端 `XFYUN_RTASR_APP_ID` / `XFYUN_RTASR_ACCESS_KEY_ID` / `XFYUN_RTASR_ACCESS_KEY_SECRET`、`AI_PROVIDER` 与 `VOLCENGINE_TTS_API_KEY`。

> §7.5 已覆盖设备侧 ASR，**无需**调用 App 专用 `GET /meetings/stream/xfyun-ws-url`。

### 7.6 会议列表 — `GET /mcu/meetings?page=1&page_size=20`

`data.items[]` 为精简字段：`id`、`title`、`status`、`summary`、`audio_duration_ms`、`created_at` 等（无完整转写）。

### 7.7 会议详情 — `GET /mcu/meetings/{meeting_id}`

**鉴权**：是（仅创建者）

**响应 `data` 主要字段**：

| 字段 | 说明 |
|------|------|
| `transcript_text` | 全文转写 |
| `transcript_segments` | 分段：`start_ms`、`end_ms`、`time`、`speaker`、`text` |
| `summary` | LLM 摘要 |
| `key_points` | `[{ "text", "assign" }]` |
| `attendees` | 说话人列表 |
| `error_message` | 失败原因 |

### 7.8 其他会议接口

| 方法 | 路径 | 说明 |
|------|------|------|
| `PATCH` | `/mcu/meetings/{id}` | 更新标题 `{"title":"..."}` |
| `DELETE` | `/mcu/meetings/{id}` | 删除会议及音频文件 |
| `POST` | `/mcu/meetings/{id}/regenerate-summary` | 仅重新生成摘要（202） |
| `POST` | `/mcu/meetings/{id}/refresh` | 任务卡住时兜底续推（202） |

---

## 8. 声纹管理

后端代理讯飞声纹识别（新）API。音频要求：**16kHz / 16bit / 单声道 WAV**，建议时长 **3~5 秒**。

用户声纹默认写入特征库 `oye_voiceprints`（可由环境变量 `XFYUN_VOICEPRINT_USER_GROUP_ID` 配置），`feature_id` 形如 `u_{user_id}`（最长 32 字符）。

### 8.1 创建特征库 — `POST /mcu/voiceprint/groups`

```json
{
  "group_name": "项目 A 声纹库",
  "group_desc": "可选描述"
}
```

**响应 `data`**：`group_id`、`group_name`、`provider`（`xfyun_voiceprint_new`）、`request_id`。

### 8.2 删除特征库 — `DELETE /mcu/voiceprint/groups/{group_id}`

### 8.3 注册当前用户声纹 — `POST /mcu/voiceprint/me/register`

**Content-Type**：`multipart/form-data`  

| 字段 | 说明 |
|------|------|
| `audio` | WAV 文件 |

**响应 `data`**：

```json
{
  "group_id": "oye_voiceprints",
  "feature_id": "u_1",
  "request_id": "..."
}
```

### 8.4 校验当前用户声纹 — `POST /mcu/voiceprint/me/verify`

表单同上。

**响应 `data`**：

| 字段 | 类型 | 说明 |
|------|------|------|
| `matched` | bool | 是否通过 |
| `score` | float | 相似度得分 |
| `threshold` | float | 阈值（默认约 0.75） |
| `request_id` | string | 讯飞请求 ID |

**MCU 建议**：`score >= threshold` 视为本人；否则拒绝敏感操作或提示重新注册。

---

## 9. 语音合成（TTS）

后端代理火山引擎 **豆包语音合成大模型 V3**（单向 SSE）。MCU 用其将 **AI 文本回复** 转为 **MP3** 后本地播放（I2S / 外置解码芯片）。

### 9.1 流式合成 — `POST /mcu/tts/stream`

| 项 | 值 |
|----|-----|
| 鉴权 | 是（`Authorization: Bearer`） |
| Content-Type | `application/json; charset=utf-8` |
| 响应类型 | `text/event-stream`（SSE） |
| 超时建议 | 客户端 **≥ 90s**（长文本合成较慢） |

**请求体**：

```json
{
  "text": "今天天气不错，适合出门散步。"
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `text` | string | 1~4000 字符；服务端会去除 Markdown 并截断至 `VOLCENGINE_TTS_MAX_TEXT_LENGTH`（默认 2000） |

**SSE 下行**（每行 `data: {json}\n\n`）：

| `type` | 字段 | 说明 |
|--------|------|------|
| `chunk` | `data` | Base64 编码的 **MP3** 分片 |
| `chunk` | `format` | 固定 `mp3`（与 `VOLCENGINE_TTS_FORMAT` 一致） |
| `done` | `format` | 合成结束，无更多音频 |
| `error` | `message` | 业务或配置错误，应停止接收 |

**示例流**：

```text
data: {"type":"chunk","data":"//uQx...","format":"mp3"}

data: {"type":"chunk","data":"SUQz...","format":"mp3"}

data: {"type":"done","format":"mp3"}
```

**MCU 解析伪代码**：

```c
// 1. POST /api/v1/mcu/tts/stream，Body: {"text":"..."}
// 2. 按行读取；以 "data: " 开头的行 JSON 解析
// 3. type==chunk → base64_decode(data) 追加到 mp3_buffer
// 4. type==done  → 关闭 HTTP，将 mp3_buffer 送播放器或写 SPIFFS
// 5. type==error  → 提示用户，勿播放残缺文件
```

**默认音频参数**（服务端配置，MCU 按 MP3 解码即可）：

| 配置项 | 默认值 |
|--------|--------|
| 采样率 | 24000 Hz |
| 格式 | mp3 |
| 音色 | `zh_female_shuangkuaisisi_uranus_bigtts`（可在服务端 `.env` 调整） |

**注意**：

- TTS **密钥仅存在于服务端**（`VOLCENGINE_TTS_API_KEY`），MCU **不得**直连火山 TTS。
- 无「单包返回整段 MP3」的 REST 接口；必须实现 **SSE 行解析** 或先用 App/网关聚合（不推荐 MCU 侧聚合）。
- 内存紧张时：每收到 `chunk` 即解码写入 **环形缓冲 / 文件**，避免整段 MP3 常驻 RAM。

### 9.2 与 AI 对话组合（设备语音助手）

```text
1. [本地 ASR] → 用户文本
2. POST /mcu/chats/sessions/{id}/messages  → assistant_message.content
3. POST /mcu/tts/stream                    → 累积 MP3
4. [本地播放 MP3]
```

推荐改用 **§7.5** `WS /mcu/voice-chat/ws` 单连接完成全流程。

---

## 10. 推荐业务流程

### 10.1 上电自检（token + 用户）

```mermaid
sequenceDiagram
    participant MCU
    participant Cloud as Oye Backend

    MCU->>Cloud: GET /mcu/me (Bearer token)
    alt code=0
        Cloud-->>MCU: UserResponse
    else code=4010
        Cloud-->>MCU: Token 失效
        Note over MCU: 提示 App 重新 SET_USER_TOKEN
    end
```

### 10.2 上传录音生成纪要

```mermaid
sequenceDiagram
    participant MCU
    participant Cloud as Oye Backend

    MCU->>Cloud: POST /mcu/meetings/upload (multipart audio)
    Cloud-->>MCU: meeting_id, status=pending
    loop 每 2~5s
        MCU->>Cloud: GET /mcu/meetings/{id}
        Cloud-->>MCU: status
    end
    Cloud-->>MCU: status=done, summary, key_points
```

### 10.3 实时会议 + 纪要

```mermaid
sequenceDiagram
    participant MCU
    participant Cloud as Oye Backend

    MCU->>Cloud: WS /mcu/meetings/stream/ws?token=...
    MCU->>Cloud: {"action":"start","save_meeting":true,...}
    loop 录音
        MCU->>Cloud: binary PCM frames
        Cloud-->>MCU: {"type":"asr","text":"..."}
    end
    MCU->>Cloud: {"action":"end"}
    Cloud-->>MCU: {"type":"done","meeting_id":12}
    loop 轮询
        MCU->>Cloud: GET /mcu/meetings/12
    end
```

### 10.4 声纹注册与校验

```text
1. POST /mcu/voiceprint/me/register  (multipart audio)  → 首次录入
2. 日常使用：POST /mcu/voiceprint/me/verify           → matched==true 放行
```

---

## 11. MCU 实现要点

| 主题 | 建议 |
|------|------|
| HTTP 客户端 | 支持 HTTPS、分块 `multipart/form-data`、足够大的接收缓冲（会议详情 JSON 可能较大） |
| SSE / TTS | 按行解析 `data:` JSON；`chunk` 用 Base64 解码；收到 `done` 再开始播放 |
| WebSocket | 会议流式需同时发 **文本帧** 与 **二进制帧**；注意心跳与断线重连 |
| 内存 | 上传大文件用 **分块 multipart**；TTS 用 **流式写文件** 避免整段 MP3 进 RAM |
| Token 存储 | NVS 最大 256 字节（与 `SetUserTokenRequest` 一致）；勿打日志明文 |
| 失败重试 | 网络错误指数退避；`4010` 不要重试，应提示 App 重新 `SET_USER_TOKEN` |
| BLE 与 HTTP | Wi‑Fi 连网后 BLE 可能关闭；HTTP 业务不依赖 BLE 保持连接 |
| 语音对话 | 推荐 `WS /mcu/voice-chat/ws`（ASR→LLM→TTS）；或 HTTP `/mcu/chats` + `/mcu/tts/stream` |

### 11.1 固件侧 HTTP 能力自检清单

- [ ] `GET /mcu/me` 校验 token
- [ ] `POST /mcu/chats/sessions` + `POST .../messages` 同步对话
- [ ] `POST /mcu/meetings/upload` multipart 上传
- [ ] `GET /mcu/meetings/{id}` 轮询 `status`
- [ ] `WS /mcu/meetings/stream/ws` 文本 + 二进制帧（会议纪要）
- [ ] `WS /mcu/voice-chat/ws` 收齐 `tts_chunk` → 播 MP3 → `voice_chat_done`
- [ ] `POST /mcu/voiceprint/me/register` 与 `verify`（multipart WAV）
- [ ] `POST /mcu/tts/stream` SSE 解析 + MP3 播放
- [ ] HTTPS 根证书（生产环境）

---

## 12. 接口速查表

| 能力 | 方法 | 路径（前缀 `/api/v1/mcu`） |
|------|------|---------------------------|
| 调试登录 | POST | `/phone-login` |
| 当前用户 | GET | `/me` |
| 更新昵称 | PATCH | `/me` |
| 关联用户 | GET | `/related` |
| 创建对话 | POST | `/chats/sessions` |
| 发消息（同步） | POST | `/chats/sessions/{id}/messages` |
| 消息历史 | GET | `/chats/sessions/{id}/messages` |
| 上传会议音频 | POST | `/meetings/upload` |
| 提交转写文本 | POST | `/meetings/stream/transcript` |
| 会议流式 ASR | WS | `/meetings/stream/ws?token=` |
| **语音助手** | WS | `/voice-chat/ws?token=` |
| 会议列表 | GET | `/meetings` |
| 会议详情 | GET | `/meetings/{id}` |
| 更新会议 | PATCH | `/meetings/{id}` |
| 删除会议 | DELETE | `/meetings/{id}` |
| 重新生成摘要 | POST | `/meetings/{id}/regenerate-summary` |
| 续推任务 | POST | `/meetings/{id}/refresh` |
| 创建声纹库 | POST | `/voiceprint/groups` |
| 删除声纹库 | DELETE | `/voiceprint/groups/{group_id}` |
| 注册我的声纹 | POST | `/voiceprint/me/register` |
| 校验我的声纹 | POST | `/voiceprint/me/verify` |
| 流式 TTS | POST | `/tts/stream`（SSE） |

---

## 13. 相关文档

| 文档 | 说明 |
|------|------|
| [oye-ble-proto_zh.md](./oye-ble-proto_zh.md) | BLE GATT / Protobuf 命令（固件与 App 对齐） |
| [oye-ble-mobile-app_zh.md](./oye-ble-mobile-app_zh.md) | 手机 App 配网流程（MCU 了解 Bond / token 时机） |
| [API_DOCUMENT.md](./API_DOCUMENT.md) | 后端全量 API（含好友、群组、待办等） |
| [`device.proto`](../Flutter/oyeo2app/proto/oye/device/v1/device.proto) | BLE `Envelope` / `Command` 定义 |

协议与接口以仓库内 **实际代码** 为准；后端或 `device.proto` 变更时请同步更新本文档并通知 MCU 固件组。
