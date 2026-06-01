#ifndef OYE_MEETING_STREAM_H
#define OYE_MEETING_STREAM_H

class WebSocket;

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>

namespace oye {

/** Meeting minutes ASR: PCM 16k mono s16le → WS /mcu/meetings/stream/ws (backend relay). */
class MeetingStream {
public:
    using TextCallback = std::function<void(const std::string& text)>;
    using DoneCallback = std::function<void(const std::string& text, int meeting_id)>;

    MeetingStream();
    ~MeetingStream();

    bool Start(const std::string& title, bool save_meeting, TextCallback on_asr, DoneCallback on_done);
    void FeedPcm(const int16_t* samples, size_t count);
    /** @param wait_for_done 为 true 时在后台会话里等待 type=done；主线程应传 false。 */
    void Stop(bool wait_for_done = true, bool send_end = true);
    bool IsRunning() const { return running_; }
    /** 后端已发 type=ready，可开始 FeedPcm / 上行。 */
    bool IsUpstreamReady() const { return stream_ready_; }
    /** 阻塞直到 ready 或超时（用于会议页在就绪后再开麦）。 */
    bool WaitForUpstreamReady(TickType_t ticks);
    bool WasInterrupted() const { return interrupted_; }
    std::string LatestText() const;

private:
    static constexpr TickType_t kDoneWaitTicks = pdMS_TO_TICKS(15000);
    /** 20ms @16kHz. Keep WS/TCP frames small enough for ESP32-S3's tight lwIP send window. */
    static constexpr size_t kPcmFrameSamples = 320;
    static constexpr size_t kPcmFrameBytes = kPcmFrameSamples * sizeof(int16_t);
    /**
     * Buffer PCM locally while the backend finishes upstream ASR handshake, then uplink after `ready`.
     * Same depth as VoiceChatStream (~5s @ 20ms/frame).
     */
    static constexpr UBaseType_t kPcmQueueDepth = 256;
    static constexpr size_t kPcmAccumMaxSamples = kPcmFrameSamples * 3;

    static constexpr EventBits_t kDoneReceivedBit = BIT0;
    static constexpr EventBits_t kSendTaskExitBit = BIT1;
    static constexpr EventBits_t kStreamReadyBit = BIT2;

    bool running_ = false;
    bool stream_ready_ = false;
    bool session_finished_ = false;
    bool transport_boost_ = false;
    bool save_meeting_ = false;
    bool interrupted_ = false;
    bool end_requested_ = false;
    volatile bool send_task_run_ = false;
    volatile TickType_t next_pcm_send_tick_ = 0;
    uint32_t pcm_feed_count_ = 0;
    uint32_t pcm_bytes_sent_ = 0;
    uint32_t pcm_queue_drops_ = 0;
    std::string latest_text_;
    mutable std::mutex mutex_;
    TextCallback on_asr_;
    DoneCallback on_done_;
    WebSocket* websocket_ = nullptr;
    EventGroupHandle_t done_event_ = nullptr;
    QueueHandle_t pcm_queue_ = nullptr;
    uint8_t* pcm_queue_storage_ = nullptr;
    StaticQueue_t pcm_queue_buffer_{};
    TaskHandle_t send_task_handle_ = nullptr;
    int16_t send_frame_[kPcmFrameSamples];
    std::vector<int16_t> pcm_accum_;

    static std::string BuildWsUrl();
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
