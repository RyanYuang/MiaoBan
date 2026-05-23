#ifndef OYE_TTS_STREAM_H
#define OYE_TTS_STREAM_H

#include <atomic>
#include <esp_err.h>
#include <string>

namespace oye {

/** Stream TTS from POST /mcu/tts/stream (SSE MP3 chunks) and play on device speaker. */
esp_err_t StreamTts(const std::string& text, std::atomic<bool>* abort_flag = nullptr);

/** MP3 playback helpers for WS tts_chunk (voice-chat). */
void ResetMp3Playback();
void FeedMp3Playback(const uint8_t* data, size_t len);
void FlushMp3Playback();
void WaitForMp3PlaybackDone();

}  // namespace oye

#endif
