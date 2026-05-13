#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <deque>
#include <functional>
#include <mutex>

/**
 * Serializes UI-facing work on a dedicated FreeRTOS task (FIFO queue).
 * Use Post() from any task; do not synchronously wait inside the worker for another Post.
 */
class UiCommandDispatcher {
public:
    static UiCommandDispatcher& Instance();

    void Start();
    void Post(std::function<void()>&& fn);

    UiCommandDispatcher(const UiCommandDispatcher&) = delete;
    UiCommandDispatcher& operator=(const UiCommandDispatcher&) = delete;

private:
    UiCommandDispatcher() = default;

    static void WorkerEntry(void* arg);
    void WorkerLoop();
    void DrainPending();

    std::mutex mutex_;
    std::deque<std::function<void()>> queue_;
    TaskHandle_t worker_handle_ = nullptr;
};
