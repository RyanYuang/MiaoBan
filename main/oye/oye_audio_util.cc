#include "oye_audio_util.h"

#include "application.h"
#include "audio_service.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace oye {

bool RecordPcmMs(int duration_ms, std::vector<int16_t>& pcm_out) {
    auto& audio = Application::GetInstance().GetAudioService();
    const bool was_processing = audio.IsAudioProcessorRunning();
    if (!was_processing) {
        audio.EnableVoiceProcessing(true);
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    pcm_out.clear();
    const int samples_per_read = 160;
    const int total_reads = (duration_ms * 16000) / (1000 * samples_per_read);
    for (int i = 0; i < total_reads; ++i) {
        std::vector<int16_t> chunk;
        if (audio.ReadAudioData(chunk, 16000, samples_per_read)) {
            pcm_out.insert(pcm_out.end(), chunk.begin(), chunk.end());
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (!was_processing) {
        audio.EnableVoiceProcessing(false);
    }
    return pcm_out.size() >= static_cast<size_t>(samples_per_read);
}

}  // namespace oye
