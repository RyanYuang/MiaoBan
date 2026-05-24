#include "oye_voice_chat_stream.h"

#include "oye_config.h"
#include "oye_tts_stream.h"

#include <board.h>
#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/base64.h>
#include <web_socket.h>

#include <cstring>
#include <memory>
#include <vector>

namespace oye {

static const char* TAG = "OyeVoiceWs";

namespace {

constexpr size_t kLogTextMax = 160;
constexpr size_t kMinLargestInternalForWs = 4096;
constexpr int kWsConnectAttempts = 3;
constexpr int kWsRetryDelayMs = 400;
constexpr uint32_t kSendTaskStackBytes = 6144;
constexpr uint32_t kSendTaskStackBytesInternalFallback = 2560;
constexpr UBaseType_t kSendTaskPriority = 5;
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

bool DecodeBase64(const char* in, size_t in_len, std::vector<uint8_t>& out) {
    out.clear();
    if (in == nullptr || in_len == 0) {
        return true;
    }
    size_t olen = 0;
    const int rc = mbedtls_base64_decode(nullptr, 0, &olen, reinterpret_cast<const unsigned char*>(in), in_len);
    if (rc != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL && rc != 0) {
        return false;
    }
    out.resize(olen);
    if (mbedtls_base64_decode(out.data(), out.size(), &olen, reinterpret_cast<const unsigned char*>(in),
                              in_len) != 0) {
        out.clear();
        return false;
    }
    out.resize(olen);
    return true;
}

}  // namespace

VoiceChatStream::VoiceChatStream() {
    done_event_ = xEventGroupCreate();
}

VoiceChatStream::~VoiceChatStream() {
    Stop(false);
    if (done_event_ != nullptr) {
        vEventGroupDelete(done_event_);
        done_event_ = nullptr;
    }
}

void VoiceChatStream::EnterTransportBoost() {
    if (!transport_boost_) {
        LogHeap("WS transport boost begin");
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        transport_boost_ = true;
    }
}

void VoiceChatStream::LeaveTransportBoost() {
    if (transport_boost_) {
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        transport_boost_ = false;
        LogHeap("WS transport boost end");
    }
}

bool VoiceChatStream::InitPcmQueue() {
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
        heap_caps_free(pcm_queue_storage_);
        pcm_queue_storage_ = nullptr;
        return false;
    }
    return true;
}

void VoiceChatStream::DestroyPcmQueue() {
    if (pcm_queue_ != nullptr) {
        vQueueDelete(pcm_queue_);
        pcm_queue_ = nullptr;
    }
    if (pcm_queue_storage_ != nullptr) {
        heap_caps_free(pcm_queue_storage_);
        pcm_queue_storage_ = nullptr;
    }
}

void VoiceChatStream::SendTaskEntry(void* arg) {
    static_cast<VoiceChatStream*>(arg)->PcmSendTask();
}

bool VoiceChatStream::StartSendTask() {
    send_task_run_ = true;
    send_task_handle_ = nullptr;
    xEventGroupClearBits(done_event_, kSendTaskExitBit);

    if (xTaskCreateWithCaps(SendTaskEntry, "oye_vc_pcm", kSendTaskStackBytes, this, kSendTaskPriority,
                            &send_task_handle_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS) {
        ESP_LOGI(TAG, "PCM send task created (SPIRAM stack %u bytes)",
                 static_cast<unsigned>(kSendTaskStackBytes));
        return true;
    }

    LogHeap("StartSendTask fallback");
    if (xTaskCreateWithCaps(SendTaskEntry, "oye_vc_pcm", kSendTaskStackBytesInternalFallback, this,
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

void VoiceChatStream::StopSendTask() {
    send_task_run_ = false;
    if (send_task_handle_ == nullptr) {
        return;
    }
    xEventGroupWaitBits(done_event_, kSendTaskExitBit, pdTRUE, pdTRUE, pdMS_TO_TICKS(3000));
    send_task_handle_ = nullptr;
}

void VoiceChatStream::PcmSendTask() {
    // 独立发送任务：从 pcm_queue_ 取 20ms 定长帧并经 WebSocket 二进制上行。
    // FeedPcm 只负责拼帧入队，避免在音频回调里执行 Send 阻塞采集链路。
    while (send_task_run_) {
        // 100ms 超时：队列空时也能周期性检查 send_task_run_，Stop 时可及时退出。
        if (xQueueReceive(pcm_queue_, send_frame_, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        if (!stream_ready_ || !running_ || websocket_ == nullptr) {
            continue;
        }

        pcm_accept_feed_ = false;
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
            pcm_accept_feed_ = true;
        } else {
            ESP_LOGE(TAG, "PCM send gave up after retries, halt uplink");
            stream_ready_ = false;
            pcm_accept_feed_ = false;
        }
    }
    // Stop 时不再 drain 发送，避免在已阻塞的 TCP 上继续 Send。
    while (xQueueReceive(pcm_queue_, send_frame_, 0) == pdTRUE) {
    }
    send_task_handle_ = nullptr;
    xEventGroupSetBits(done_event_, kSendTaskExitBit);
    vTaskDeleteWithCaps(nullptr);
}

bool VoiceChatStream::Start(int chat_session_id, TextCallback on_asr, TextCallback on_asr_final,
                            TextCallback on_llm_text, std::function<void(const char* state)> on_tts_state,
                            DoneCallback on_done, ErrorCallback on_error) {
    ESP_LOGI(TAG, "Start chat_session_id=%d", chat_session_id);
    Stop(false);
    if (!HasAccessToken()) {
        ESP_LOGE(TAG, "Start: no access token");
        return false;
    }

    on_asr_ = std::move(on_asr);
    on_asr_final_ = std::move(on_asr_final);
    on_llm_text_ = std::move(on_llm_text);
    on_tts_state_ = std::move(on_tts_state);
    on_done_ = std::move(on_done);
    on_error_ = std::move(on_error);
    pcm_feed_count_ = 0;
    pcm_bytes_sent_ = 0;
    pcm_queue_drops_ = 0;
    pcm_accum_.clear();
    pcm_accum_.reserve(kPcmAccumMaxSamples);
    next_tts_seq_ = 0;
    user_text_.clear();
    assistant_text_.clear();
    stream_ready_ = false;
    pcm_accept_feed_ = false;
    session_finished_ = false;
    if (done_event_ != nullptr) {
        xEventGroupClearBits(done_event_, kDoneReceivedBit | kSendTaskExitBit);
    }

    if (!HeapOkForWs()) {
        LogHeap("Start: reject WS (low internal heap)");
        return false;
    }
    if (!InitPcmQueue()) {
        return false;
    }

    EnterTransportBoost();
    vTaskDelay(pdMS_TO_TICKS(80));

    auto network = Board::GetInstance().GetNetwork();
    std::unique_ptr<WebSocket> ws = network->CreateWebSocket(2);
    if (ws == nullptr) {
        DestroyPcmQueue();
        LeaveTransportBoost();
        return false;
    }

    const std::string url = BuildWebSocketUrl("/voice-chat/ws");
    ESP_LOGI(TAG, "WS connect (token redacted) path=%s/voice-chat/ws", kApiPrefix);
    ws->OnData([this](const char* data, size_t len, bool binary) {
        if (!binary && data != nullptr && len > 0) {
            HandleText(data, len);
        }
    });
    ws->OnDisconnected([this]() {
        ESP_LOGW(TAG, "WS disconnected (feeds=%u bytes=%u drops=%u)",
                 static_cast<unsigned>(pcm_feed_count_), static_cast<unsigned>(pcm_bytes_sent_),
                 static_cast<unsigned>(pcm_queue_drops_));
        stream_ready_ = false;
        pcm_accept_feed_ = false;
        running_ = false;
        send_task_run_ = false;
        SignalSessionEnd();
    });

    bool connected = false;
    for (int attempt = 1; attempt <= kWsConnectAttempts; ++attempt) {
        if (!HeapOkForWs()) {
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
        DestroyPcmQueue();
        LeaveTransportBoost();
        return false;
    }
    ESP_LOGI(TAG, "WS connected");

    websocket_ = ws.release();

    cJSON* start = cJSON_CreateObject();
    cJSON_AddStringToObject(start, "action", "start");
    if (chat_session_id > 0) {
        cJSON_AddNumberToObject(start, "chat_session_id", chat_session_id);
    } else {
        cJSON_AddNullToObject(start, "chat_session_id");
    }
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
    ESP_LOGI(TAG, "sent start JSON: pcm 16kHz mono s16le");

    if (!StartSendTask()) {
        Stop(false);
        return false;
    }

    running_ = true;
    stream_ready_ = true;
    pcm_accept_feed_ = true;
    return true;
}

/**
 * 将一帧 PCM 以 WebSocket 二进制帧发送到 /voice-chat/ws。
 * 由 PcmSendTask 从队列取出定长帧（默认 320 样点 ≈ 20ms @16kHz）后调用，不在音频回调里直接 Send。
 *
 * @param samples 16-bit 单声道 PCM 缓冲区
 * @param count   样点数（通常 kPcmFrameSamples=320）
 * @return 发送成功 true；参数无效、未连接或 Send 失败 false
 */
bool VoiceChatStream::SendPcmChunk(const int16_t* samples, size_t count) {
    // 无 socket、空指针或 0 长度 → 直接失败（调用方应保证 count 为整帧）
    if (websocket_ == nullptr || samples == nullptr || count == 0) {
        return false;
    }
    auto* socket = static_cast<WebSocket*>(websocket_);
    // 连接已断：标记 stream 不可用，后续 FeedPcm 入队帧会在发送任务里被丢弃
    if (!socket->IsConnected()) {
        stream_ready_ = false;
        return false;
    }

    const size_t bytes = count * sizeof(int16_t);
    // binary=true：服务端按二进制 PCM 解析（s16le 16kHz mono，与 start JSON 中 audio 一致）
    if (!socket->Send(reinterpret_cast<const char*>(samples), bytes, true)) {
        if (!socket->IsConnected()) {
            // 发送过程中断线：停流并清 stream_ready_，避免继续往死连接写数据
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

    // 统计上行：首帧与每 50 帧打一次日志，便于确认麦数据是否真的发出
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

void VoiceChatStream::FlushPcmAccumToQueue() {
    if (!pcm_accept_feed_ || pcm_queue_ == nullptr) {
        return;
    }
    while (pcm_accum_.size() >= kPcmFrameSamples) {
        if (xQueueSend(pcm_queue_, pcm_accum_.data(), 0) != pdTRUE) {
            pcm_accept_feed_ = false;
            return;
        }
        pcm_accum_.erase(pcm_accum_.begin(), pcm_accum_.begin() + kPcmFrameSamples);
    }
}

void VoiceChatStream::FeedPcm(const int16_t* samples, size_t count) {
    if (!running_ || !stream_ready_ || !pcm_accept_feed_ || pcm_queue_ == nullptr || samples == nullptr ||
        count == 0) {
        return;
    }
    pcm_accum_.insert(pcm_accum_.end(), samples, samples + count);
    FlushPcmAccumToQueue();
}

void VoiceChatStream::SignalSessionEnd() {
    if (session_finished_) {
        return;
    }
    session_finished_ = true;
    if (done_event_ != nullptr) {
        xEventGroupSetBits(done_event_, kDoneReceivedBit);
    }
}

void VoiceChatStream::CloseWebSocket() {
    if (websocket_ != nullptr) {
        delete static_cast<WebSocket*>(websocket_);
        websocket_ = nullptr;
    }
}

void VoiceChatStream::Stop(bool wait_for_done) {
    if (!running_ && websocket_ == nullptr && !transport_boost_ && send_task_handle_ == nullptr) {
        return;
    }

    stream_ready_ = false;
    pcm_accept_feed_ = false;
    running_ = false;
    send_task_run_ = false;

    if (websocket_ != nullptr) {
        auto* socket = static_cast<WebSocket*>(websocket_);
        ESP_LOGI(TAG, "Stop: sending end (feeds=%u bytes=%u wait_done=%d)",
                 static_cast<unsigned>(pcm_feed_count_), static_cast<unsigned>(pcm_bytes_sent_),
                 wait_for_done ? 1 : 0);
        if (socket->IsConnected()) {
            if (!socket->Send(R"({"action":"end"})")) {
                wait_for_done = false;
            }
        }
        socket->ShutdownTransport();
    }

    StopSendTask();
    DestroyPcmQueue();
    pcm_accum_.clear();

    if (websocket_ != nullptr && wait_for_done && !session_finished_ && done_event_ != nullptr) {
        const EventBits_t bits =
            xEventGroupWaitBits(done_event_, kDoneReceivedBit, pdFALSE, pdTRUE, kDoneWaitTicks);
        if ((bits & kDoneReceivedBit) == 0) {
            ESP_LOGW(TAG, "Stop: voice_chat_done timeout");
        }
    }

    WaitForMp3PlaybackDone();
    CloseWebSocket();
    LeaveTransportBoost();
}

void VoiceChatStream::HandleText(const char* data, size_t len) {
    std::string payload(data, len);
    cJSON* root = cJSON_Parse(payload.c_str());
    if (root == nullptr) {
        return;
    }
    auto type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type)) {
        cJSON_Delete(root);
        return;
    }

    const char* t = type->valuestring;
    if (strcmp(t, "asr") == 0) {
        auto text = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(text)) {
            LogTextPreview("WS asr", text->valuestring);
            if (on_asr_) {
                on_asr_(text->valuestring);
            }
        }
    } else if (strcmp(t, "asr_final") == 0) {
        auto text = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(text)) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                user_text_ = text->valuestring;
            }
            LogTextPreview("WS asr_final", text->valuestring);
            if (on_asr_final_) {
                on_asr_final_(text->valuestring);
            }
        }
    } else if (strcmp(t, "llm_start") == 0) {
        auto sid = cJSON_GetObjectItem(root, "chat_session_id");
        ESP_LOGI(TAG, "WS llm_start session=%d", cJSON_IsNumber(sid) ? sid->valueint : -1);
    } else if (strcmp(t, "llm_text") == 0) {
        auto text = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(text)) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                assistant_text_ = text->valuestring;
            }
            LogTextPreview("WS llm_text", text->valuestring);
            if (on_llm_text_) {
                on_llm_text_(text->valuestring);
            }
        }
    } else if (strcmp(t, "tts_start") == 0) {
        next_tts_seq_ = 0;
        ResetMp3Playback();
        ESP_LOGI(TAG, "WS tts_start");
        if (on_tts_state_) {
            on_tts_state_("start");
        }
    } else if (strcmp(t, "tts_chunk") == 0) {
        auto seq = cJSON_GetObjectItem(root, "seq");
        auto data_b64 = cJSON_GetObjectItem(root, "data");
        if (cJSON_IsNumber(seq) && cJSON_IsString(data_b64)) {
            const int seq_val = seq->valueint;
            if (seq_val != next_tts_seq_) {
                ESP_LOGW(TAG, "tts_chunk seq=%d expected=%d", seq_val, next_tts_seq_);
            }
            std::vector<uint8_t> mp3;
            if (DecodeBase64(data_b64->valuestring, strlen(data_b64->valuestring), mp3)) {
                FeedMp3Playback(mp3.data(), mp3.size());
                next_tts_seq_ = seq_val + 1;
            }
        }
    } else if (strcmp(t, "tts_done") == 0) {
        FlushMp3Playback();
        ESP_LOGI(TAG, "WS tts_done");
    } else if (strcmp(t, "voice_chat_done") == 0) {
        int chat_session_id = 0;
        auto ut = cJSON_GetObjectItem(root, "user_text");
        auto at = cJSON_GetObjectItem(root, "assistant_text");
        auto sid = cJSON_GetObjectItem(root, "chat_session_id");
        if (cJSON_IsString(ut)) {
            std::lock_guard<std::mutex> lock(mutex_);
            user_text_ = ut->valuestring;
        }
        if (cJSON_IsString(at)) {
            std::lock_guard<std::mutex> lock(mutex_);
            assistant_text_ = at->valuestring;
        }
        if (cJSON_IsNumber(sid)) {
            chat_session_id = sid->valueint;
        }
        std::string user_copy;
        std::string assistant_copy;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            user_copy = user_text_;
            assistant_copy = assistant_text_;
        }
        ESP_LOGI(TAG, "WS voice_chat_done session=%d", chat_session_id);
        if (on_tts_state_) {
            on_tts_state_("stop");
        }
        if (on_done_) {
            on_done_(user_copy, assistant_copy, chat_session_id);
        }
        SignalSessionEnd();
    } else if (strcmp(t, "error") == 0) {
        auto msg = cJSON_GetObjectItem(root, "message");
        const char* err = cJSON_IsString(msg) ? msg->valuestring : "unknown";
        ESP_LOGE(TAG, "WS error: %s", err);
        if (on_error_) {
            on_error_(err);
        }
        SignalSessionEnd();
    } else {
        ESP_LOGD(TAG, "WS type=%s", t);
    }
    cJSON_Delete(root);
}

}  // namespace oye
