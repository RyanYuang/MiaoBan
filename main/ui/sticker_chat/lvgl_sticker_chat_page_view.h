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

    /** 供根节点 bundle 在 DELETE 时释放；由 BuildLayout 内创建。 */
    void* quick_settings_ctx() const { return quick_settings_ctx_; }

    void BindTouchPresenter(StickerChatPagePresenter* p) { touch_presenter_ = p; }

    /** 在 `CreateRouterPageRoot` 里于 `BuildLayout` 前设置，用于生命周期与状态监听。 */
    void SetPageAliveFlag(bool* page_alive) { page_alive_ = page_alive; }

private:
    Display* display_;
    LvglTheme* theme_;
    void* root_ = nullptr;
    bool* page_alive_ = nullptr;
    StickerChatPagePresenter* touch_presenter_ = nullptr;
    void* quick_settings_ctx_ = nullptr;
    std::unique_ptr<LvglAllocatedImage> sticker_image_;
};

}  // namespace ui::sticker_chat
