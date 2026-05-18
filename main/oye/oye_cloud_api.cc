#include "oye_cloud_api.h"

#include "oye_http.h"

#include <cJSON.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstring>
#include <settings.h>

namespace oye {

static const char* TAG = "OyeApi";

bool BuildWavFromPcm(const std::vector<int16_t>& pcm, std::vector<uint8_t>& wav_out) {
    if (pcm.empty()) {
        return false;
    }
    const uint32_t data_size = static_cast<uint32_t>(pcm.size() * sizeof(int16_t));
    const uint32_t file_size = 36 + data_size;
    wav_out.resize(44 + data_size);

    auto write_u32 = [&](size_t off, uint32_t v) {
        wav_out[off] = static_cast<uint8_t>(v & 0xff);
        wav_out[off + 1] = static_cast<uint8_t>((v >> 8) & 0xff);
        wav_out[off + 2] = static_cast<uint8_t>((v >> 16) & 0xff);
        wav_out[off + 3] = static_cast<uint8_t>((v >> 24) & 0xff);
    };
    auto write_u16 = [&](size_t off, uint16_t v) {
        wav_out[off] = static_cast<uint8_t>(v & 0xff);
        wav_out[off + 1] = static_cast<uint8_t>((v >> 8) & 0xff);
    };

    memcpy(wav_out.data(), "RIFF", 4);
    write_u32(4, file_size);
    memcpy(wav_out.data() + 8, "WAVE", 4);
    memcpy(wav_out.data() + 12, "fmt ", 4);
    write_u32(16, 16);
    write_u16(20, 1);
    write_u16(22, 1);
    write_u32(24, 16000);
    write_u32(28, 16000 * 2);
    write_u16(32, 2);
    write_u16(34, 16);
    memcpy(wav_out.data() + 36, "data", 4);
    write_u32(40, data_size);
    memcpy(wav_out.data() + 44, pcm.data(), data_size);
    return true;
}

esp_err_t GetCurrentUser(UserInfo& out) {
    auto res = RequestJson("GET", "/users/me");
    if (!res.Ok()) {
        ESP_LOGE(TAG, "GET /users/me failed code=%d %s", res.code, res.message.c_str());
        return ESP_FAIL;
    }
    if (res.data == nullptr) {
        return ESP_FAIL;
    }
    auto id = cJSON_GetObjectItem(res.data, "id");
    auto phone = cJSON_GetObjectItem(res.data, "phone");
    auto nickname = cJSON_GetObjectItem(res.data, "nickname");
    if (cJSON_IsNumber(id)) {
        out.id = id->valueint;
    }
    if (cJSON_IsString(phone)) {
        out.phone = phone->valuestring;
    }
    if (cJSON_IsString(nickname)) {
        out.nickname = nickname->valuestring;
    }
    return ESP_OK;
}

esp_err_t EnsureChatSession(int& session_id) {
    Settings settings("oye", false);
    session_id = settings.GetInt("chat_session_id", 0);
    if (session_id > 0) {
        return ESP_OK;
    }

    std::string body = R"({"title":"设备助手","type":"personal","group_id":null})";
    auto res = RequestJson("POST", "/chats/sessions", body);
    if (!res.Ok() || res.data == nullptr) {
        ESP_LOGE(TAG, "create session failed code=%d", res.code);
        return ESP_FAIL;
    }
    auto id = cJSON_GetObjectItem(res.data, "id");
    if (!cJSON_IsNumber(id)) {
        return ESP_FAIL;
    }
    session_id = id->valueint;
    Settings writable("oye", true);
    writable.SetInt("chat_session_id", session_id);
    ESP_LOGI(TAG, "chat session_id=%d", session_id);
    return ESP_OK;
}

esp_err_t SendChatMessage(int session_id, const std::string& user_text, ChatMessageResult& out) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "role", "user");
    cJSON_AddStringToObject(root, "content", user_text.c_str());
    char* printed = cJSON_PrintUnformatted(root);
    std::string body = printed ? printed : "";
    cJSON_free(printed);
    cJSON_Delete(root);

    std::string path = "/chats/sessions/" + std::to_string(session_id) + "/messages";
    auto res = RequestJson("POST", path.c_str(), body);
    if (!res.Ok() || res.data == nullptr) {
        ESP_LOGE(TAG, "send message failed code=%d %s", res.code, res.message.c_str());
        return ESP_FAIL;
    }

    auto user_msg = cJSON_GetObjectItem(res.data, "user_message");
    auto assistant_msg = cJSON_GetObjectItem(res.data, "assistant_message");
    if (cJSON_IsObject(user_msg)) {
        auto content = cJSON_GetObjectItem(user_msg, "content");
        if (cJSON_IsString(content)) {
            out.user_text = content->valuestring;
        }
    }
    if (cJSON_IsObject(assistant_msg)) {
        auto content = cJSON_GetObjectItem(assistant_msg, "content");
        if (cJSON_IsString(content)) {
            out.assistant_text = content->valuestring;
        }
    }
    return out.assistant_text.empty() ? ESP_FAIL : ESP_OK;
}

static esp_err_t ParseMeeting(const cJSON* data, MeetingInfo& out) {
    if (data == nullptr) {
        return ESP_FAIL;
    }
    auto id = cJSON_GetObjectItem(data, "id");
    auto title = cJSON_GetObjectItem(data, "title");
    auto status = cJSON_GetObjectItem(data, "status");
    auto summary = cJSON_GetObjectItem(data, "summary");
    auto transcript = cJSON_GetObjectItem(data, "transcript_text");
    auto err = cJSON_GetObjectItem(data, "error_message");
    if (cJSON_IsNumber(id)) {
        out.id = id->valueint;
    }
    if (cJSON_IsString(title)) {
        out.title = title->valuestring;
    }
    if (cJSON_IsString(status)) {
        out.status = status->valuestring;
    }
    if (cJSON_IsString(summary)) {
        out.summary = summary->valuestring;
    }
    if (cJSON_IsString(transcript)) {
        out.transcript_text = transcript->valuestring;
    }
    if (cJSON_IsString(err)) {
        out.error_message = err->valuestring;
    }
    return ESP_OK;
}

esp_err_t UploadMeetingAudio(const uint8_t* wav_data, size_t wav_size, const std::string& title,
                             int& meeting_id) {
    auto res = RequestMultipart("/meetings/upload", "audio", "meet.wav", "audio/wav", wav_data,
                                wav_size, "title", title);
    if (!res.Ok() || res.data == nullptr) {
        ESP_LOGE(TAG, "upload meeting failed code=%d", res.code);
        return ESP_FAIL;
    }
    MeetingInfo info;
    ParseMeeting(res.data, info);
    meeting_id = info.id;
    return meeting_id > 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t SubmitMeetingTranscript(const std::string& title, const std::string& transcript,
                                  int& meeting_id) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "title", title.c_str());
    cJSON_AddStringToObject(root, "transcript_text", transcript.c_str());
    cJSON_AddNullToObject(root, "group_id");
    char* printed = cJSON_PrintUnformatted(root);
    std::string body = printed ? printed : "";
    cJSON_free(printed);
    cJSON_Delete(root);

    auto res = RequestJson("POST", "/meetings/stream/transcript", body);
    if (!res.Ok() || res.data == nullptr) {
        return ESP_FAIL;
    }
    MeetingInfo info;
    ParseMeeting(res.data, info);
    meeting_id = info.id;
    return meeting_id > 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t GetMeeting(int meeting_id, MeetingInfo& out) {
    std::string path = "/meetings/" + std::to_string(meeting_id);
    auto res = RequestJson("GET", path);
    if (!res.Ok()) {
        return ESP_FAIL;
    }
    return ParseMeeting(res.data, out);
}

esp_err_t PollMeetingUntilDone(int meeting_id, MeetingInfo& out, int timeout_sec) {
    const int interval_ms = 3000;
    int elapsed_ms = 0;
    while (elapsed_ms < timeout_sec * 1000) {
        if (GetMeeting(meeting_id, out) != ESP_OK) {
            return ESP_FAIL;
        }
        if (out.status == "done") {
            return ESP_OK;
        }
        if (out.status == "failed") {
            ESP_LOGE(TAG, "meeting failed: %s", out.error_message.c_str());
            return ESP_FAIL;
        }
        vTaskDelay(pdMS_TO_TICKS(interval_ms));
        elapsed_ms += interval_ms;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t RegisterMyVoiceprint(const uint8_t* wav_data, size_t wav_size) {
    auto res = RequestMultipart("/meetings/voiceprint/me/register", "audio", "voice.wav", "audio/wav",
                                wav_data, wav_size);
    if (!res.Ok()) {
        ESP_LOGE(TAG, "voiceprint register failed code=%d", res.code);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t VerifyMyVoiceprint(const uint8_t* wav_data, size_t wav_size, VoiceprintVerifyResult& out) {
    auto res = RequestMultipart("/meetings/voiceprint/me/verify", "audio", "voice.wav", "audio/wav",
                                wav_data, wav_size);
    if (!res.Ok() || res.data == nullptr) {
        return ESP_FAIL;
    }
    auto matched = cJSON_GetObjectItem(res.data, "matched");
    auto score = cJSON_GetObjectItem(res.data, "score");
    auto threshold = cJSON_GetObjectItem(res.data, "threshold");
    if (cJSON_IsBool(matched)) {
        out.matched = cJSON_IsTrue(matched);
    }
    if (cJSON_IsNumber(score)) {
        out.score = static_cast<float>(score->valuedouble);
    }
    if (cJSON_IsNumber(threshold)) {
        out.threshold = static_cast<float>(threshold->valuedouble);
    }
    return ESP_OK;
}

}  // namespace oye
