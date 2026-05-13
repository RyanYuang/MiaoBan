#pragma once

#include "lvgl_page_touch_presenter.h"

namespace ui::sticker_chat {

/** 贴图对话页：返回关闭叠层；点击 Chat_btn 先出栈再 ToggleChatState（连上协议与大模型对话）。 */
class StickerChatPagePresenter final : public ui::mvp::LvglPageTouchPresenter {
public:
    StickerChatPagePresenter() = default;

    void OnClick(lv_event_t* e) override;
};

}  // namespace ui::sticker_chat
