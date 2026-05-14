#pragma once

#include "device_state.h"
#include "lvgl_page_touch_presenter.h"

class Display;

namespace ui::sticker_chat {

/**
 * 贴图对话页：返回仅出栈；点击 Chat_btn 只 ToggleChatState（不关闭叠页）。
 * 底部提示文案随状态机更新（如 listening→idle 恢复「点击按钮…」）。
 */
class StickerChatPagePresenter final : public ui::mvp::LvglPageTouchPresenter {
public:
    StickerChatPagePresenter() = default;
    ~StickerChatPagePresenter() override;

    void OnClick(lv_event_t* e) override;

    /** 在 BuildLayout 末尾调用：`page_alive` 在根 DELETE 前置 false，避免异步改已删控件。 */
    void AttachStateHints(Display* display, bool* page_alive, void* hint_label);

private:
    void OnDeviceState(DeviceState old_state, DeviceState new_state);

    Display* display_ = nullptr;
    bool* page_alive_ = nullptr;
    void* hint_label_ = nullptr;
    int listener_id_ = -1;
};

}  // namespace ui::sticker_chat
