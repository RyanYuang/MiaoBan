#include "oye_chat_protocol.h"

#include "application.h"
#include "audio_service.h"
#include "oye/oye_cloud_api.h"
#include "oye/oye_config.h"
#include "oye/oye_voice_chat_stream.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <cJSON.h>
#include <settings.h>

#include <cstring>
#include <mutex>

#define TAG "OyeChat"

namespace {

constexpr size_t kLogTextMax = 160;
constexpr int64_t kUserCheckCacheUs = 300 * 1000000LL;

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

}  // namespace

OyeChatProtocol::OyeChatProtocol() {
    voice_chat_stream_ = new oye::VoiceChatStream();
    server_sample_rate_ = 24000;
}

OyeChatProtocol::~OyeChatProtocol() {
    CloseAudioChannel(false);
    delete voice_chat_stream_;
    voice_chat_stream_ = nullptr;
}

void OyeChatProtocol::SetMainScheduler(std::function<void(std::function<void()>)> scheduler) {
    schedule_ = std::move(scheduler);
}

bool OyeChatProtocol::Start() {
    return true;
}

bool OyeChatProtocol::OpenAudioChannel() {
    ESP_LOGI(TAG, "[1/2] OpenAudioChannel: begin");
    if (!oye::HasAccessToken()) {
        ESP_LOGE(TAG, "[1/2] OpenAudioChannel: no access token");
        SetError("未绑定账号，请在 App 中 SET_USER_TOKEN");
        return false;
    }

    const int64_t now_us = esp_timer_get_time();
    oye::UserInfo user;
    if (user_check_ok_ && (now_us - user_check_time_us_) < kUserCheckCacheUs) {
        ESP_LOGI(TAG, "[1/2] OpenAudioChannel: skip /mcu/me (cached ok)");
    } else {
        if (oye::GetCurrentUser(user) != ESP_OK) {
            ESP_LOGE(TAG, "[1/2] OpenAudioChannel: GET /mcu/me failed");
            user_check_ok_ = false;
            SetError("Token 无效或已过期");
            return false;
        }
        user_check_ok_ = true;
        user_check_time_us_ = now_us;
        ESP_LOGI(TAG, "[1/2] OpenAudioChannel: user id=%d phone=%s nickname=%s", user.id,
                 user.phone.c_str(), user.nickname.c_str());
    }

    Settings settings("oye", false);
    chat_session_id_ = settings.GetInt("chat_session_id", 0);

    error_occurred_ = false;
    channel_open_ = true;
    session_id_ = chat_session_id_ > 0 ? std::to_string(chat_session_id_) : "voice-chat";
    last_incoming_time_ = std::chrono::steady_clock::now();

    if (on_connected_) {
        on_connected_();
    }
    if (on_audio_channel_opened_) {
        on_audio_channel_opened_();
    }
    ESP_LOGI(TAG, "[2/2] OpenAudioChannel: ok chat_session_id=%d", chat_session_id_);
    return true;
}

void OyeChatProtocol::CloseAudioChannel(bool send_goodbye) {
    (void)send_goodbye;
    ESP_LOGI(TAG, "CloseAudioChannel");
    speak_abort_ = true;
    Application::GetInstance().GetAudioService().ClearPcmTap();
    if (voice_chat_stream_ != nullptr) {
        voice_chat_stream_->Stop(false);
    }
    channel_open_ = false;
    user_check_ok_ = false;
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

void OyeChatProtocol::SendAbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "SendAbortSpeaking reason=%d", static_cast<int>(reason));
    speak_abort_ = true;
    Application::GetInstance().GetAudioService().ResetDecoder();
}

/**
 * 进入聆听态后的协议入口：建立 /voice-chat/ws，注册下行回调，再挂 PCM 上行。
 * 须在 OpenAudioChannel() 成功后由 Application::HandleStateChangedEvent 调用。
 * @param mode 聆听模式（Oye 当前未下发给服务端，仅作日志）
 */
void OyeChatProtocol::SendStartListening(ListeningMode mode) {
    ESP_LOGI(TAG, "[voice-chat] SendStartListening begin mode=%d session=%d schedule=%s",
             static_cast<int>(mode), chat_session_id_, schedule_ ? "set" : "null");

    // 1) 前置检查：逻辑通道须已打开（HTTP 鉴权完成），流对象须存在
    if (!channel_open_) {
        ESP_LOGW(TAG, "[voice-chat] [1/8] abort: channel_open_=0 (call OpenAudioChannel first)");
        return;
    }
    if (voice_chat_stream_ == nullptr) {
        ESP_LOGW(TAG, "[voice-chat] [1/8] abort: voice_chat_stream_=null");
        return;
    }
    ESP_LOGI(TAG, "[voice-chat] [1/8] precheck ok channel_open=1 stream=%p session_id=%s",
             static_cast<void*>(voice_chat_stream_), session_id_.c_str());

    // 2) 清除上一轮「用户打断播报」标志，允许本轮 TTS 正常播放
    const bool was_abort = speak_abort_.exchange(false);
    ESP_LOGI(TAG, "[voice-chat] [2/8] speak_abort cleared (was=%d)", was_abort ? 1 : 0);

    auto& audio = Application::GetInstance().GetAudioService();
    const bool processor_running_before = audio.IsAudioProcessorRunning();
    ESP_LOGI(TAG, "[voice-chat] [3/8] ClearPcmTap (voice_processor_running=%d)",
             processor_running_before ? 1 : 0);
    // 3) 摘掉旧的 PCM 回调，避免重复 tap 或 Stop 后残留上行
    audio.ClearPcmTap();

    // 4) 启动 VoiceChatStream：连 WebSocket、发 start JSON、建 PCM 发送队列
    //    以下 lambda 在 WS 接收线程触发，经 schedule_ 投递到 Application 主循环再改 UI/状态
    ESP_LOGI(TAG, "[voice-chat] [4/8] VoiceChatStream::Start begin chat_session_id=%d",
             chat_session_id_);
    const bool ok = voice_chat_stream_->Start(
        chat_session_id_,
        // 4a) ASR 中间结果 → 转成 type=stt，刷新用户字幕（边说边出字）
        [this](const std::string& text) {
            last_incoming_time_ = std::chrono::steady_clock::now();
            ESP_LOGD(TAG, "[voice-chat] [4a] ASR partial len=%u", static_cast<unsigned>(text.size()));
            LogTextPreview("[ASR] partial", text);
            if (schedule_) {
                schedule_([this, text]() { EmitJson("stt", text); });
            } else {
                ESP_LOGW(TAG, "[voice-chat] [4a] schedule_ null, drop partial stt");
            }
        },
        // 4b) ASR 最终结果 → 同样走 stt（覆盖/定格用户一句）
        [this](const std::string& text) {
            ESP_LOGI(TAG, "[voice-chat] [4b] ASR final len=%u", static_cast<unsigned>(text.size()));
            LogTextPreview("[ASR] final", text);
            if (schedule_) {
                schedule_([this, text]() { EmitJson("stt", text); });
            } else {
                ESP_LOGW(TAG, "[voice-chat] [4b] schedule_ null, drop final stt");
            }
        },
        // 4c) LLM 流式文本 → 转成 tts + sentence_start，更新助手字幕（尚未播音）
        [this](const std::string& text) {
            ESP_LOGI(TAG, "[voice-chat] [4c] LLM chunk len=%u", static_cast<unsigned>(text.size()));
            LogTextPreview("[LLM]", text);
            if (schedule_) {
                schedule_([this, text]() {
                    EmitJson("tts", text, "sentence_start");
                });
            } else {
                ESP_LOGW(TAG, "[voice-chat] [4c] schedule_ null, drop llm sentence_start");
            }
        },
        // 4d) TTS 状态机：start → Application 切 Speaking；stop → 回 Idle/Listening
        [this](const char* state) {
            if (schedule_ == nullptr || state == nullptr) {
                ESP_LOGW(TAG, "[voice-chat] [4d] skip tts state: schedule=%s state=%s",
                         schedule_ ? "set" : "null", state ? state : "null");
                return;
            }
            ESP_LOGI(TAG, "[voice-chat] [4d] TTS state=%s -> EmitJson", state);
            if (strcmp(state, "start") == 0) {
                schedule_([this]() { EmitJson("tts", "", "start"); });
            } else if (strcmp(state, "stop") == 0) {
                schedule_([this]() { EmitJson("tts", "", "stop"); });
            } else {
                ESP_LOGW(TAG, "[voice-chat] [4d] unknown TTS state=%s", state);
            }
        },
        // 4e) 一轮 voice_chat_done：持久化服务端分配的新 chat_session_id
        [this](const std::string& user_text, const std::string& assistant_text, int session_id) {
            ESP_LOGI(TAG, "[voice-chat] [4e] voice_chat_done session_id=%d (local=%d)",
                     session_id, chat_session_id_);
            if (session_id > 0 && session_id != chat_session_id_) {
                const int old_session = chat_session_id_;
                chat_session_id_ = session_id;
                Settings writable("oye", true);
                writable.SetInt("chat_session_id", session_id);
                session_id_ = std::to_string(session_id);
                ESP_LOGI(TAG, "[voice-chat] [4e] persist chat_session_id %d -> %d",
                         old_session, session_id);
            }
            LogTextPreview("[done] user", user_text);
            LogTextPreview("[done] assistant", assistant_text);
        },
        // 4f) WS/管线错误 → 主线程 SetError，触发 MAIN_EVENT_ERROR 弹窗并回 Idle
        [this](const std::string& message) {
            ESP_LOGE(TAG, "[voice-chat] [4f] stream error: %s", message.c_str());
            if (schedule_) {
                schedule_([this, message]() { SetError(message); });
            } else {
                ESP_LOGW(TAG, "[voice-chat] [4f] schedule_ null, cannot SetError on main loop");
            }
        });

    // 5) WS 握手或 start JSON 失败：不挂 tap、不开麦，通知用户
    if (!ok) {
        ESP_LOGE(TAG, "[voice-chat] [5/8] VoiceChatStream::Start failed (no tap, no voice processing)");
        if (schedule_) {
            schedule_([this]() {
                SetError("语音通道连接失败，请稍后重试或重启设备");
            });
        } else {
            ESP_LOGW(TAG, "[voice-chat] [5/8] schedule_ null, user alert not scheduled");
        }
        return;
    }
    ESP_LOGI(TAG, "[voice-chat] [4/8] VoiceChatStream::Start ok running=%d",
             voice_chat_stream_->IsRunning() ? 1 : 0);

    // 6) WS 已就绪后再关唤醒词（与 Application 聆听态配置叠加，降低 internal heap 占用）
    ESP_LOGI(TAG, "[voice-chat] [6/8] EnableWakeWordDetection(false)");
    audio.EnableWakeWordDetection(false);

    // 7) 注册 PCM tap：AFE 处理后的 16k 单声道帧 → 入队 → 独立任务二进制上行
    //    必须在 Start 成功之后挂 tap，避免握手期间堆积 PCM 占满内存
    ESP_LOGI(TAG, "[voice-chat] [7/8] SetPcmTap -> FeedPcm (post-WS, avoid pre-handshake queue)");
    pcm_tap_first_feed_logged_ = false;
    audio.SetPcmTap([this](const std::vector<int16_t>& pcm) {
        if (voice_chat_stream_ == nullptr || !voice_chat_stream_->IsRunning()) {
            ESP_LOGD(TAG, "[voice-chat] [7/8] tap drop: stream=%p running=%d samples=%u",
                     static_cast<void*>(voice_chat_stream_),
                     voice_chat_stream_ ? (voice_chat_stream_->IsRunning() ? 1 : 0) : 0,
                     static_cast<unsigned>(pcm.size()));
            return;
        }
        if (!pcm_tap_first_feed_logged_.exchange(true)) {
            ESP_LOGI(TAG, "[voice-chat] [7/8] first PCM tap feed samples=%u",
                     static_cast<unsigned>(pcm.size()));
        }
#if CONFIG_OYE_SPEECH_END_DETECTION
        Application::GetInstance().ObserveUplinkPcmForSpeechEnd(pcm.data(), pcm.size());
#endif
        voice_chat_stream_->FeedPcm(pcm.data(), pcm.size());
    });

    // 8) 若语音处理管线未跑则启动（采集 + AFE）；已运行则仅复用现有管线
    if (!audio.IsAudioProcessorRunning()) {
        ESP_LOGI(TAG, "[voice-chat] [8/8] EnableVoiceProcessing(true) (was not running)");
        audio.EnableVoiceProcessing(true);
    } else {
        ESP_LOGI(TAG, "[voice-chat] [8/8] reuse existing voice processor (already running)");
    }
    ESP_LOGI(TAG,
             "[voice-chat] SendStartListening done: ws=ok tap=on wake_word=off processor=%d",
             audio.IsAudioProcessorRunning() ? 1 : 0);
}

void OyeChatProtocol::SendStopListening() {
    ESP_LOGI(TAG, "[voice-chat] SendStopListening");
    if (!channel_open_ || voice_chat_stream_ == nullptr) {
        ESP_LOGW(TAG, "[voice-chat] SendStopListening skipped");
        return;
    }
    Application::GetInstance().GetAudioService().ClearPcmTap();
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        if (processing_) {
            ESP_LOGW(TAG, "[voice-chat] utterance worker already running");
            return;
        }
        processing_ = true;
    }
    ESP_LOGI(TAG, "[voice-chat] spawn utterance worker (end -> voice_chat_done)");
    xTaskCreate(ProcessUtteranceEntry, "oye_chat", 12288, this, 5, &worker_);
}

void OyeChatProtocol::ProcessUtteranceEntry(void* arg) {
    static_cast<OyeChatProtocol*>(arg)->ProcessUtteranceTask(arg);
}

void OyeChatProtocol::ProcessUtteranceTask(void* arg) {
    (void)arg;
    ESP_LOGI(TAG, "[pipeline] utterance worker started");
    if (voice_chat_stream_ != nullptr) {
        ESP_LOGI(TAG, "[voice-chat] stopping WS, waiting for voice_chat_done + TTS");
        voice_chat_stream_->Stop(true);
    }

    if (speak_abort_.load()) {
        ESP_LOGI(TAG, "[pipeline] aborted by user");
    }

    processing_ = false;
    worker_ = nullptr;
    last_incoming_time_ = std::chrono::steady_clock::now();
    auto& audio = Application::GetInstance().GetAudioService();
    if (Application::GetInstance().GetDeviceState() == kDeviceStateListening) {
        audio.EnableWakeWordDetection(audio.IsAfeWakeWord());
    }
    ESP_LOGI(TAG, "[pipeline] utterance worker finished");
    vTaskDelete(nullptr);
}

void OyeChatProtocol::EmitJson(const char* type, const std::string& text, const char* state) {
    if (on_incoming_json_ == nullptr) {
        return;
    }
    if (strcmp(type, "stt") == 0) {
        LogTextPreview("EmitJson stt", text);
    } else if (strcmp(type, "tts") == 0 && state != nullptr) {
        ESP_LOGD(TAG, "EmitJson tts state=%s text_len=%u", state, static_cast<unsigned>(text.size()));
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
