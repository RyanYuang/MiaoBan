#include "sticker_chat_page_presenter.h"

#include "application.h"
#include "display.h"
#include "ui_command_dispatcher.h"
#include "ui_page_router.h"

#include <cstdint>
#include <lvgl.h>
#include <string>

namespace ui::sticker_chat {

namespace {

constexpr uintptr_t kUserDataBack = 0x4241434Bu;
constexpr uintptr_t kUserDataSticker = 0x53544B52u;

}  // namespace

StickerChatPagePresenter::~StickerChatPagePresenter()
{
    if (listener_id_ >= 0) {
        Application::GetInstance().RemoveDeviceStateChangeListener(listener_id_);
        listener_id_ = -1;
    }
    if (recognition_listener_id_ >= 0) {
        Application::GetInstance().RemoveRecognitionTextListener(recognition_listener_id_);
        recognition_listener_id_ = -1;
    }
}

void StickerChatPagePresenter::AttachStateUi(Display* display, bool* page_alive, void* hint_label, void* transcript_label)
{
    display_ = display;
    page_alive_ = page_alive;
    hint_label_ = hint_label;
    transcript_label_ = transcript_label;
    if (listener_id_ >= 0 || recognition_listener_id_ >= 0 ||
        display_ == nullptr || page_alive_ == nullptr || hint_label_ == nullptr || transcript_label_ == nullptr) {
        return;
    }
    listener_id_ = Application::GetInstance().AddDeviceStateChangeListener(
        [this](DeviceState old_state, DeviceState new_state) { OnDeviceState(old_state, new_state); });
    recognition_listener_id_ = Application::GetInstance().AddRecognitionTextListener(
        [this](const std::string& text) { OnRecognitionText(text); });
}

void StickerChatPagePresenter::OnDeviceState(DeviceState old_state, DeviceState new_state)
{
    const char* text = nullptr;
    bool clear_transcript = false;
    if (old_state == kDeviceStateListening && new_state == kDeviceStateIdle) {
        text = "点击按钮与大模型对话";
    } else if (new_state == kDeviceStateListening) {
        text = "聆听中… 再次点击按钮结束";
        clear_transcript = true;
    } else if (new_state == kDeviceStateConnecting) {
        clear_transcript = true;
    }

    if (text == nullptr && !clear_transcript) {
        return;
    }

    Display* disp = display_;
    bool* alive = page_alive_;
    lv_obj_t* hint = static_cast<lv_obj_t*>(hint_label_);
    lv_obj_t* transcript = static_cast<lv_obj_t*>(transcript_label_);
    if (disp == nullptr || alive == nullptr || hint == nullptr || transcript == nullptr) {
        return;
    }

    std::string copy = text != nullptr ? text : "";
    UiCommandDispatcher::Instance().Post([disp, alive, hint, transcript, clear_transcript, copy = std::move(copy)]() {
        if (alive == nullptr || !*alive) {
            return;
        }
        DisplayLockGuard lock(disp);
        if (!copy.empty()) {
            lv_label_set_text(hint, copy.c_str());
        }
        if (clear_transcript) {
            lv_label_set_text(transcript, "");
        }
    });
}

void StickerChatPagePresenter::OnRecognitionText(const std::string& text)
{
    Display* disp = display_;
    bool* alive = page_alive_;
    lv_obj_t* transcript = static_cast<lv_obj_t*>(transcript_label_);
    if (disp == nullptr || alive == nullptr || transcript == nullptr) {
        return;
    }

    UiCommandDispatcher::Instance().Post([disp, alive, transcript, text]() {
        if (alive == nullptr || !*alive) {
            return;
        }
        DisplayLockGuard lock(disp);
        lv_label_set_text(transcript, text.c_str());
    });
}

void StickerChatPagePresenter::OnClick(lv_event_t* e)
{
    if (e == nullptr) {
        return;
    }
    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const uintptr_t ud = reinterpret_cast<uintptr_t>(lv_obj_get_user_data(target));

    if (ud == kUserDataBack) {
        UiPageRouter::Instance().PostNavigateTo(UI_PAGE_ID(Settings));
        return;
    }
    if (ud == kUserDataSticker) {
        UiCommandDispatcher::Instance().Post([]() { Application::GetInstance().ToggleChatState(); });
        return;
    }
}

}  // namespace ui::sticker_chat
