#include "oye_tts_stream.h"

#include "oye_config.h"
#include "oye_http.h"

#include "application.h"
#include "audio_service.h"

#include <board.h>
#include <cJSON.h>
#include <decoder/impl/esp_mp3_dec.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <mbedtls/base64.h>

#include <cstring>
#include <memory>
#include <vector>

namespace oye {

namespace {

constexpr char TAG[] = "OyeTts";
constexpr int kTtsHttpTimeoutMs = 90000;
constexpr size_t kHttpReadChunk = 1024;
constexpr size_t kPcmOutCapacity = 8192;
constexpr size_t kLogTextMax = 160;

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

class Mp3StreamPlayer {
public:
    Mp3StreamPlayer() {
        if (esp_mp3_dec_open(nullptr, 0, &decoder_) != ESP_AUDIO_ERR_OK) {
            decoder_ = nullptr;
            ESP_LOGE(TAG, "esp_mp3_dec_open failed");
        } else {
            ESP_LOGI(TAG, "MP3 decoder opened");
        }
        pcm_out_.resize(kPcmOutCapacity);
    }

    ~Mp3StreamPlayer() {
        ESP_LOGI(TAG,
                 "MP3 stats: sse_chunks=%u mp3_bytes=%u pcm_frames=%u pcm_samples=%u carry_left=%u",
                 static_cast<unsigned>(sse_chunk_count_), static_cast<unsigned>(total_mp3_bytes_),
                 static_cast<unsigned>(pcm_frame_count_), static_cast<unsigned>(total_pcm_samples_),
                 static_cast<unsigned>(carry_.size()));
        if (decoder_ != nullptr) {
            esp_mp3_dec_close(decoder_);
            decoder_ = nullptr;
        }
    }

    bool Ok() const { return decoder_ != nullptr; }

    void Feed(const uint8_t* data, size_t len) {
        if (decoder_ == nullptr || data == nullptr || len == 0) {
            return;
        }
        total_mp3_bytes_ += len;
        carry_.insert(carry_.end(), data, data + len);
        DecodeAvailable();
    }

    void Flush() {
        ESP_LOGI(TAG, "MP3 flush, carry=%u bytes", static_cast<unsigned>(carry_.size()));
        DecodeAvailable();
    }

    void OnSseChunk() { ++sse_chunk_count_; }

private:
    void DecodeAvailable() {
        while (!carry_.empty() && decoder_ != nullptr) {
            esp_audio_dec_in_raw_t raw = {
                .buffer = carry_.data(),
                .len = static_cast<uint32_t>(carry_.size()),
                .consumed = 0,
                .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
            };
            esp_audio_dec_out_frame_t out = {
                .buffer = reinterpret_cast<uint8_t*>(pcm_out_.data()),
                .len = static_cast<uint32_t>(pcm_out_.size() * sizeof(int16_t)),
                .needed_size = 0,
                .decoded_size = 0,
            };
            esp_audio_dec_info_t info = {};
            const auto ret = esp_mp3_dec_decode(decoder_, &raw, &out, &info);
            if (raw.consumed > 0) {
                carry_.erase(carry_.begin(), carry_.begin() + raw.consumed);
            }
            if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                ESP_LOGW(TAG, "PCM buffer too small, grow %u -> %u",
                         static_cast<unsigned>(pcm_out_.size()),
                         static_cast<unsigned>(pcm_out_.size() * 2));
                pcm_out_.resize(pcm_out_.size() * 2);
                continue;
            }
            if (ret != ESP_AUDIO_ERR_OK) {
                if (!carry_.empty()) {
                    ESP_LOGD(TAG, "mp3 decode pause ret=%d carry=%u", static_cast<int>(ret),
                             static_cast<unsigned>(carry_.size()));
                }
                break;
            }
            if (out.decoded_size == 0) {
                break;
            }
            const size_t samples = out.decoded_size / sizeof(int16_t);
            const int rate = info.sample_rate > 0 ? static_cast<int>(info.sample_rate) : 24000;
            ++pcm_frame_count_;
            total_pcm_samples_ += samples;
            if (pcm_frame_count_ == 1 || (pcm_frame_count_ % 20) == 0) {
                ESP_LOGI(TAG, "decode frame #%u: %u samples @ %d Hz (mp3 consumed %u)",
                         static_cast<unsigned>(pcm_frame_count_), static_cast<unsigned>(samples), rate,
                         raw.consumed);
            }
            std::vector<int16_t> pcm(pcm_out_.begin(), pcm_out_.begin() + samples);
            Application::GetInstance().GetAudioService().PlayPcm(pcm, rate);
        }
    }

    void* decoder_ = nullptr;
    std::vector<uint8_t> carry_;
    std::vector<int16_t> pcm_out_;
    uint32_t sse_chunk_count_ = 0;
    uint32_t total_mp3_bytes_ = 0;
    uint32_t pcm_frame_count_ = 0;
    uint32_t total_pcm_samples_ = 0;
};

bool DecodeBase64(const std::string& in, std::vector<uint8_t>& out) {
    out.clear();
    if (in.empty()) {
        return true;
    }
    size_t olen = 0;
    const int rc = mbedtls_base64_decode(nullptr, 0, &olen,
                                         reinterpret_cast<const unsigned char*>(in.data()), in.size());
    if (rc != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL && rc != 0) {
        ESP_LOGW(TAG, "base64 size probe failed rc=%d b64_len=%u", rc, static_cast<unsigned>(in.size()));
        return false;
    }
    out.resize(olen);
    if (mbedtls_base64_decode(out.data(), out.size(), &olen,
                              reinterpret_cast<const unsigned char*>(in.data()),
                              in.size()) != 0) {
        ESP_LOGW(TAG, "base64 decode failed b64_len=%u", static_cast<unsigned>(in.size()));
        out.clear();
        return false;
    }
    out.resize(olen);
    return true;
}

bool HandleSseDataLine(const std::string& json_line, Mp3StreamPlayer& player, std::atomic<bool>* abort_flag,
                       uint32_t& sse_event_count) {
    if (abort_flag != nullptr && abort_flag->load()) {
        return false;
    }
    ++sse_event_count;
    cJSON* root = cJSON_Parse(json_line.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "SSE event #%u: invalid JSON (len=%u)", static_cast<unsigned>(sse_event_count),
                 static_cast<unsigned>(json_line.size()));
        return true;
    }
    auto type = cJSON_GetObjectItem(root, "type");
    bool keep_going = true;
    if (cJSON_IsString(type)) {
        if (strcmp(type->valuestring, "chunk") == 0) {
            auto data = cJSON_GetObjectItem(root, "data");
            if (cJSON_IsString(data)) {
                std::vector<uint8_t> mp3;
                if (DecodeBase64(data->valuestring, mp3)) {
                    player.OnSseChunk();
                    if (player.Ok()) {
                        ESP_LOGD(TAG, "SSE chunk #%u: b64=%u -> mp3=%u bytes",
                                 static_cast<unsigned>(sse_event_count),
                                 static_cast<unsigned>(strlen(data->valuestring)),
                                 static_cast<unsigned>(mp3.size()));
                    }
                    player.Feed(mp3.data(), mp3.size());
                } else {
                    ESP_LOGW(TAG, "SSE chunk #%u: base64 decode failed", static_cast<unsigned>(sse_event_count));
                }
            } else {
                ESP_LOGW(TAG, "SSE chunk #%u: missing data field", static_cast<unsigned>(sse_event_count));
            }
        } else if (strcmp(type->valuestring, "done") == 0) {
            ESP_LOGI(TAG, "SSE event #%u: done", static_cast<unsigned>(sse_event_count));
            keep_going = false;
        } else if (strcmp(type->valuestring, "error") == 0) {
            auto msg = cJSON_GetObjectItem(root, "message");
            ESP_LOGE(TAG, "SSE event #%u: error %s", static_cast<unsigned>(sse_event_count),
                     cJSON_IsString(msg) ? msg->valuestring : "unknown");
            keep_going = false;
        } else {
            ESP_LOGD(TAG, "SSE event #%u: type=%s", static_cast<unsigned>(sse_event_count), type->valuestring);
        }
    } else {
        ESP_LOGW(TAG, "SSE event #%u: no type field", static_cast<unsigned>(sse_event_count));
    }
    cJSON_Delete(root);
    return keep_going;
}

void ConsumeSseBuffer(std::string& buffer, Mp3StreamPlayer& player, std::atomic<bool>* abort_flag, bool& done,
                      uint32_t& sse_event_count) {
    size_t pos = 0;
    while (!done) {
        size_t line_end = buffer.find('\n', pos);
        if (line_end == std::string::npos) {
            buffer.erase(0, pos);
            return;
        }
        std::string line = buffer.substr(pos, line_end - pos);
        pos = line_end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.rfind("data:", 0) == 0) {
            std::string payload = line.substr(5);
            while (!payload.empty() && payload.front() == ' ') {
                payload.erase(payload.begin());
            }
            if (!payload.empty() && !HandleSseDataLine(payload, player, abort_flag, sse_event_count)) {
                done = true;
            }
        }
    }
    buffer.clear();
}

std::unique_ptr<Mp3StreamPlayer> g_ws_mp3_player;

void ResetMp3Player() {
    g_ws_mp3_player = std::make_unique<Mp3StreamPlayer>();
    if (g_ws_mp3_player != nullptr && !g_ws_mp3_player->Ok()) {
        ESP_LOGE(TAG, "MP3 decoder init failed");
        g_ws_mp3_player.reset();
    }
}

void FeedMp3Player(const uint8_t* data, size_t len) {
    if (g_ws_mp3_player != nullptr && data != nullptr && len > 0) {
        g_ws_mp3_player->Feed(data, len);
    }
}

void FlushMp3Player() {
    if (g_ws_mp3_player != nullptr) {
        g_ws_mp3_player->Flush();
    }
}

void WaitMp3PlayerDone() {
    if (g_ws_mp3_player != nullptr) {
        g_ws_mp3_player->Flush();
    }
    Application::GetInstance().GetAudioService().WaitForPlaybackQueueEmpty();
    g_ws_mp3_player.reset();
}

}  // namespace

void ResetMp3Playback() {
    ResetMp3Player();
}

void FeedMp3Playback(const uint8_t* data, size_t len) {
    FeedMp3Player(data, len);
}

void FlushMp3Playback() {
    FlushMp3Player();
}

void WaitForMp3PlaybackDone() {
    WaitMp3PlayerDone();
}

esp_err_t StreamTts(const std::string& text, std::atomic<bool>* abort_flag) {
    const int64_t t0_us = esp_timer_get_time();
    ESP_LOGI(TAG, "======== TTS begin ========");
    LogTextPreview("request text", text);

    if (text.empty()) {
        ESP_LOGE(TAG, "TTS: empty text");
        return ESP_ERR_INVALID_ARG;
    }
    if (!HasAccessToken()) {
        ESP_LOGE(TAG, "TTS: no access token");
        return ESP_ERR_INVALID_STATE;
    }

    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    auto http = NewAuthedHttp(0);
    if (http == nullptr) {
        ESP_LOGE(TAG, "TTS: NewAuthedHttp failed");
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        return ESP_ERR_INVALID_STATE;
    }
    http->SetTimeout(kTtsHttpTimeoutMs);
    http->SetHeader("Accept", "text/event-stream");

    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "text", text.c_str());
    char* printed = cJSON_PrintUnformatted(body);
    std::string content = printed ? printed : "";
    cJSON_free(printed);
    cJSON_Delete(body);
    http->SetContent(std::move(content));

    const std::string url = BuildUrl("/tts/stream");
    ESP_LOGI(TAG, "POST %s body_bytes=%u timeout_ms=%d", url.c_str(),
             static_cast<unsigned>(content.size()), kTtsHttpTimeoutMs);
    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG, "TTS: HTTP Open failed");
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        return ESP_FAIL;
    }

    const int status = http->GetStatusCode();
    const int64_t t_headers_us = esp_timer_get_time();
    ESP_LOGI(TAG, "TTS: HTTP status=%d headers_ms=%lld", status,
             static_cast<long long>((t_headers_us - t0_us) / 1000));
    if (status < 200 || status >= 300) {
        http->Close();
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        return ESP_FAIL;
    }

    Mp3StreamPlayer player;
    if (!player.Ok()) {
        ESP_LOGE(TAG, "TTS: MP3 decoder init failed");
        http->Close();
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        return ESP_FAIL;
    }

    std::string sse_buffer;
    char read_buf[kHttpReadChunk];
    bool done = false;
    uint32_t sse_event_count = 0;
    uint32_t read_rounds = 0;
    size_t total_http_bytes = 0;
    while (!done) {
        if (abort_flag != nullptr && abort_flag->load()) {
            ESP_LOGI(TAG, "TTS: aborted by user after %u read rounds, %u bytes",
                     static_cast<unsigned>(read_rounds), static_cast<unsigned>(total_http_bytes));
            break;
        }
        const int n = http->Read(read_buf, sizeof(read_buf));
        if (n < 0) {
            ESP_LOGE(TAG, "TTS: HTTP Read error at round %u", static_cast<unsigned>(read_rounds));
            http->Close();
            Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
            return ESP_FAIL;
        }
        if (n == 0) {
            ESP_LOGI(TAG, "TTS: HTTP body EOF after %u rounds, %u bytes",
                     static_cast<unsigned>(read_rounds), static_cast<unsigned>(total_http_bytes));
            break;
        }
        ++read_rounds;
        total_http_bytes += static_cast<size_t>(n);
        sse_buffer.append(read_buf, static_cast<size_t>(n));
        ConsumeSseBuffer(sse_buffer, player, abort_flag, done, sse_event_count);
    }

    player.Flush();
    http->Close();
    const int64_t t_stream_us = esp_timer_get_time();
    ESP_LOGI(TAG, "TTS: SSE done events=%u http_bytes=%u stream_ms=%lld", static_cast<unsigned>(sse_event_count),
             static_cast<unsigned>(total_http_bytes),
             static_cast<long long>((t_stream_us - t_headers_us) / 1000));

    ESP_LOGI(TAG, "TTS: waiting playback queue empty");
    Application::GetInstance().GetAudioService().WaitForPlaybackQueueEmpty();
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

    const int64_t t_end_us = esp_timer_get_time();
    if (abort_flag != nullptr && abort_flag->load()) {
        ESP_LOGW(TAG, "======== TTS aborted total_ms=%lld ========",
                 static_cast<long long>((t_end_us - t0_us) / 1000));
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "======== TTS ok total_ms=%lld (headers=%lld stream=%lld play=%lld) ========",
             static_cast<long long>((t_end_us - t0_us) / 1000),
             static_cast<long long>((t_headers_us - t0_us) / 1000),
             static_cast<long long>((t_stream_us - t_headers_us) / 1000),
             static_cast<long long>((t_end_us - t_stream_us) / 1000));
    return ESP_OK;
}

}  // namespace oye
