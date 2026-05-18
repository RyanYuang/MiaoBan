#include "mcp_server.h"

#if CONFIG_USE_OYE_CLOUD_API

#include "application.h"
#include "oye/oye_audio_util.h"
#include "oye/oye_cloud_api.h"
#include "oye/oye_config.h"

#include <esp_log.h>

static const char* TAG = "OyeMcp";

void McpServer::AddOyeCloudTools() {
    if (!oye::HasAccessToken()) {
        return;
    }

    AddUserOnlyTool("self.oye.get_user",
                    "Get current Oye cloud user profile (requires BLE token).",
                    PropertyList(),
                    [](const PropertyList&) -> ReturnValue {
                        oye::UserInfo user;
                        if (oye::GetCurrentUser(user) != ESP_OK) {
                            throw std::runtime_error("GET /users/me failed");
                        }
                        cJSON* root = cJSON_CreateObject();
                        cJSON_AddNumberToObject(root, "id", user.id);
                        cJSON_AddStringToObject(root, "phone", user.phone.c_str());
                        cJSON_AddStringToObject(root, "nickname", user.nickname.c_str());
                        return root;
                    });

    AddUserOnlyTool("self.oye.chat.send",
                    "Send a text message to Oye AI chat (sync HTTP).",
                    PropertyList({Property("text", kPropertyTypeString)}),
                    [](const PropertyList& properties) -> ReturnValue {
                        int session_id = 0;
                        if (oye::EnsureChatSession(session_id) != ESP_OK) {
                            throw std::runtime_error("create session failed");
                        }
                        auto text = properties["text"].value<std::string>();
                        oye::ChatMessageResult result;
                        if (oye::SendChatMessage(session_id, text, result) != ESP_OK) {
                            throw std::runtime_error("send message failed");
                        }
                        cJSON* root = cJSON_CreateObject();
                        cJSON_AddStringToObject(root, "assistant", result.assistant_text.c_str());
                        return root;
                    });

    AddUserOnlyTool("self.oye.meeting.upload_recording",
                    "Record audio from mic, upload as meeting WAV, poll until summary ready.",
                    PropertyList({
                        Property("duration_ms", kPropertyTypeInteger, 5000, 3000, 120000),
                        Property("title", kPropertyTypeString, std::string("设备录音")),
                    }),
                    [](const PropertyList& properties) -> ReturnValue {
                        int duration_ms = properties["duration_ms"].value<int>();
                        auto title = properties["title"].value<std::string>();
                        if (title.empty()) {
                            title = "设备录音";
                        }
                        std::vector<int16_t> pcm;
                        if (!oye::RecordPcmMs(duration_ms, pcm)) {
                            throw std::runtime_error("record failed");
                        }
                        std::vector<uint8_t> wav;
                        if (!oye::BuildWavFromPcm(pcm, wav)) {
                            throw std::runtime_error("wav encode failed");
                        }
                        int meeting_id = 0;
                        if (oye::UploadMeetingAudio(wav.data(), wav.size(), title, meeting_id) != ESP_OK) {
                            throw std::runtime_error("upload failed");
                        }
                        oye::MeetingInfo info;
                        if (oye::PollMeetingUntilDone(meeting_id, info, 300) != ESP_OK) {
                            throw std::runtime_error("poll failed: " + info.status);
                        }
                        cJSON* root = cJSON_CreateObject();
                        cJSON_AddNumberToObject(root, "meeting_id", meeting_id);
                        cJSON_AddStringToObject(root, "status", info.status.c_str());
                        cJSON_AddStringToObject(root, "summary", info.summary.c_str());
                        return root;
                    });

    AddUserOnlyTool("self.oye.voiceprint.register",
                    "Record ~4s audio and register current user voiceprint.",
                    PropertyList(),
                    [](const PropertyList&) -> ReturnValue {
                        std::vector<int16_t> pcm;
                        if (!oye::RecordPcmMs(4000, pcm)) {
                            throw std::runtime_error("record failed");
                        }
                        std::vector<uint8_t> wav;
                        if (!oye::BuildWavFromPcm(pcm, wav)) {
                            throw std::runtime_error("wav encode failed");
                        }
                        if (oye::RegisterMyVoiceprint(wav.data(), wav.size()) != ESP_OK) {
                            throw std::runtime_error("register failed");
                        }
                        return true;
                    });

    AddUserOnlyTool("self.oye.voiceprint.verify",
                    "Record ~3s audio and verify against registered voiceprint.",
                    PropertyList(),
                    [](const PropertyList&) -> ReturnValue {
                        std::vector<int16_t> pcm;
                        if (!oye::RecordPcmMs(3000, pcm)) {
                            throw std::runtime_error("record failed");
                        }
                        std::vector<uint8_t> wav;
                        if (!oye::BuildWavFromPcm(pcm, wav)) {
                            throw std::runtime_error("wav encode failed");
                        }
                        oye::VoiceprintVerifyResult result;
                        if (oye::VerifyMyVoiceprint(wav.data(), wav.size(), result) != ESP_OK) {
                            throw std::runtime_error("verify failed");
                        }
                        cJSON* root = cJSON_CreateObject();
                        cJSON_AddBoolToObject(root, "matched", result.matched);
                        cJSON_AddNumberToObject(root, "score", result.score);
                        cJSON_AddNumberToObject(root, "threshold", result.threshold);
                        return root;
                    });

    ESP_LOGI(TAG, "Oye cloud MCP tools registered");
}

#endif
