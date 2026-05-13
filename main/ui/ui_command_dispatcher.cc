#include "ui_command_dispatcher.h"

#include <sdkconfig.h>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#ifndef CONFIG_UI_CMD_QUEUE_MAX_DEPTH
#define CONFIG_UI_CMD_QUEUE_MAX_DEPTH 32
#endif
#ifndef CONFIG_UI_CMD_TASK_STACK_SIZE
#define CONFIG_UI_CMD_TASK_STACK_SIZE 4096
#endif
#ifndef CONFIG_UI_CMD_TASK_PRIORITY
#define CONFIG_UI_CMD_TASK_PRIORITY 5
#endif

namespace {
constexpr char TAG[] = "UiCmd";
}

UiCommandDispatcher& UiCommandDispatcher::Instance() {
    static UiCommandDispatcher instance;
    return instance;
}

void UiCommandDispatcher::WorkerEntry(void* arg) {
    static_cast<UiCommandDispatcher*>(arg)->WorkerLoop();
}

void UiCommandDispatcher::WorkerLoop() {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        DrainPending();
    }
}

void UiCommandDispatcher::DrainPending() {
    for (;;) {
        std::function<void()> job;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.empty()) {
                return;
            }
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        if (job) {
            job();
        }
    }
}

void UiCommandDispatcher::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (worker_handle_ != nullptr) {
        return;
    }
    const uint32_t stack = static_cast<uint32_t>(CONFIG_UI_CMD_TASK_STACK_SIZE);
    const UBaseType_t prio = static_cast<UBaseType_t>(CONFIG_UI_CMD_TASK_PRIORITY);
    BaseType_t ok = xTaskCreate(WorkerEntry, "ui_cmd", stack, this, prio, &worker_handle_);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create ui_cmd task");
        worker_handle_ = nullptr;
        return;
    }
    if (!queue_.empty()) {
        xTaskNotifyGive(worker_handle_);
    }
}

/**
 * 投递一条「改界面」相关的工作到 ui_cmd 任务上串行执行。
 *
 * 使用方式：
 * 1. 进程生命周期内先调用一次 Start()（通常在 Application::Initialize() 里，Display 就绪之后）。
 * 2. 任意任务（WiFi、协议回调、定时器等）需要安全更新 Display / LVGL 时：
 *    UiCommandDispatcher::Instance().Post([display = Board::GetInstance().GetDisplay()]() {
 *        if (display) {
 *            display->SetChatMessage("system", "hello");  // 内部仍会 DisplayLockGuard
 *        }
 *    });
 * 3. Lambda 内用到的指针、字符串等：若来自其它线程或短命对象，务必按值捕获（拷贝），避免执行时悬空。
 *
 * 注意：
 * - 不要在某个 Post 的 job 里同步阻塞等待「另一个仍由本队列执行的 Post」完成，否则可能死锁。
 * - 若当前就在 ui_cmd 任务里再次 Post，本函数会直接同步执行 fn()（见下方重入分支），用于链式子任务。
 */
void UiCommandDispatcher::Post(std::function<void()>&& fn) {
    // 先只读取出 worker 句柄，用于判断是否「在 ui_cmd 任务内重入 Post」。
    TaskHandle_t worker_copy = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        worker_copy = worker_handle_;
    }
    // 重入：当前任务就是 ui_cmd 时不能再入队后等自己（会与 mutex_ 死锁），改为直接执行。
    if (worker_copy != nullptr && xTaskGetCurrentTaskHandle() == worker_copy) {
        if (fn) {
            fn();
        }
        return;
    }

    // 普通路径：入 FIFO；满则丢最旧；最后唤醒 ui_cmd 一次（可在 DrainPending 里连续出队多条）。
    TaskHandle_t notify = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const size_t max_depth = static_cast<size_t>(CONFIG_UI_CMD_QUEUE_MAX_DEPTH);
        while (queue_.size() >= max_depth) {
            ESP_LOGW(TAG, "Queue full (%u), dropping oldest", static_cast<unsigned>(max_depth));
            queue_.pop_front();
        }
        queue_.push_back(std::move(fn));
        notify = worker_handle_;
    }
    if (notify != nullptr) {
        xTaskNotifyGive(notify);
    }
}
