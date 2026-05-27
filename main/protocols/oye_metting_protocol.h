#ifndef OYE_METTING_PROTOCOL_H
#define OYE_METTING_PROTOCOL_H

#include "protocol.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

namespace oye {
class VoiceChatStream;
}

/**
 * Oye cloud voice dialog: WS /mcu/voice-chat/ws (binary PCM uplink, embedded ASR+LLM+TTS).
 * Sole voice protocol when CONFIG_USE_OYE_CLOUD_API is enabled (Xiaozhi MQTT/WS not built).
 */
class OyeMettingProtocol : public Protocol {
public:
    OyeMettingProtocol();
    ~OyeMettingProtocol() override;

    void SetMainScheduler(std::function<void(std::function<void()>)> scheduler);

    bool Start() override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    void SendStartListening(ListeningMode mode) override;
    void SendStopListening() override;
    void SendAbortSpeaking(AbortReason reason) override;
    bool IsVoiceResultPending() const override;

protected:
    bool SendText(const std::string& text) override;

private:
    std::function<void(std::function<void()>)> schedule_;
    bool channel_open_ = false;
    int chat_session_id_ = 0;
    oye::VoiceChatStream* voice_chat_stream_ = nullptr;
    TaskHandle_t worker_ = nullptr;
    std::mutex worker_mutex_;
    std::atomic<bool> processing_{false};
    std::atomic<bool> speak_abort_{false};
    std::atomic<bool> pcm_tap_first_feed_logged_{false};
    bool user_check_ok_ = false;
    int64_t user_check_time_us_ = 0;

    void EmitJson(const char* type, const std::string& text, const char* state = nullptr);
    void ProcessUtteranceTask(void* arg);
    static void ProcessUtteranceEntry(void* arg);
};

#endif  // OYE_METTING_PROTOCOL_H
