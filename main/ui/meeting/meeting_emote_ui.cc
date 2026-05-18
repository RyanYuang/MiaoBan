#include "meeting_emote_ui.h"

#include "display.h"
#include "ui_command_dispatcher.h"
#include "ui_page_router.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#if CONFIG_USE_OYE_CLOUD_API
#include "oye/oye_audio_util.h"
#include "oye/oye_cloud_api.h"
#include "oye/oye_config.h"
#endif

namespace ui::meeting {

namespace {
constexpr char TAG[] = "MeetingEmote";
constexpr int kRecordMs = 8000;
}  // namespace

MeetingEmoteUi& MeetingEmoteUi::Instance() {
    static MeetingEmoteUi ui;
    return ui;
}

void MeetingEmoteUi::Enter(Display* display) {
#if !CONFIG_USE_OYE_CLOUD_API
    (void)display;
    return;
#endif
    display_ = display;
    active_ = true;
    ShowMessage("system", "会议纪要\n单击刷新\n双击退出\n长按录音上传");
    RefreshListAsync();
}

void MeetingEmoteUi::Exit() {
    active_ = false;
    busy_ = false;
    if (display_ != nullptr) {
        display_->ShowNotification("已退出会议纪要", 2000);
    }
    display_ = nullptr;
}

void MeetingEmoteUi::OnShortTap() {
#if CONFIG_USE_OYE_CLOUD_API
    if (!active_ || busy_) {
        return;
    }
    const uint32_t now = static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (last_tap_ms_ != 0 && (now - last_tap_ms_) < 600) {
        last_tap_ms_ = 0;
        Exit();
        UiPageRouter::Instance().PostNavigateBack();
        return;
    }
    last_tap_ms_ = now;
    RefreshListAsync();
#endif
}

void MeetingEmoteUi::ShowMessage(const char* role, const std::string& text) {
    if (display_ != nullptr) {
        display_->SetChatMessage(role, text.c_str());
    }
}

void MeetingEmoteUi::RefreshListAsync() {
#if CONFIG_USE_OYE_CLOUD_API
    if (!active_ || display_ == nullptr) {
        return;
    }
    if (!oye::HasAccessToken()) {
        ShowMessage("system", "请先在 App 中绑定账号");
        return;
    }
    SetBusy(true);
    display_->ShowNotification("加载会议…", 3000);

    xTaskCreate(
        [](void*) {
            std::vector<oye::MeetingInfo> items;
            esp_err_t err = oye::ListMeetings(1, 1, items);

            UiCommandDispatcher::Instance().Post([err, items = std::move(items)]() mutable {
                auto& ui = MeetingEmoteUi::Instance();
                ui.SetBusy(false);
                if (!ui.active_) {
                    return;
                }
                if (err != ESP_OK) {
                    ui.ShowMessage("system", "会议列表加载失败");
                    return;
                }
                std::string text = "会议纪要 (" + std::to_string(items.size()) + ")\n";
                int shown = 0;
                for (const auto& m : items) {
                    if (shown >= 4) {
                        text += "…\n";
                        break;
                    }
                    text += std::to_string(m.id) + ". ";
                    text += m.title.empty() ? "未命名" : m.title;
                    text += " [";
                    text += m.status;
                    text += "]\n";
                    ++shown;
                }
                if (items.empty()) {
                    text += "暂无记录，长按录音上传";
                } else {
                    text += "\n长按 8 秒录音并上传";
                }
                ui.ShowMessage("system", text);
            });
            vTaskDelete(nullptr);
        },
        "oye_meeting_emote", 4096, nullptr, 5, nullptr);
#endif
}

#if CONFIG_USE_OYE_CLOUD_API
static void RecordAndUploadEmote(Display* display) {
    if (display == nullptr) {
        return;
    }
    display->ShowNotification("录音 8 秒…", kRecordMs + 500);

    xTaskCreate(
        [](void* arg) {
            auto* disp = static_cast<Display*>(arg);
            std::vector<int16_t> pcm;
            if (!oye::RecordPcmMs(kRecordMs, pcm)) {
                UiCommandDispatcher::Instance().Post([disp]() {
                    MeetingEmoteUi::Instance().SetBusy(false);
                    disp->ShowNotification("录音失败", 3000);
                });
                vTaskDelete(nullptr);
                return;
            }
            std::vector<uint8_t> wav;
            if (!oye::BuildWavFromPcm(pcm, wav)) {
                UiCommandDispatcher::Instance().Post([disp]() {
                    MeetingEmoteUi::Instance().SetBusy(false);
                    disp->ShowNotification("编码失败", 3000);
                });
                vTaskDelete(nullptr);
                return;
            }
            int meeting_id = 0;
            if (oye::UploadMeetingAudio(wav.data(), wav.size(), "设备录音", meeting_id) != ESP_OK) {
                UiCommandDispatcher::Instance().Post([disp]() {
                    MeetingEmoteUi::Instance().SetBusy(false);
                    disp->ShowNotification("上传失败", 3000);
                });
                vTaskDelete(nullptr);
                return;
            }
            oye::MeetingInfo info;
            esp_err_t poll = oye::PollMeetingUntilDone(meeting_id, info, 300);
            UiCommandDispatcher::Instance().Post([disp, poll, info = std::move(info)]() mutable {
                auto& ui = MeetingEmoteUi::Instance();
                ui.SetBusy(false);
                if (poll != ESP_OK) {
                    disp->ShowNotification("纪要生成失败", 4000);
                    return;
                }
                std::string msg = "纪要完成\n";
                msg += info.summary.empty() ? "(无摘要)" : info.summary;
                disp->SetChatMessage("system", msg.c_str());
                disp->ShowNotification("上传完成", 3000);
                ui.RefreshListAsync();
            });
            vTaskDelete(nullptr);
        },
        "oye_meeting_rec", 8192, display, 5, nullptr);
}
#endif

void MeetingEmoteUi::StartRecordUpload() {
#if CONFIG_USE_OYE_CLOUD_API
    if (display_ == nullptr || busy_) {
        return;
    }
    SetBusy(true);
    RecordAndUploadEmote(display_);
#endif
}

}  // namespace ui::meeting
