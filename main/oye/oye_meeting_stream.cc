#include "oye_meeting_stream.h"

#include "oye_config.h"

#include <board.h>
#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include <web_socket.h>

#include <cstring>
#include <memory>

namespace oye {

static const char* TAG = "OyeMeetingWs";

namespace {

constexpr size_t kLogTextMax = 160;
constexpr size_t kMinLargestInternalForWs = 4096;
constexpr int kWsConnectAttempts = 3;
constexpr int kWsRetryDelayMs = 400;
// Stack in SPIRAM (see StartSendTask); WebSocket::Send needs headroom beyond frame buffer.
constexpr uint32_t kSendTaskStackBytes = 6144 * sizeof(StackType_t);
constexpr uint32_t kSendTaskStackBytesInternalFallback = 2560 * sizeof(StackType_t);
constexpr UBaseType_t kSendTaskPriority = 5;
constexpr TickType_t kSendTaskStopWaitTicks = pdMS_TO_TICKS(7000);
constexpr int kPcmSendPaceMs = 20;
constexpr int kPcmSendRetryDelayMs = 20;
constexpr int kPcmSendRetryMaxAttempts = 250;

void LogTextPreview(const char* label, const std::string& text) {
    if (text.empty()) {
        ESP_LOGI(TAG, "%s: (empty)", label);
        return;
    }
    if (text.size() <= kLogTextMax) {
        ESP_LOGI(TAG, "%s (%u chars): %s", label, static_cast<unsigned>(text.size()), text.c_str());
        return;
    }
    std::string preview(text.data(), kLogTextMax);
    ESP_LOGI(TAG, "%s (%u chars): %s…", label, static_cast<unsigned>(text.size()), preview.c_str());
}

void LogHeap(const char* label) {
    ESP_LOGI(TAG, "%s: free_internal=%u largest_internal=%u", label,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
}

bool HeapOkForWs() {
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >= kMinLargestInternalForWs;
}

}  // namespace

// 构造函数，创建事件组
MeetingStream::MeetingStream() {
    done_event_ = xEventGroupCreate();
}

// 析构函数，停止对话流并删除事件组
MeetingStream::~MeetingStream() {
    Stop(false);
    if (done_event_ != nullptr) {
        vEventGroupDelete(done_event_);
        done_event_ = nullptr;
    }
}

// 进入传输增强模式，提高性能
void MeetingStream::EnterTransportBoost() {
    if (!transport_boost_) {
        LogHeap("WS transport boost begin");
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        transport_boost_ = true;
    }
}

// 离开传输增强模式，降低功耗
void MeetingStream::LeaveTransportBoost() {
    if (transport_boost_) {
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        transport_boost_ = false;
        LogHeap("WS transport boost end");
    }
}

// 建立 WebSocket 连接 URL
std::string MeetingStream::BuildWsUrl() {
    return BuildWebSocketUrl("/meetings/stream/ws");
}

// 初始化 PCM 队列
bool MeetingStream::InitPcmQueue() {
    DestroyPcmQueue();
    pcm_queue_storage_ = static_cast<uint8_t*>(
        heap_caps_malloc(kPcmFrameBytes * kPcmQueueDepth, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (pcm_queue_storage_ == nullptr) {
        pcm_queue_storage_ =
            static_cast<uint8_t*>(heap_caps_malloc(kPcmFrameBytes * kPcmQueueDepth, MALLOC_CAP_INTERNAL));
    }
    if (pcm_queue_storage_ == nullptr) {
        ESP_LOGE(TAG, "InitPcmQueue: storage alloc failed");
        return false;
    }
    pcm_queue_ = xQueueCreateStatic(kPcmQueueDepth, kPcmFrameBytes, pcm_queue_storage_, &pcm_queue_buffer_);
    if (pcm_queue_ == nullptr) {
        ESP_LOGE(TAG, "InitPcmQueue: xQueueCreateStatic failed");
        heap_caps_free(pcm_queue_storage_);
        pcm_queue_storage_ = nullptr;
        return false;
    }
    return true;
}

// 销毁 PCM 队列
void MeetingStream::DestroyPcmQueue() {
    if (pcm_queue_ != nullptr) {
        vQueueDelete(pcm_queue_);
        pcm_queue_ = nullptr;
    }
    if (pcm_queue_storage_ != nullptr) {
        heap_caps_free(pcm_queue_storage_);
        pcm_queue_storage_ = nullptr;
    }
}

// 发送任务入口
void MeetingStream::SendTaskEntry(void* arg) {
    static_cast<MeetingStream*>(arg)->PcmSendTask();
}

// 启动发送任务
bool MeetingStream::StartSendTask() {
    send_task_run_ = true;
    send_task_handle_ = nullptr;
    xEventGroupClearBits(done_event_, kSendTaskExitBit);

    if (xTaskCreateWithCaps(SendTaskEntry, "oye_ws_pcm", kSendTaskStackBytes, this, kSendTaskPriority,
                            &send_task_handle_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS) {
        ESP_LOGI(TAG, "PCM send task created (SPIRAM stack %u bytes)",
                 static_cast<unsigned>(kSendTaskStackBytes));
        return true;
    }

    LogHeap("StartSendTask fallback");
    if (xTaskCreateWithCaps(SendTaskEntry, "oye_ws_pcm", kSendTaskStackBytesInternalFallback, this,
                            kSendTaskPriority, &send_task_handle_,
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        send_task_run_ = false;
        send_task_handle_ = nullptr;
        ESP_LOGE(TAG, "StartSendTask: xTaskCreate failed (need largest>=%u)",
                 static_cast<unsigned>(kSendTaskStackBytesInternalFallback));
        return false;
    }
    ESP_LOGI(TAG, "PCM send task created (internal stack %u bytes)",
             static_cast<unsigned>(kSendTaskStackBytesInternalFallback));
    return true;
}

// 停止发送任务
void MeetingStream::StopSendTask() {
    send_task_run_ = false;
    if (send_task_handle_ == nullptr) {
        return;
    }
    const EventBits_t bits = xEventGroupWaitBits(done_event_, kSendTaskExitBit, pdTRUE, pdTRUE, kSendTaskStopWaitTicks);
    if ((bits & kSendTaskExitBit) == 0) {
        ESP_LOGW(TAG, "StopSendTask: send task exit timeout after %u ms",
                 static_cast<unsigned>(kSendTaskStopWaitTicks * portTICK_PERIOD_MS));
        return;
    }
    send_task_handle_ = nullptr;
}

// 发送任务主循环
void MeetingStream::PcmSendTask() {
    ESP_LOGI(TAG, "PCM send task started (stack=%u bytes)", static_cast<unsigned>(kSendTaskStackBytes));
    // 独立发送任务：在后端 `ready` 前仅本地缓存 20ms PCM 帧，收到 `ready` 后再按实时节奏上行。
    while (send_task_run_) {
        if (!running_ || websocket_ == nullptr) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (!stream_ready_) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        const TickType_t now = xTaskGetTickCount();
        if (now < next_pcm_send_tick_) {
            vTaskDelay(next_pcm_send_tick_ - now);
            continue;
        }
        if (xQueueReceive(pcm_queue_, send_frame_, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }

        bool sent = false;
        for (int attempt = 0; send_task_run_ && stream_ready_ && running_; ++attempt) {
            if (SendPcmChunk(send_frame_, kPcmFrameSamples)) {
                sent = true;
                break;
            }
            if (attempt + 1 >= kPcmSendRetryMaxAttempts) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(kPcmSendRetryDelayMs));
        }

        if (sent) {
            next_pcm_send_tick_ = xTaskGetTickCount() + pdMS_TO_TICKS(kPcmSendPaceMs);
        } else {
            ESP_LOGE(TAG, "PCM send gave up after retries, halt meeting uplink");
            stream_ready_ = false;
            if (!end_requested_ && !session_finished_) {
                interrupted_ = true;
            }
            running_ = false;
            send_task_run_ = false;
            SignalSessionEnd();
        }
    }
    while (xQueueReceive(pcm_queue_, send_frame_, 0) == pdTRUE) {
    }
    ESP_LOGI(TAG, "PCM send task exit (feeds=%u drops=%u)", static_cast<unsigned>(pcm_feed_count_),
             static_cast<unsigned>(pcm_queue_drops_));
    send_task_handle_ = nullptr;
    xEventGroupSetBits(done_event_, kSendTaskExitBit);
    vTaskDeleteWithCaps(nullptr);
}

// 启动对话流
bool MeetingStream::Start(const std::string& title, bool save_meeting, TextCallback on_asr,
                          DoneCallback on_done) {
    ESP_LOGI(TAG, "Start title=%s save_meeting=%d", title.c_str(), save_meeting ? 1 : 0);
    // 停止对话流
    Stop(false);
    // 检查是否有后端的访问令牌
    if (!HasAccessToken()) {
        ESP_LOGE(TAG, "Start: no access token");
        return false;
    }

    // 设置回调函数
    on_asr_ = std::move(on_asr);
    on_done_ = std::move(on_done);
    // 设置保存会议标志
    save_meeting_ = save_meeting;
    // 重置计数器
    pcm_feed_count_ = 0;
    pcm_bytes_sent_ = 0;
    pcm_queue_drops_ = 0;
    pcm_accum_.clear();
    pcm_accum_.reserve(kPcmAccumMaxSamples);
    stream_ready_ = false;
    next_pcm_send_tick_ = 0;
    session_finished_ = false;
    interrupted_ = false;
    end_requested_ = false;
    // 清除事件组
    if (done_event_ != nullptr) {
        xEventGroupClearBits(done_event_, kDoneReceivedBit | kSendTaskExitBit | kStreamReadyBit);
    }

    // 检查堆内存是否足够
    if (!HeapOkForWs()) {
        LogHeap("Start: reject WS (low internal heap)");
        return false;
    }

    // 初始化 PCM 队列
    if (!InitPcmQueue()) {
        return false;
    }

    // 进入传输增强模式
    EnterTransportBoost();
    // 延迟80ms
    vTaskDelay(pdMS_TO_TICKS(80));

    // 获取网络实例
    auto network = Board::GetInstance().GetNetwork();
    // 创建 WebSocket 实例
    std::unique_ptr<WebSocket> ws = network->CreateWebSocket(2);
    if (ws == nullptr) {
        ESP_LOGE(TAG, "Start: CreateWebSocket failed");
        DestroyPcmQueue();
        LeaveTransportBoost();
        return false;
    }

    // 构建 WebSocket URL
    const std::string url = BuildWsUrl();
    ESP_LOGI(TAG, "WS connect (token redacted) path=%s/meetings/stream/ws", kApiPrefix);

    // 设置数据回调
    ws->OnData([this](const char* data, size_t len, bool binary) {
        if (!binary && data != nullptr && len > 0) {
            HandleText(data, len);
        }
    });
    ws->OnDisconnected([this]() {
        ESP_LOGW(TAG, "WS disconnected (feeds=%u bytes=%u drops=%u)",
                 static_cast<unsigned>(pcm_feed_count_), static_cast<unsigned>(pcm_bytes_sent_),
                 static_cast<unsigned>(pcm_queue_drops_));
        if (!end_requested_ && !session_finished_) {
            interrupted_ = true;
        }
        stream_ready_ = false;
        running_ = false;
        send_task_run_ = false;
        SignalSessionEnd();
    });

    bool connected = false;
    for (int attempt = 1; attempt <= kWsConnectAttempts; ++attempt) {
        if (!HeapOkForWs()) {
            LogHeap("WS connect skipped (heap)");
            break;
        }
        ESP_LOGI(TAG, "WS connect attempt %d/%d", attempt, kWsConnectAttempts);
        if (ws->Connect(url.c_str())) {
            connected = true;
            break;
        }
        if (attempt < kWsConnectAttempts) {
            vTaskDelay(pdMS_TO_TICKS(kWsRetryDelayMs));
        }
    }
    if (!connected) {
        ESP_LOGE(TAG, "WS connect failed after %d attempts", kWsConnectAttempts);
        DestroyPcmQueue();
        LeaveTransportBoost();
        return false;
    }
    ESP_LOGI(TAG, "WS connected");

    websocket_ = ws.release();

    cJSON* start = cJSON_CreateObject();
    cJSON_AddStringToObject(start, "action", "start");
    cJSON_AddStringToObject(start, "title", title.c_str());
    cJSON_AddBoolToObject(start, "save_meeting", save_meeting);
    cJSON_AddNullToObject(start, "group_id");
    cJSON* audio = cJSON_CreateObject();
    cJSON_AddStringToObject(audio, "format", "pcm");
    cJSON_AddNumberToObject(audio, "rate", 16000);
    cJSON_AddNumberToObject(audio, "bits", 16);
    cJSON_AddNumberToObject(audio, "channel", 1);
    cJSON_AddItemToObject(start, "audio", audio);
    char* printed = cJSON_PrintUnformatted(start);
    std::string msg = printed ? printed : "";
    cJSON_free(printed);
    cJSON_Delete(start);

    auto* socket = static_cast<WebSocket*>(websocket_);
    if (!socket->Send(msg)) {
        ESP_LOGE(TAG, "send start JSON failed");
        Stop(false);
        return false;
    }
    ESP_LOGI(TAG, "sent start JSON: pcm 16kHz mono s16le save_meeting=%d", save_meeting ? 1 : 0);

    if (!StartSendTask()) {
        Stop(false);
        return false;
    }

    running_ = true;
    stream_ready_ = false;
    ESP_LOGI(TAG, "waiting for backend ready before PCM uplink");
    return true;
}

// 发送 PCM 数据块
bool MeetingStream::SendPcmChunk(const int16_t* samples, size_t count) {
    if (websocket_ == nullptr || samples == nullptr || count == 0) {
        return false;
    }
    auto* socket = static_cast<WebSocket*>(websocket_);
    if (!socket->IsConnected()) {
        if (!end_requested_ && !session_finished_) {
            interrupted_ = true;
        }
        stream_ready_ = false;
        return false;
    }
    const size_t bytes = count * sizeof(int16_t);
    if (!socket->Send(reinterpret_cast<const char*>(samples), bytes, true)) {
        if (!socket->IsConnected()) {
            if (!end_requested_ && !session_finished_) {
                interrupted_ = true;
            }
            stream_ready_ = false;
            running_ = false;
            ESP_LOGW(TAG, "WS send failed, connection lost (feeds=%u bytes=%u err=%d)",
                     static_cast<unsigned>(pcm_feed_count_), static_cast<unsigned>(pcm_bytes_sent_),
                     socket->GetLastError());
        } else {
            ESP_LOGD(TAG, "binary PCM send blocked (%u bytes) err=%d, will retry",
                     static_cast<unsigned>(bytes), socket->GetLastError());
        }
        return false;
    }
    ++pcm_feed_count_;
    pcm_bytes_sent_ += bytes;
    if (pcm_feed_count_ == 1) {
        ESP_LOGI(TAG, "first binary PCM sent: %u bytes", static_cast<unsigned>(bytes));
    } else if ((pcm_feed_count_ % 50) == 0) {
        ESP_LOGI(TAG, "PCM uplink #%u (%u bytes total, drops=%u)", static_cast<unsigned>(pcm_feed_count_),
                 static_cast<unsigned>(pcm_bytes_sent_), static_cast<unsigned>(pcm_queue_drops_));
    }
    return true;
}

// 刷新 PCM 累积队列
void MeetingStream::FlushPcmAccumToQueue() {
    if (pcm_queue_ == nullptr) {
        return;
    }
    while (pcm_accum_.size() >= kPcmFrameSamples) {
        if (xQueueSend(pcm_queue_, pcm_accum_.data(), 0) != pdTRUE) {
            ++pcm_queue_drops_;
            if ((pcm_queue_drops_ % 20) == 1) {
                ESP_LOGW(TAG, "PCM queue full, dropped frame (drops=%u)", static_cast<unsigned>(pcm_queue_drops_));
            }
        }
        pcm_accum_.erase(pcm_accum_.begin(), pcm_accum_.begin() + kPcmFrameSamples);
    }
}

// 喂入 PCM 数据
bool MeetingStream::WaitForUpstreamReady(TickType_t ticks) {
    if (stream_ready_) {
        return true;
    }
    if (done_event_ == nullptr || !running_) {
        return false;
    }
    const EventBits_t bits =
        xEventGroupWaitBits(done_event_, kStreamReadyBit, pdFALSE, pdTRUE, ticks);
    return (bits & kStreamReadyBit) != 0 && stream_ready_ && running_;
}

void MeetingStream::FeedPcm(const int16_t* samples, size_t count) {
    if (!running_ || !stream_ready_ || pcm_queue_ == nullptr || samples == nullptr || count == 0) {
        return;
    }
    pcm_accum_.insert(pcm_accum_.end(), samples, samples + count);
    FlushPcmAccumToQueue();
}

// 信号会话结束
void MeetingStream::SignalSessionEnd() {
    if (session_finished_) {
        return;
    }
    session_finished_ = true;
    if (done_event_ != nullptr) {
        xEventGroupSetBits(done_event_, kDoneReceivedBit);
    }
}

// 关闭 WebSocket 连接
void MeetingStream::CloseWebSocket() {
    if (websocket_ != nullptr) {
        delete static_cast<WebSocket*>(websocket_);
        websocket_ = nullptr;
    }
}

// 停止对话流
void MeetingStream::Stop(bool wait_for_done, bool send_end) {
    if (!running_ && websocket_ == nullptr && !transport_boost_ && send_task_handle_ == nullptr) {
        return;
    }

    if (send_end) {
        end_requested_ = true;
    }
    stream_ready_ = false;
    running_ = false;
    StopSendTask();
    DestroyPcmQueue();

    if (websocket_ != nullptr) {
        ESP_LOGI(TAG, "Stop: closing stream (feeds=%u bytes=%u drops=%u wait_done=%d send_end=%d)",
                 static_cast<unsigned>(pcm_feed_count_), static_cast<unsigned>(pcm_bytes_sent_),
                 static_cast<unsigned>(pcm_queue_drops_), wait_for_done ? 1 : 0, send_end ? 1 : 0);
        auto* socket = static_cast<WebSocket*>(websocket_);
        if (send_end) {
            if (!socket->Send(R"({"action":"end"})")) {
                ESP_LOGW(TAG, "send end JSON failed, closing WS");
                wait_for_done = false;
            }
        } else {
            wait_for_done = false;
        }

        if (wait_for_done && !session_finished_ && done_event_ != nullptr) {
            ESP_LOGI(TAG, "Stop: waiting for type=done (max %u ms)",
                     static_cast<unsigned>(kDoneWaitTicks * portTICK_PERIOD_MS));
            const EventBits_t bits =
                xEventGroupWaitBits(done_event_, kDoneReceivedBit, pdFALSE, pdTRUE, kDoneWaitTicks);
            if ((bits & kDoneReceivedBit) == 0) {
                ESP_LOGW(TAG, "Stop: done timeout, closing WS anyway");
            }
        }
    }

    CloseWebSocket();
    LogTextPreview("Stop: latest_text", latest_text_);
    LeaveTransportBoost();
}

// 获取最新文本
std::string MeetingStream::LatestText() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_text_;
}

// 处理文本消息
void MeetingStream::HandleText(const char* data, size_t len) {
    std::string payload(data, len);
    cJSON* root = cJSON_Parse(payload.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "HandleText: invalid JSON len=%u", static_cast<unsigned>(len));
        return;
    }
    auto type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type)) {
        ESP_LOGW(TAG, "HandleText: missing type");
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type->valuestring, "asr") == 0) {
        auto text = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(text)) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                latest_text_ = text->valuestring;
            }
            LogTextPreview("WS asr", text->valuestring);
            if (on_asr_) {
                on_asr_(text->valuestring);
            }
        }
    } else if (strcmp(type->valuestring, "done") == 0) {
        std::string final_text;
        int meeting_id = 0;
        auto text = cJSON_GetObjectItem(root, "text");
        auto mid = cJSON_GetObjectItem(root, "meeting_id");
        if (cJSON_IsString(text)) {
            final_text = text->valuestring;
        }
        if (cJSON_IsNumber(mid)) {
            meeting_id = mid->valueint;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!final_text.empty()) {
                latest_text_ = final_text;
            }
        }
        LogTextPreview("WS done", latest_text_);
        ESP_LOGI(TAG, "WS done meeting_id=%d", meeting_id);
        if (on_done_) {
            on_done_(latest_text_, meeting_id);
        }
        SignalSessionEnd();
    } else if (strcmp(type->valuestring, "ready") == 0) {
        stream_ready_ = true;
        next_pcm_send_tick_ = xTaskGetTickCount();
        if (done_event_ != nullptr) {
            xEventGroupSetBits(done_event_, kStreamReadyBit);
        }
        auto msg = cJSON_GetObjectItem(root, "message");
        ESP_LOGI(TAG, "WS ready: backend upstream prepared, start PCM uplink (%s)",
                 cJSON_IsString(msg) ? msg->valuestring : "ok");
    } else if (strcmp(type->valuestring, "info") == 0) {
        auto msg = cJSON_GetObjectItem(root, "message");
        ESP_LOGI(TAG, "WS info: %s", cJSON_IsString(msg) ? msg->valuestring : "(no message)");
    } else if (strcmp(type->valuestring, "error") == 0) {
        auto msg = cJSON_GetObjectItem(root, "message");
        ESP_LOGE(TAG, "WS error: %s", cJSON_IsString(msg) ? msg->valuestring : "unknown");
        SignalSessionEnd();
    } else {
        ESP_LOGD(TAG, "WS type=%s len=%u", type->valuestring, static_cast<unsigned>(len));
    }
    cJSON_Delete(root);
}

}  // namespace oye
