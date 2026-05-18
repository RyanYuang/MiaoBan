#ifndef OYE_AUDIO_UTIL_H
#define OYE_AUDIO_UTIL_H

#include <cstdint>
#include <vector>

namespace oye {

/** Record mono 16 kHz PCM from microphone for duration_ms (blocking). */
bool RecordPcmMs(int duration_ms, std::vector<int16_t>& pcm_out);

}  // namespace oye

#endif
