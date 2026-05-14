#include "lvgl_sticker_chat_page_view.h"

#include "assets.h"
#include "display.h"
#include "lvgl_page_touch_presenter.h"
#include "lvgl_theme.h"
#include "sticker_chat_page_presenter.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace ui::sticker_chat {

namespace {

constexpr uintptr_t kUserDataBack = 0x4241434Bu;
constexpr uintptr_t kUserDataSticker = 0x53544B52u;

struct StickerChatMvpBundle {
    bool* page_alive = nullptr;
    StickerChatPagePresenter* presenter = nullptr;
    LvglStickerChatPageView* view = nullptr;
};

static void StickerChatRootOnDelete(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    lv_obj_t* root = static_cast<lv_obj_t*>(lv_event_get_target(e));
    auto* bundle = static_cast<StickerChatMvpBundle*>(lv_obj_get_user_data(root));
    if (bundle == nullptr) {
        return;
    }
    if (bundle->page_alive != nullptr) {
        *bundle->page_alive = false;
    }
    delete bundle->presenter;
    delete bundle->view;
    if (bundle->page_alive != nullptr) {
        delete bundle->page_alive;
    }
    delete bundle;
    lv_obj_set_user_data(root, nullptr);
}

}  // namespace

constexpr char TAG[] = "LvglStickerChat";

void* LvglStickerChatPageView::CreateRouterPageRoot(Display* display, LvglTheme* theme)
{
    if (display == nullptr || theme == nullptr) {
        return nullptr;
    }
    auto* view = new LvglStickerChatPageView(display, theme);
    auto* presenter = new StickerChatPagePresenter();
    bool* page_alive = new bool(true);
    view->SetPageAliveFlag(page_alive);
    view->BindTouchPresenter(presenter);
    view->BuildLayout();

    lv_obj_t* root = static_cast<lv_obj_t*>(view->RootHandle());
    if (root == nullptr) {
        delete presenter;
        delete view;
        delete page_alive;
        return nullptr;
    }

    auto* bundle = new StickerChatMvpBundle{page_alive, presenter, view};
    lv_obj_set_user_data(root, bundle);
    lv_obj_add_event_cb(root, StickerChatRootOnDelete, LV_EVENT_DELETE, nullptr);
    return root;
}

LvglStickerChatPageView::LvglStickerChatPageView(Display* display, LvglTheme* theme)
    : display_(display), theme_(theme) {}

LvglStickerChatPageView::~LvglStickerChatPageView()
{
    root_ = nullptr;
}

void LvglStickerChatPageView::BuildLayout()
{
    if (display_ == nullptr || theme_ == nullptr || root_ != nullptr) {
        return;
    }

    lv_obj_t* screen = lv_screen_active();
    lv_obj_t* panel = lv_obj_create(screen);
    lv_obj_set_size(panel, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(panel, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(panel, theme_->chat_background_color(), 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* back_lbl = lv_label_create(panel);
    lv_label_set_text(back_lbl, "返回");
    lv_obj_set_style_text_font(back_lbl, theme_->text_font()->font(), 0);
    lv_obj_set_style_text_color(back_lbl, theme_->text_color(), 0);
    lv_obj_align(back_lbl, LV_ALIGN_TOP_LEFT, 12, 12);
    lv_obj_add_flag(back_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(back_lbl, reinterpret_cast<void*>(kUserDataBack));

    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, "对话");
    lv_obj_set_style_text_font(title, theme_->text_font()->font(), 0);
    lv_obj_set_style_text_color(title, theme_->text_color(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t* hint = lv_label_create(panel);
    lv_label_set_text(hint, "点击按钮与大模型对话");
    lv_obj_set_style_text_font(hint, theme_->text_font()->font(), 0);
    lv_obj_set_style_text_color(hint, theme_->text_color(), 0);
    lv_obj_set_style_text_opa(hint, LV_OPA_70, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -32);

    if (Assets::GetInstance().partition_valid()) {
        void* asset_ptr = nullptr;
        size_t asset_size = 0;
        if (Assets::GetInstance().GetAssetData("Chat_btn.png", asset_ptr, asset_size) && asset_ptr != nullptr && asset_size > 0) {
            auto* heap_copy = static_cast<uint8_t*>(heap_caps_malloc(asset_size, MALLOC_CAP_8BIT));
            if (heap_copy != nullptr) {
                memcpy(heap_copy, asset_ptr, asset_size);
                try {
                    sticker_image_ = std::make_unique<LvglAllocatedImage>(heap_copy, asset_size);
                    lv_obj_t* img = lv_image_create(panel);
                    lv_image_set_src(img, sticker_image_->image_dsc());
                    lv_obj_set_size(img, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
                    lv_obj_align(img, LV_ALIGN_CENTER, 0, 8);
                    lv_obj_add_flag(img, LV_OBJ_FLAG_CLICKABLE);
                    lv_obj_set_user_data(img, reinterpret_cast<void*>(kUserDataSticker));
                    if (touch_presenter_ != nullptr) {
                        ui::mvp::LvglPageAttachTouchHandlers(img, touch_presenter_);
                    }
                } catch (const std::runtime_error& e) {
                    ESP_LOGW(TAG, "Chat_btn.png decode failed: %s", e.what());
                    heap_caps_free(heap_copy);
                }
            }
        } else {
            ESP_LOGW(TAG, "Chat_btn.png not in assets");
        }
    }

    if (touch_presenter_ != nullptr && page_alive_ != nullptr) {
        touch_presenter_->AttachStateHints(display_, page_alive_, hint);
    }

    if (touch_presenter_ != nullptr) {
        ui::mvp::LvglPageAttachTouchHandlers(panel, touch_presenter_);
        ui::mvp::LvglPageAttachTouchHandlers(back_lbl, touch_presenter_);
        ui::mvp::LvglPageAttachTouchHandlers(title, touch_presenter_);
        ui::mvp::LvglPageAttachTouchHandlers(hint, touch_presenter_);
    }

    root_ = panel;
}

}  // namespace ui::sticker_chat
