#ifndef OYE_VOICE_CHAT_STREAM_H
#define OYE_VOICE_CHAT_STREAM_H

class WebSocket;

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/idf_additions.h>
#include <freertos/queue.h>

namespace oye {

/** Voice assistant: PCM → WS /mcu/voice-chat/ws → ASR + LLM + TTS (binary PCM uplink). */
class VoiceChatStream {
public:
    using TextCallback = std::function<void(const std::string& text)>;
    using DoneCallback =
        std::function<void(const std::string& user_text, const std::string& assistant_text, int chat_session_id)>;
    using ErrorCallback = std::function<void(const std::string& message)>;

    VoiceChatStream();
    ~VoiceChatStream();

    bool Start(int chat_session_id, TextCallback on_asr, TextCallback on_asr_final,
               std::function<void()> on_llm_start, TextCallback on_llm_text,
               std::function<void(const char* state)> on_tts_state, DoneCallback on_done,
               ErrorCallback on_error);
    void FeedPcm(const int16_t* samples, size_t count);
    void Stop(bool wait_for_done = true);
    bool IsRunning() const { return running_; }

private:
    static constexpr TickType_t kDoneWaitTicks = pdMS_TO_TICKS(120000);
    /** 20ms @16kHz. Keep WS/TCP frames small enough for ESP32-S3's tight lwIP send window. */
    static constexpr size_t kPcmFrameSamples = 320;
    static constexpr size_t kPcmFrameBytes = kPcmFrameSamples * sizeof(int16_t);
    /**
     * Buffer several seconds of 20ms PCM locally while the backend finishes
     * the upstream ASR handshake, then start uplink after it sends `ready`.
     */
    static constexpr UBaseType_t kPcmQueueDepth = 256;
    static constexpr size_t kPcmAccumMaxSamples = kPcmFrameSamples * 3;

    static constexpr EventBits_t kDoneReceivedBit = BIT0;
    static constexpr EventBits_t kSendTaskExitBit = BIT1;

    bool running_ = false;
    bool stream_ready_ = false;
    volatile bool uplink_failed_ = false;
    bool session_finished_ = false;
    bool transport_boost_ = false;
    volatile bool send_task_run_ = false;
    uint32_t pcm_feed_count_ = 0;
    uint32_t pcm_bytes_sent_ = 0;
    uint32_t pcm_queue_drops_ = 0;
    int next_tts_seq_ = 0;
    std::string user_text_;
    std::string assistant_text_;
    mutable std::mutex mutex_;
    TextCallback on_asr_;
    TextCallback on_asr_final_;
    std::function<void()> on_llm_start_;
    TextCallback on_llm_text_;
    std::function<void(const char* state)> on_tts_state_;
    DoneCallback on_done_;
    ErrorCallback on_error_;
    WebSocket* websocket_ = nullptr;
    EventGroupHandle_t done_event_ = nullptr;
    QueueHandle_t pcm_queue_ = nullptr;
    uint8_t* pcm_queue_storage_ = nullptr;
    StaticQueue_t pcm_queue_buffer_{};
    TaskHandle_t send_task_handle_ = nullptr;
    int16_t send_frame_[kPcmFrameSamples];
    std::vector<int16_t> pcm_accum_;

    static void SendTaskEntry(void* arg);
    void FlushPcmAccumToQueue();
    void HandleText(const char* data, size_t len);
    bool SendPcmChunk(const int16_t* samples, size_t count);
    void SignalSessionEnd();
    void CloseWebSocket();
    void EnterTransportBoost();
    void LeaveTransportBoost();
    bool InitPcmQueue();
    void DestroyPcmQueue();
    bool StartSendTask();
    void StopSendTask();
    void PcmSendTask();
};

}  // namespace oye

#endif
