#pragma once

#include <lvgl.h>
#include <memory>

#include "lvgl_image.h"

class Display;
class LvglTheme;

namespace ui::sticker_chat {

class StickerChatPagePresenter;

/** 全屏贴图 + 提示；点击按钮走 ToggleChatState 与大模型语音对话（见 Presenter）。 */
class LvglStickerChatPageView {
public:
    static void* CreateRouterPageRoot(Display* display, LvglTheme* theme);

    LvglStickerChatPageView(Display* display, LvglTheme* theme);
    ~LvglStickerChatPageView();

    void BuildLayout();

    void* RootHandle() const { return root_; }

    void BindTouchPresenter(StickerChatPagePresenter* p) { touch_presenter_ = p; }

private:
    Display* display_;
    LvglTheme* theme_;
    void* root_ = nullptr;
    StickerChatPagePresenter* touch_presenter_ = nullptr;
    std::unique_ptr<LvglAllocatedImage> sticker_image_;
};

}  // namespace ui::sticker_chat
