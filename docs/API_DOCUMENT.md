# Oye Backend 接口文档

本文档基于当前后端实现（FastAPI）整理，适用于 App / 管理端等前端联调。

## 1. 基础信息

- 基础路径：`/api/v1`
- 健康检查：`GET /health`（无需鉴权）
- 业务接口统一返回：

```json
{
  "code": 0,
  "message": "success",
  "data": {}
}
```

## 2. 鉴权说明

- 公开登录接口（无需鉴权）：`POST /api/v1/users/phone-login`（手机号）
- 其余业务接口默认需要 `Authorization` 请求头：

```text
Authorization: Bearer <access_token>
```

> `access_token` 为占位 token，形如 `<subject>.<expire>.<random>`，其中 **`subject` 必须为 `phone_<数字手机号>`**，服务端按手机号解析用户（聊天与其它业务共用）。

## 3. 通用约定

### 3.1 分页参数

- `page`：页码，从 1 开始，默认 `1`
- `page_size`：每页条数，默认 `20`，最大 `100`

分页返回结构：

```json
{
  "code": 0,
  "message": "success",
  "data": {
    "items": [],
    "total": 0
  }
}
```

### 3.2 常见业务错误码

- `4000`：参数错误
- `4010`：未认证/Token 无效
- `4030`：无权限
- `4040`：资源不存在
- `4090`：资源冲突
- `5000`：服务内部错误

错误响应示例：

```json
{
  "code": 4010,
  "message": "缺少有效 Bearer Token",
  "data": null
}
```

## 4. 枚举值说明

- `ChatSessionType`：`personal` | `group`
- `ChatMessageRole`：`user` | `assistant` | `system`
- `GroupMemberRole`：`owner` | `admin` | `member`
- `TodoStatus`：`open` | `done`
- `FriendRequestStatus`：`pending` | `accepted` | `rejected` | `cancelled`
- `MeetingStatus`：`pending` | `submitting` | `transcribing` | `summarizing` | `done` | `failed`

## 5. 接口列表

### 5.1 系统接口

#### 5.1.1 健康检查

- 方法与路径：`GET /health`
- 鉴权：否

响应示例：

```json
{
  "status": "ok",
  "env": "dev"
}
```

---

### 5.2 用户模块

#### 5.2.1 手机号登录

- 方法与路径：`POST /api/v1/users/phone-login`
- 鉴权：否

请求体：

```json
{
  "phone": "13800138000"
}
```

字段说明：

- `phone`：手机号，必填，长度 5~20（服务端会去掉空格与非数字字符）

成功响应（201）：未注册手机号会自动创建用户。

```json
{
  "code": 0,
  "message": "登录成功",
  "data": {
    "token": {
      "access_token": "phone_13800138000.1710000000.xxxxxx",
      "token_type": "Bearer",
      "expires_in": 604800
    },
    "user": {
      "id": 1,
      "phone": "13800138000",
      "nickname": "13800138000",
      "avatar_url": "",
      "created_at": "2026-04-21T08:00:00+00:00"
    }
  }
}
```

#### 5.2.2 获取当前用户

- 方法与路径：`GET /api/v1/users/me`
- 鉴权：是

成功响应：

```json
{
  "code": 0,
  "message": "获取成功",
  "data": {
    "id": 1,
    "phone": "13800138000",
    "nickname": "Ryan",
    "avatar_url": "https://example.com/avatar.png",
    "created_at": "2026-04-21T08:00:00+00:00"
  }
}
```

#### 5.2.3 关联用户（同群）

- 方法与路径：`GET /api/v1/users/related`
- 鉴权：是

返回与当前用户至少共处于一个群组的其他用户；用于「联系人」等场景。

成功响应：

```json
{
  "code": 0,
  "message": "获取成功",
  "data": [
    {
      "user_id": 2,
      "phone": "13900139000",
      "nickname": "Bob",
      "avatar_url": "",
      "shared_groups": [{ "id": 1, "name": "讨论组" }]
    }
  ]
}
```

---

### 5.3 好友模块

好友关系为 **双向**：仅在请求被 **同意**（或双方反向 pending 自动合并）后，`GET /friends` 可见。接口返回中的 `phone_masked` 为脱敏展示手机号。

#### 5.3.1 发起好友请求

- 方法与路径：`POST /api/v1/friends/requests`
- 鉴权：是

请求体：

```json
{
  "phone": "13800138000"
}
```

说明：

- 若对方手机号未注册 → `4040`
- 若已是好友 → `4090`
- 若已向对方发过 pending 请求 → `4090`
- 若对方 **已向你** 发过 pending 请求 → **自动互为好友**，响应 `merged_via_reverse_pending: true`，`status` 为 `accepted`

成功响应（201）：`data` 为好友请求对象（含 `from_user` / `to_user`，手机号均为 `phone_masked`）。

#### 5.3.2 收到的待处理好友请求

- 方法与路径：`GET /api/v1/friends/requests/incoming?page=1&page_size=20`
- 鉴权：是

分页字段见通用约定；`data.items` 为 `FriendRequestResponse` 列表。

#### 5.3.3 同意好友请求

- 方法与路径：`POST /api/v1/friends/requests/{request_id}/accept`
- 鉴权：是（仅接收方可操作）

#### 5.3.4 拒绝好友请求

- 方法与路径：`POST /api/v1/friends/requests/{request_id}/reject`
- 鉴权：是（仅接收方可操作）

#### 5.3.5 撤销发出的好友请求

- 方法与路径：`DELETE /api/v1/friends/requests/{request_id}`
- 鉴权：是（仅发起方可操作，且请求须为 `pending`）

#### 5.3.6 好友列表

- 方法与路径：`GET /api/v1/friends?page=1&page_size=20`
- 鉴权：是

`data.items` 每项包含：`friend`（`FriendUserPublic`）、`friends_since`。

#### 5.3.7 好友资料（校验互为好友）

- 方法与路径：`GET /api/v1/friends/{friend_user_id}`
- 鉴权：是

若非好友 → `4040`。

#### 5.3.8 删除好友

- 方法与路径：`DELETE /api/v1/friends/{friend_user_id}`
- 鉴权：是

移除双向好友关系。

#### 5.3.9 生成可过期好友邀请（二维码）

- 方法与路径：`POST /api/v1/friends/invites`
- 鉴权：是

说明：签发新的邀请会使该用户此前 **未使用且未过期** 的邀请立即失效（通过将 `expires_at` 置为当前时间）。明文 `token` 仅在本次响应返回一次；数据库存 **SHA256** 摘要。

请求体（可选）：

```json
{
  "ttl_seconds": 900
}
```

- `ttl_seconds`：可选；默认 `900`；服务端钳制在 **60～86400** 秒。

成功响应（201）：

```json
{
  "code": 0,
  "message": "邀请已生成",
  "data": {
    "token": "<urlsafe 明文>",
    "expires_at": "2026-05-02T12:00:00+00:00",
    "qr_payload": "oye:friend_invite:v1:<urlsafe 明文>"
  }
}
```

App 应将 `qr_payload` 编码为二维码供他人扫描。

#### 5.3.10 兑换好友邀请

- 方法与路径：`POST /api/v1/friends/invites/redeem`
- 鉴权：是

请求体：

```json
{
  "token": "oye:friend_invite:v1:<urlsafe 明文>"
}
```

`token` 可为完整 `qr_payload`，也可仅为明文段（与签发时的 `token` 字段一致）。

成功响应（200）：`data` 为邀请方用户的 `FriendUserPublic`（`phone_masked` 等），并与当前用户 **立即建立双向好友关系**。

业务错误：

- 邀请不存在、已使用、已过期、格式无效 → `4000`，文案统一为「邀请无效或已过期」类提示
- 扫描自己的邀请 → `4000`
- 与对方 **已是好友** → `4090`

邀请 **一次性**：兑换成功后同一令牌不可再用。

---

### 5.4 群组模块

#### 5.4.1 创建群组

- 方法与路径：`POST /api/v1/groups`
- 鉴权：是

请求体：

```json
{
  "name": "产品讨论群",
  "description": "用于日常需求讨论"
}
```

字段说明：

- `name`：必填，1~100 字符
- `description`：可选

成功响应（201）：

```json
{
  "code": 0,
  "message": "创建成功",
  "data": {
    "id": 10,
    "name": "产品讨论群",
    "description": "用于日常需求讨论",
    "owner_id": 1,
    "created_at": "2026-04-21T08:00:00+00:00"
  }
}
```

#### 5.4.2 我的群组列表

- 方法与路径：`GET /api/v1/groups?page=1&page_size=20`
- 鉴权：是

成功响应：

```json
{
  "code": 0,
  "message": "success",
  "data": {
    "items": [
      {
        "id": 10,
        "name": "产品讨论群",
        "description": "用于日常需求讨论",
        "owner_id": 1,
        "created_at": "2026-04-21T08:00:00+00:00"
      }
    ],
    "total": 1
  }
}
```

#### 5.4.3 群组详情

- 方法与路径：`GET /api/v1/groups/{group_id}`
- 鉴权：是

成功响应：

```json
{
  "code": 0,
  "message": "success",
  "data": {
    "id": 10,
    "name": "产品讨论群",
    "description": "用于日常需求讨论",
    "owner_id": 1,
    "created_at": "2026-04-21T08:00:00+00:00",
    "members": [
      {
        "id": 100,
        "group_id": 10,
        "user_id": 1,
        "role": "owner",
        "joined_at": "2026-04-21T08:00:00+00:00"
      }
    ]
  }
}
```

#### 5.4.4 添加群成员

- 方法与路径：`POST /api/v1/groups/{group_id}/members`
- 鉴权：是

请求体：

```json
{
  "user_id": 2,
  "role": "member"
}
```

成功响应（201）：

```json
{
  "code": 0,
  "message": "添加成员成功",
  "data": {
    "id": 101,
    "group_id": 10,
    "user_id": 2,
    "role": "member",
    "joined_at": "2026-04-21T08:10:00+00:00"
  }
}
```

#### 5.4.5 移除群成员

- 方法与路径：`DELETE /api/v1/groups/{group_id}/members/{user_id}`
- 鉴权：是

成功响应：

```json
{
  "code": 0,
  "message": "移除成员成功",
  "data": {
    "result": "ok"
  }
}
```

#### 5.4.6 群备忘录列表

- 方法与路径：`GET /api/v1/groups/{group_id}/memos?page=1&page_size=20`
- 鉴权：是

成功响应：

```json
{
  "code": 0,
  "message": "success",
  "data": {
    "items": [
      {
        "id": 200,
        "title": "会议纪要",
        "content": "今天确认了排期",
        "creator_id": 1,
        "group_id": 10,
        "created_at": "2026-04-21T09:00:00+00:00",
        "updated_at": "2026-04-21T09:00:00+00:00"
      }
    ],
    "total": 1
  }
}
```

---

### 5.5 备忘录模块

#### 5.5.1 创建备忘录

- 方法与路径：`POST /api/v1/memos`
- 鉴权：是

请求体：

```json
{
  "title": "今日待办",
  "content": "1. 联调接口",
  "group_id": null
}
```

字段说明：

- `title`：必填，1~200 字符
- `content`：可选
- `group_id`：可选；为空表示个人备忘录

成功响应（201）：

```json
{
  "code": 0,
  "message": "创建成功",
  "data": {
    "id": 200,
    "title": "今日待办",
    "content": "1. 联调接口",
    "creator_id": 1,
    "group_id": null,
    "created_at": "2026-04-21T09:00:00+00:00",
    "updated_at": "2026-04-21T09:00:00+00:00"
  }
}
```

#### 5.5.2 我的备忘录列表

- 方法与路径：`GET /api/v1/memos?page=1&page_size=20`
- 鉴权：是

成功响应：分页结构同上，`items` 为 `MemoResponse`。

#### 5.5.3 更新备忘录

- 方法与路径：`PATCH /api/v1/memos/{memo_id}`
- 鉴权：是

请求体（至少传一个字段）：

```json
{
  "title": "更新后的标题",
  "content": "更新后的内容"
}
```

成功响应：

```json
{
  "code": 0,
  "message": "更新成功",
  "data": {
    "id": 200,
    "title": "更新后的标题",
    "content": "更新后的内容",
    "creator_id": 1,
    "group_id": null,
    "created_at": "2026-04-21T09:00:00+00:00",
    "updated_at": "2026-04-21T09:30:00+00:00"
  }
}
```

#### 5.5.4 删除备忘录

- 方法与路径：`DELETE /api/v1/memos/{memo_id}`
- 鉴权：是

成功响应：

```json
{
  "code": 0,
  "message": "删除成功",
  "data": {
    "result": "ok"
  }
}
```

---

### 5.6 待办事项模块

#### 5.6.1 创建待办

- 方法与路径：`POST /api/v1/todos`
- 鉴权：是

请求体：

```json
{
  "title": "标题",
  "description": "",
  "due_at": null,
  "timezone": "Asia/Shanghai"
}
```

成功响应（201）：`data` 为单条 `TodoResponse`。

#### 5.6.2 我的待办列表

- 方法与路径：`GET /api/v1/todos?page=1&page_size=20`
- 鉴权：是

成功响应：分页结构，`items` 为 `TodoResponse` 数组。

#### 5.6.3 待办详情

- 方法与路径：`GET /api/v1/todos/{todo_id}`
- 鉴权：是

说明：仅创建者可查看；他人或非本人待办返回业务错误（403）；不存在返回 404。

成功响应：

```json
{
  "code": 0,
  "message": "success",
  "data": {
    "id": 1,
    "title": "标题",
    "description": "",
    "due_at": null,
    "timezone": "Asia/Shanghai",
    "status": "open",
    "creator_id": 10,
    "created_at": "2026-04-21T09:00:00+00:00",
    "updated_at": "2026-04-21T09:00:00+00:00"
  }
}
```

#### 5.6.4 更新待办

- 方法与路径：`PATCH /api/v1/todos/{todo_id}`
- 鉴权：是

请求体字段均可选：`title`、`description`、`due_at`、`timezone`、`status`（`open` | `done`）。

成功响应：`data` 为更新后的 `TodoResponse`。

#### 5.6.5 好友协作待办邀请

说明：发起方向已是好友的用户发送邀请；被邀请人在「待办」侧收到待处理项并可接受或拒绝。仅在接受后，双方各自生成一条正式待办（`POST /todos` 创建的普通待办）。大模型侧可通过工具 `list_friends_for_todo` / `invite_friend_to_todo` 写入邀请；客户端也可直接调用下列 REST。

- **创建邀请**：`POST /api/v1/todo-invitations`，鉴权：是。

请求体：

```json
{
  "invitee_user_id": 12,
  "title": "一起吃饭",
  "description": "",
  "due_text": "周五晚上七点",
  "timezone": "Asia/Shanghai"
}
```

成功响应（201）：`data` 为 `TodoInvitationItemResponse`（含 `counterpart` 为对方公开信息、`status` 为 `pending`）。

- **我收到的待处理邀请**：`GET /api/v1/todo-invitations/incoming?page=1&page_size=20`

- **我发出的邀请（待确认或已拒绝）**：`GET /api/v1/todo-invitations/outgoing?page=1&page_size=20`  
  （已接受并已为双方创建待办后不再列出，以免与普通待办重复。）

- **接受**：`POST /api/v1/todo-invitations/{id}/accept`，鉴权：是（仅被邀请人）。

- **拒绝**：`POST /api/v1/todo-invitations/{id}/reject`，鉴权：是（仅被邀请人）。

`TodoInvitationItemResponse` 字段：`id`、`counterpart`（`id` / `nickname` / `phone_masked` / `avatar_url`）、`title`、`description`、`due_at`、`timezone`、`status`（`pending` | `accepted` | `rejected`）、`created_at`。

---

### 5.7 聊天模块

#### 5.7.1 创建会话

- 方法与路径：`POST /api/v1/chats/sessions`
- 鉴权：是

请求体：

```json
{
  "title": "项目答疑",
  "type": "personal",
  "group_id": null
}
```

字段说明：

- `title`：必填，1~200 字符
- `type`：可选，`personal` 或 `group`，默认 `personal`
- `group_id`：可选，群会话时建议传入对应群组 ID

成功响应（201）：

```json
{
  "code": 0,
  "message": "创建会话成功",
  "data": {
    "id": 300,
    "title": "项目答疑",
    "owner_id": 1,
    "group_id": null,
    "type": "personal",
    "created_at": "2026-04-21T10:00:00+00:00"
  }
}
```

#### 5.7.2 查询会话消息列表

- 方法与路径：`GET /api/v1/chats/sessions/{session_id}/messages?page=1&page_size=20`
- 鉴权：是

成功响应：

```json
{
  "code": 0,
  "message": "success",
  "data": {
    "items": [
      {
        "id": 500,
        "session_id": 300,
        "role": "user",
        "content": "你好",
        "created_at": "2026-04-21T10:01:00+00:00"
      }
    ],
    "total": 1
  }
}
```

#### 5.7.3 发送消息（含 AI 回复）

- 方法与路径：`POST /api/v1/chats/sessions/{session_id}/messages`
- 鉴权：是

请求体：

```json
{
  "role": "user",
  "content": "帮我总结今天会议重点"
}
```

字段说明：

- `role`：可选，默认 `user`
- `content`：必填，最少 1 个字符

成功响应（201）：

```json
{
  "code": 0,
  "message": "发送成功",
  "data": {
    "user_message": {
      "id": 501,
      "session_id": 300,
      "role": "user",
      "content": "帮我总结今天会议重点",
      "created_at": "2026-04-21T10:02:00+00:00"
    },
    "assistant_message": {
      "id": 502,
      "session_id": 300,
      "role": "assistant",
      "content": "今天会议重点有三点：...",
      "created_at": "2026-04-21T10:02:01+00:00"
    }
  }
}
```

#### 5.7.4 发送消息（SSE 流式）

- 方法与路径：`POST /api/v1/chats/sessions/{session_id}/messages/stream`
- 鉴权：是
- 响应类型：`text/event-stream`

请求体（同 5.5.3）：

```json
{
  "role": "user",
  "content": "帮我总结今天会议重点"
}
```

SSE 事件格式（每条均为 `data: <json>\n\n`）：

1) 用户消息入库成功：

```json
{
  "type": "user_message",
  "data": {
    "id": 501,
    "session_id": 300
  }
}
```

2) 模型增量输出（多条）：

```json
{
  "type": "delta",
  "data": "今天会议重点有三点："
}
```

3) 流结束并返回 assistant 落库结果：

```json
{
  "type": "done",
  "data": {
    "id": 502,
    "session_id": 300,
    "content": "今天会议重点有三点：..."
  }
}
```

### 5.8 会议纪要模块

会议纪要走「上传音频 → 异步转写 → LLM 二次总结 → 结构化纪要落库」的链路。底层 ASR 默认接火山引擎「大模型录音文件识别」（参考 [火山文档](https://www.volcengine.com/docs/6561/1354871)），未配置凭据时回退本地 mock 转写。摘要复用既有 LLM Provider（受 `AI_PROVIDER` 控制）。

**任务状态机**：

```
pending → submitting → transcribing → summarizing → done
                            ↓               ↓
                          failed          failed
```

`failed` 时通过 `error_message` 携带原因。客户端可调用 `POST /meetings/{id}/refresh` 触发兜底续推。

#### 5.8.1 上传会议音频

- 方法与路径：`POST /api/v1/meetings/upload`
- 鉴权：是
- Content-Type：`multipart/form-data`

表单字段：

| 字段       | 类型    | 必填 | 说明                                                            |
| ---------- | ------- | ---- | --------------------------------------------------------------- |
| `audio`    | File    | 是   | 会议录音，支持 mp3/wav/m4a/aac/flac/ogg/opus，默认上限 200MB    |
| `title`    | string  | 否   | 会议标题，不传时使用上传文件名                                  |
| `group_id` | int     | 否   | 关联群组 ID（可空）                                             |

成功响应（201）：服务端立即创建 `Meeting(status=pending)` 并把后台任务入队，返回的 `data` 即为初始的 `MeetingResponse`，客户端应轮询 `GET /meetings/{id}` 直至 `status == "done"`。

```json
{
  "code": 0,
  "message": "上传成功，正在转写",
  "data": {
    "id": 12,
    "title": "Q3营销总结",
    "status": "pending",
    "creator_id": 1,
    "group_id": null,
    "audio_mime": "audio/mpeg",
    "audio_duration_ms": null,
    "audio_size_bytes": 4283190,
    "error_message": null,
    "transcript_text": "",
    "transcript_segments": [],
    "summary": "",
    "key_points": [],
    "attendees": [],
    "created_at": "2026-05-03T10:00:00+00:00",
    "updated_at": "2026-05-03T10:00:00+00:00"
  }
}
```

业务错误：

- 文件为空 / 大小超限 → `4000`，文案如「上传的音频内容为空」「音频文件过大，超过 200 MB 限制」

#### 5.8.2 我的会议列表

- 方法与路径：`GET /api/v1/meetings?page=1&page_size=20`
- 鉴权：是

`data.items` 形态为精简版 `MeetingListItem`（不含 `transcript_segments`、`key_points`、`attendees` 等大字段）：

```json
{
  "code": 0,
  "message": "success",
  "data": {
    "items": [
      {
        "id": 12,
        "title": "Q3营销总结",
        "status": "done",
        "creator_id": 1,
        "group_id": null,
        "audio_duration_ms": 5400000,
        "audio_size_bytes": 4283190,
        "summary": "复盘 Q3 数据并初步确定 Q4 大促策略...",
        "error_message": null,
        "created_at": "2026-05-03T10:00:00+00:00",
        "updated_at": "2026-05-03T10:05:30+00:00"
      }
    ],
    "total": 1
  }
}
```

#### 5.8.3 会议纪要详情

- 方法与路径：`GET /api/v1/meetings/{meeting_id}`
- 鉴权：是（仅创建者）

成功响应：

```json
{
  "code": 0,
  "message": "success",
  "data": {
    "id": 12,
    "title": "Q3营销总结",
    "status": "done",
    "creator_id": 1,
    "group_id": null,
    "audio_mime": "audio/mpeg",
    "audio_duration_ms": 5400000,
    "audio_size_bytes": 4283190,
    "error_message": null,
    "transcript_text": "大家早上好...",
    "transcript_segments": [
      {
        "start_ms": 2000,
        "end_ms": 8000,
        "time": "00:02",
        "speaker": "说话人 1",
        "text": "大家早上好，今天主要过一下 Q3 数据和 Q4 计划。"
      }
    ],
    "summary": "本次会议复盘了 Q3 数据并对 Q4 大促做了初步规划...",
    "key_points": [
      { "text": "Q3 整体营收超额完成 15%", "assign": null },
      { "text": "周五前定稿主视觉", "assign": "李四" }
    ],
    "attendees": ["说话人 1", "说话人 2", "说话人 3"],
    "created_at": "2026-05-03T10:00:00+00:00",
    "updated_at": "2026-05-03T10:05:30+00:00"
  }
}
```

业务错误：

- 不存在 → `4040`
- 非创建者访问 → `4030`

#### 5.8.4 更新会议（仅标题）

- 方法与路径：`PATCH /api/v1/meetings/{meeting_id}`
- 鉴权：是（仅创建者）

请求体：

```json
{ "title": "Q3 营销总结与 Q4 规划" }
```

成功响应：`data` 为更新后的 `MeetingResponse`。

#### 5.8.5 删除会议

- 方法与路径：`DELETE /api/v1/meetings/{meeting_id}`
- 鉴权：是（仅创建者）

服务端会同步删除磁盘上的音频文件。

```json
{ "code": 0, "message": "删除成功", "data": { "result": "ok" } }
```

#### 5.8.6 重新生成总结

- 方法与路径：`POST /api/v1/meetings/{meeting_id}/regenerate-summary`
- 鉴权：是（仅创建者）
- 状态码：`202 Accepted`（后台异步执行，仅当已有 `transcript_text` 时生效）

仅重新调用 LLM 生成 `summary` / `key_points`，不会再次调用 ASR。客户端应继续轮询 `GET /meetings/{id}`。

#### 5.8.7 刷新任务状态（兜底）

- 方法与路径：`POST /api/v1/meetings/{meeting_id}/refresh`
- 鉴权：是（仅创建者）
- 状态码：`202 Accepted`

适用于后台任务被中断（进程重启）、仍处于 `submitting`/`transcribing`/`summarizing` 的会议：服务端按现有 `asr_task_id` 主动续推。

#### 5.8.8 实时流式语音识别（WebSocket）

- 路径：`WS /api/v1/meetings/stream/ws?token=<access_token>`
- 鉴权：`token` 查询参数，取值与 `Authorization: Bearer <access_token>` 中的 token **相同**（无需写 `Bearer ` 前缀）

底层对接火山「大模型流式语音识别」WebSocket 二进制协议（参考 [大模型流式语音识别 API](https://www.volcengine.com/docs/6561/1354869)）。需在服务端配置：

- `ASR_PROVIDER=volcengine`
- `VOLCENGINE_ASR_APP_KEY` / `VOLCENGINE_ASR_ACCESS_KEY`
- `VOLCENGINE_ASR_STREAM_RESOURCE_ID`：流式资源 ID（默认 `volc.bigasr.sauc.duration`，与录音文件识别的 `volc.bigasr.auc` 不同）
- `VOLCENGINE_ASR_STREAM_PATH`：WebSocket 路径（默认 `/api/v3/sauc/bigmodel`；若使用流式输入优化版可改为文档中的 `bigmodel_nostream` 等）

**客户端消息顺序**

1. **首条文本 JSON（必填）**

```json
{
  "action": "start",
  "title": "实时会议标题",
  "save_meeting": true,
  "group_id": null,
  "audio": { "format": "pcm", "rate": 16000, "bits": 16, "channel": 1 }
}
```

- `save_meeting`：为 `true` 时，在识别结束后写入 `meetings` 表（`audio_path` 为 `stream:none`），并异步触发与上传录音相同的 LLM 摘要流水线。
- `audio.format`：需与二进制帧实际编码一致；`pcm` / `wav` 内线码为 **pcm_s16le**，采样率目前建议 **16000**（与火山文档一致）。

2. **多条二进制帧**：PCM s16le 切片（建议单包约 100～200ms）。

3. **结束文本 JSON**

```json
{ "action": "end" }
```

**服务端下行 JSON**

- 识别过程中：`{"type":"asr","text":"<当前累计全文>"}`（`result_type` 配置为 `single` 时为增量友好）
- 非火山或未配置密钥：`{"type":"info","message":"..."}`，结束时使用 mock 转写
- 结束：`{"type":"done","text":"...","meeting_id":123}`（若未落库则 `meeting_id` 为 `null`）
- 错误：`{"type":"error","message":"..."}`

## 6. 联调建议

- 先调登录接口拿到 token，再串行联调其他业务接口
- 列表接口统一处理 `data.items` 与 `data.total`
- 业务报错优先根据 `code` 分支处理，再显示 `message`
- 会议纪要：**整段录音**上传后通过 `GET /meetings/{id}` 轮询 `status` 直至 `done` / `failed`；轮询频率建议 2～5 秒
- 会议纪要：**流式**识别结束后若 `save_meeting=true`，同样用返回的 `meeting_id` 轮询详情直至摘要完成
