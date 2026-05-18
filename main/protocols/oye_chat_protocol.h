#ifndef OYE_CHAT_PROTOCOL_H
#define OYE_CHAT_PROTOCOL_H

#include "protocol.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <functional>
#include <mutex>
#include <string>

namespace oye {
class MeetingStream;
}

/**
 * Oye cloud voice dialog: meeting WS ASR + POST /chats/.../messages.
 * Replaces MQTT/WebSocket Opus streaming when CONFIG_USE_OYE_CLOUD_API is enabled.
 */
class OyeChatProtocol : public Protocol {
public:
    OyeChatProtocol();
    ~OyeChatProtocol() override;

    void SetMainScheduler(std::function<void(std::function<void()>)> scheduler);

    bool Start() override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    void SendStartListening(ListeningMode mode) override;
    void SendStopListening() override;

protected:
    bool SendText(const std::string& text) override;

private:
    std::function<void(std::function<void()>)> schedule_;
    bool channel_open_ = false;
    int chat_session_id_ = 0;
    oye::MeetingStream* meeting_stream_ = nullptr;
    TaskHandle_t worker_ = nullptr;
    std::mutex worker_mutex_;
    bool processing_ = false;

    void EmitJson(const char* type, const std::string& text, const char* state = nullptr);
    void ProcessUtteranceTask(void* arg);
    static void ProcessUtteranceEntry(void* arg);
};

#endif
