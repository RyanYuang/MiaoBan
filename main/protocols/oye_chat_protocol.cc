#include "oye_chat_protocol.h"

#include "application.h"
#include "audio_service.h"
#include "oye/oye_cloud_api.h"
#include "oye/oye_config.h"
#include "oye/oye_meeting_stream.h"

#include <esp_log.h>
#include <cJSON.h>

#define TAG "OyeChat"

OyeChatProtocol::OyeChatProtocol() {
    meeting_stream_ = new oye::MeetingStream();
}

OyeChatProtocol::~OyeChatProtocol() {
    CloseAudioChannel(false);
    delete meeting_stream_;
    meeting_stream_ = nullptr;
}

void OyeChatProtocol::SetMainScheduler(std::function<void(std::function<void()>)> scheduler) {
    schedule_ = std::move(scheduler);
}

bool OyeChatProtocol::Start() {
    return true;
}

bool OyeChatProtocol::OpenAudioChannel() {
    if (!oye::HasAccessToken()) {
        SetError("未绑定账号，请在 App 中 SET_USER_TOKEN");
        return false;
    }

    oye::UserInfo user;
    if (oye::GetCurrentUser(user) != ESP_OK) {
        SetError("Token 无效或已过期");
        return false;
    }

    if (oye::EnsureChatSession(chat_session_id_) != ESP_OK) {
        SetError("创建对话会话失败");
        return false;
    }

    error_occurred_ = false;
    channel_open_ = true;
    session_id_ = std::to_string(chat_session_id_);
    last_incoming_time_ = std::chrono::steady_clock::now();

    if (on_connected_) {
        on_connected_();
    }
    if (on_audio_channel_opened_) {
        on_audio_channel_opened_();
    }
    ESP_LOGI(TAG, "Oye chat channel open, user=%s session=%d", user.nickname.c_str(),
             chat_session_id_);
    return true;
}

void OyeChatProtocol::CloseAudioChannel(bool send_goodbye) {
    (void)send_goodbye;
    Application::GetInstance().GetAudioService().ClearPcmTap();
    if (meeting_stream_ != nullptr) {
        meeting_stream_->Stop();
    }
    channel_open_ = false;
    if (on_audio_channel_closed_) {
        on_audio_channel_closed_();
    }
    if (on_disconnected_) {
        on_disconnected_();
    }
}

bool OyeChatProtocol::IsAudioChannelOpened() const {
    return channel_open_ && !error_occurred_;
}

bool OyeChatProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    (void)packet;
    return true;
}

bool OyeChatProtocol::SendText(const std::string& text) {
    (void)text;
    return true;
}

void OyeChatProtocol::SendStartListening(ListeningMode mode) {
    (void)mode;
    if (!channel_open_ || meeting_stream_ == nullptr) {
        return;
    }

    auto& audio = Application::GetInstance().GetAudioService();
    audio.SetPcmTap([this](const std::vector<int16_t>& pcm) {
        if (meeting_stream_ == nullptr || !meeting_stream_->IsRunning()) {
            return;
        }
        meeting_stream_->FeedPcm(pcm.data(), pcm.size());
    });

    meeting_stream_->Start(
        "设备语音对话", false,
        [this](const std::string& text) {
            last_incoming_time_ = std::chrono::steady_clock::now();
            if (schedule_) {
                schedule_([this, text]() { EmitJson("stt", text); });
            }
        },
        nullptr);
}

void OyeChatProtocol::SendStopListening() {
    if (!channel_open_ || meeting_stream_ == nullptr) {
        return;
    }
    Application::GetInstance().GetAudioService().ClearPcmTap();
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        if (processing_) {
            return;
        }
        processing_ = true;
    }
    xTaskCreate(ProcessUtteranceEntry, "oye_chat", 8192, this, 5, &worker_);
}

void OyeChatProtocol::ProcessUtteranceEntry(void* arg) {
    static_cast<OyeChatProtocol*>(arg)->ProcessUtteranceTask(arg);
}

void OyeChatProtocol::ProcessUtteranceTask(void* arg) {
    (void)arg;
    std::string user_text;
    if (meeting_stream_ != nullptr) {
        meeting_stream_->Stop();
        user_text = meeting_stream_->LatestText();
    }

    if (user_text.empty()) {
        if (schedule_) {
            schedule_([this]() { SetError("未识别到语音"); });
        }
        processing_ = false;
        worker_ = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    oye::ChatMessageResult chat;
    esp_err_t err = oye::SendChatMessage(chat_session_id_, user_text, chat);
    if (err != ESP_OK) {
        if (schedule_) {
            schedule_([this]() { SetError("对话请求失败"); });
        }
        processing_ = false;
        worker_ = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    if (schedule_) {
        schedule_([this, user_text, reply = chat.assistant_text]() {
            EmitJson("tts", "", "start");
            EmitJson("tts", reply, "sentence_start");
            EmitJson("tts", "", "stop");
            last_incoming_time_ = std::chrono::steady_clock::now();
        });
    }

    processing_ = false;
    worker_ = nullptr;
    vTaskDelete(nullptr);
}

void OyeChatProtocol::EmitJson(const char* type, const std::string& text, const char* state) {
    if (on_incoming_json_ == nullptr) {
        return;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", type);
    if (state != nullptr) {
        cJSON_AddStringToObject(root, "state", state);
    }
    if (!text.empty()) {
        cJSON_AddStringToObject(root, "text", text.c_str());
    }
    on_incoming_json_(root);
    cJSON_Delete(root);
}
