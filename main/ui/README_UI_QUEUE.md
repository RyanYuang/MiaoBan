# UI 命令队列（`UiCommandDispatcher`）

## 作用

凡是需要**跨任务保持先后顺序**的、面向**显示/界面**的修改，都应通过 `UiCommandDispatcher::Instance().Post(std::function<void()>&& fn)` 投递。专用 FreeRTOS 任务 `ui_cmd` 按 **FIFO** 依次取出并执行每个任务。

LVGL 仍由 `esp_lvgl_port` 驱动；每个 job 里通常会调用带 `DisplayLockGuard` 的 `Display` / `LvglDisplay` 接口。队列解决的是**多任务之间**谁先谁后的问题，**不能**替代 `lvgl_port_lock`。

## API

- `UiCommandDispatcher::Instance().Start()` — 在 `Application::Initialize()` 里尽早调用一次（板级与 `Display` 已就绪之后、大量 UI 生产者之前）。
- `UiCommandDispatcher::Instance().Post(...)` — **任意任务**可调用；仅入队并唤醒 worker，**立即返回**。

若在 **`ui_cmd` 任务自身**里再次调用 `Post`，会在当前上下文**同步执行**该可调用对象（可重入、避免与队列互斥锁死锁）。

## 配置（`menuconfig` → Xiaozhi Assistant → UI command dispatcher）

| 符号 | 含义 |
|------|------|
| `CONFIG_UI_CMD_QUEUE_MAX_DEPTH` | 队列最大深度；满时**丢弃最旧**任务并打日志。 |
| `CONFIG_UI_CMD_TASK_STACK_SIZE` | `ui_cmd` 任务栈大小（字节）。 |
| `CONFIG_UI_CMD_TASK_PRIORITY` | `ui_cmd` 优先级（需与音频、LVGL 端口任务等权衡）。 |

## 何时用 `Post`，何时用 `Application::Schedule`

| 使用方式 | 适用场景 |
|----------|----------|
| **`UiCommandDispatcher::Post`** | 仅（或主要）操作 `Display*` / 依赖 LVGL 的界面逻辑；协议 JSON 中 `Alert` 等需先把字符串拷贝到 `std::string` 再投递，避免悬空指针。 |
| **`Application::Schedule`** | 必须在 **应用主循环**（`Run`）里跑的逻辑：状态机、`SetDeviceState`、协议/音频控制、`Reboot` 等。 |

**禁止**在某个 UI job 里阻塞等待另一个同样依赖该 worker 的 UI job（会死锁）。

## 板级与回调代码

在 GPIO、WiFi、定时器等回调里，对 `GetDisplay()->ShowNotification`、`SetStatus` 等，优先 **`Post`**，避免在任意任务里直接调 `Display` 造成顺序不确定。

其它板卡可参考 `boards/bread-compact-wifi/compact_wifi_board.cc` 与 `boards/common/wifi_board.cc` 的模式逐步迁移。
