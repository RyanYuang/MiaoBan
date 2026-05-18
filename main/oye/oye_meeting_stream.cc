#include "oye_meeting_stream.h"

#include "oye_config.h"
#include "oye_http.h"

#include <board.h>
#include <cJSON.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <web_socket.h>

#include <cstring>
#include <memory>

namespace oye {

static const char* TAG = "OyeMeetingWs";

std::string MeetingStream::BuildWsUrl() {
    std::string base = GetApiBaseUrl();
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    std::string ws_base;
    if (base.rfind("https://", 0) == 0) {
        ws_base = "wss://" + base.substr(8);
    } else if (base.rfind("http://", 0) == 0) {
        ws_base = "ws://" + base.substr(7);
    } else {
        ws_base = "ws://" + base;
    }
    return ws_base + std::string(kApiPrefix) + "/meetings/stream/ws?token=" + GetAccessTokenRaw();
}

bool MeetingStream::Start(const std::string& title, bool save_meeting, TextCallback on_asr,
                          DoneCallback on_done) {
    Stop();
    if (!HasAccessToken()) {
        return false;
    }

    on_asr_ = std::move(on_asr);
    on_done_ = std::move(on_done);
    save_meeting_ = save_meeting;

    auto network = Board::GetInstance().GetNetwork();
    std::unique_ptr<WebSocket> ws = network->CreateWebSocket(1);
    if (ws == nullptr) {
        return false;
    }

    std::string url = BuildWsUrl();
    ws->OnData([this](const char* data, size_t len, bool binary) {
        if (!binary && data != nullptr && len > 0) {
            HandleText(data, len);
        }
    });

    if (!ws->Connect(url.c_str())) {
        ESP_LOGE(TAG, "WS connect failed");
        return false;
    }

    websocket_ = ws.release();
    running_ = true;

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
        ESP_LOGE(TAG, "send start failed");
        Stop();
        return false;
    }
    ESP_LOGI(TAG, "meeting stream started");
    return true;
}

void MeetingStream::FeedPcm(const int16_t* samples, size_t count) {
    if (!running_ || websocket_ == nullptr || samples == nullptr || count == 0) {
        return;
    }
    auto* socket = static_cast<WebSocket*>(websocket_);
    socket->Send(reinterpret_cast<const char*>(samples), count * sizeof(int16_t), true);
}

void MeetingStream::Stop() {
    if (websocket_ != nullptr && running_) {
        auto* socket = static_cast<WebSocket*>(websocket_);
        std::string end = R"({"action":"end"})";
        socket->Send(end);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (websocket_ != nullptr) {
        delete static_cast<WebSocket*>(websocket_);
        websocket_ = nullptr;
    }
    running_ = false;
}

std::string MeetingStream::LatestText() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_text_;
}

void MeetingStream::HandleText(const char* data, size_t len) {
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

    if (strcmp(type->valuestring, "asr") == 0) {
        auto text = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(text)) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                latest_text_ = text->valuestring;
            }
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
        if (on_done_) {
            on_done_(latest_text_, meeting_id);
        }
    } else if (strcmp(type->valuestring, "error") == 0) {
        auto msg = cJSON_GetObjectItem(root, "message");
        ESP_LOGE(TAG, "stream error: %s", cJSON_IsString(msg) ? msg->valuestring : "unknown");
    }
    cJSON_Delete(root);
}

}  // namespace oye
