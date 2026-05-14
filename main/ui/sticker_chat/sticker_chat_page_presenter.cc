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
}

void StickerChatPagePresenter::AttachStateHints(Display* display, bool* page_alive, void* hint_label)
{
    display_ = display;
    page_alive_ = page_alive;
    hint_label_ = hint_label;
    if (listener_id_ >= 0 || display_ == nullptr || page_alive_ == nullptr || hint_label_ == nullptr) {
        return;
    }
    listener_id_ = Application::GetInstance().AddDeviceStateChangeListener(
        [this](DeviceState old_state, DeviceState new_state) { OnDeviceState(old_state, new_state); });
}

void StickerChatPagePresenter::OnDeviceState(DeviceState old_state, DeviceState new_state)
{
    const char* text = nullptr;
    if (old_state == kDeviceStateListening && new_state == kDeviceStateIdle) {
        text = "点击按钮与大模型对话";
    } else if (new_state == kDeviceStateListening) {
        text = "聆听中… 再次点击按钮结束";
    }

    if (text == nullptr) {
        return;
    }

    Display* disp = display_;
    bool* alive = page_alive_;
    lv_obj_t* hint = static_cast<lv_obj_t*>(hint_label_);
    if (disp == nullptr || alive == nullptr || hint == nullptr) {
        return;
    }

    std::string copy(text);
    UiCommandDispatcher::Instance().Post([disp, alive, hint, copy = std::move(copy)]() {
        if (alive == nullptr || !*alive) {
            return;
        }
        DisplayLockGuard lock(disp);
        lv_label_set_text(hint, copy.c_str());
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
        UiPageRouter::Instance().PostNavigateBack();
        return;
    }
    if (ud == kUserDataSticker) {
        UiCommandDispatcher::Instance().Post([]() { Application::GetInstance().ToggleChatState(); });
        return;
    }
}

}  // namespace ui::sticker_chat
