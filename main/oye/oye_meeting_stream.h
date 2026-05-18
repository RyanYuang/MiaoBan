#ifndef OYE_MEETING_STREAM_H
#define OYE_MEETING_STREAM_H

class WebSocket;

#include <functional>
#include <mutex>
#include <string>

namespace oye {

/** Real-time ASR over /meetings/stream/ws (PCM 16k mono s16le). */
class MeetingStream {
public:
    using TextCallback = std::function<void(const std::string& text)>;
    using DoneCallback = std::function<void(const std::string& text, int meeting_id)>;

    bool Start(const std::string& title, bool save_meeting, TextCallback on_asr, DoneCallback on_done);
    void FeedPcm(const int16_t* samples, size_t count);
    void Stop();
    bool IsRunning() const { return running_; }
    std::string LatestText() const;

private:
    bool running_ = false;
    bool save_meeting_ = false;
    std::string latest_text_;
    mutable std::mutex mutex_;
    TextCallback on_asr_;
    DoneCallback on_done_;
    WebSocket* websocket_ = nullptr;

    static std::string BuildWsUrl();
    void HandleText(const char* data, size_t len);
};

}  // namespace oye

#endif
