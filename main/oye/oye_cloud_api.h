#ifndef OYE_CLOUD_API_H
#define OYE_CLOUD_API_H

#include <esp_err.h>
#include <string>
#include <vector>

namespace oye {

struct UserInfo {
    int id = 0;
    std::string phone;
    std::string nickname;
};

struct ChatMessageResult {
    std::string user_text;
    std::string assistant_text;
};

struct MeetingInfo {
    int id = 0;
    std::string title;
    std::string status;
    std::string summary;
    std::string transcript_text;
    std::string error_message;
};

struct VoiceprintVerifyResult {
    bool matched = false;
    float score = 0.f;
    float threshold = 0.75f;
};

esp_err_t GetCurrentUser(UserInfo& out);
esp_err_t EnsureChatSession(int& session_id);
esp_err_t SendChatMessage(int session_id, const std::string& user_text, ChatMessageResult& out);

esp_err_t UploadMeetingAudio(const uint8_t* wav_data, size_t wav_size, const std::string& title,
                             int& meeting_id);
esp_err_t SubmitMeetingTranscript(const std::string& title, const std::string& transcript,
                                  int& meeting_id);
esp_err_t ListMeetings(int page, int page_size, std::vector<MeetingInfo>& out);
esp_err_t GetMeeting(int meeting_id, MeetingInfo& out);
esp_err_t PollMeetingUntilDone(int meeting_id, MeetingInfo& out, int timeout_sec = 300);

esp_err_t RegisterMyVoiceprint(const uint8_t* wav_data, size_t wav_size);
esp_err_t VerifyMyVoiceprint(const uint8_t* wav_data, size_t wav_size, VoiceprintVerifyResult& out);

/** Build 16kHz mono 16-bit WAV in buffer. */
bool BuildWavFromPcm(const std::vector<int16_t>& pcm, std::vector<uint8_t>& wav_out);

}  // namespace oye

#endif
