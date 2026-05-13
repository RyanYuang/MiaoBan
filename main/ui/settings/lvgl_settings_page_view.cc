#include "lvgl_settings_page_view.h"

#include "assets.h"
#include "display.h"
#include "lvgl_image.h"
#include "lvgl_page_touch_presenter.h"
#include "lvgl_theme.h"
#include "settings_page_model.h"
#include "settings_page_presenter.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <cstdint>
#include <cstring>
#include <stdexcept>

#define TAG "LvglSettings"

namespace ui::settings {

namespace {

/** 与 `settings_page_presenter.cc` 中一致：点击该 label 进入贴图对话页。 */
constexpr uintptr_t kUserDataOpenStickerChat = 0x53544348u;  // 'STCH'

/** 挂在根节点 user_data 上，根 DELETE 时一并释放 Presenter 与 View。 */
struct SettingsMvpBundle {
    SettingsPagePresenter* presenter = nullptr;
    LvglSettingsPageView* view = nullptr;
};

/** 根节点 LV_EVENT_DELETE：释放 MVP bundle，避免悬空指针与泄漏。 */
static void SettingsRootOnDelete(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    lv_obj_t* root = static_cast<lv_obj_t*>(lv_event_get_target(e));
    auto* bundle = static_cast<SettingsMvpBundle*>(lv_obj_get_user_data(root));
    if (bundle == nullptr) {
        return;
    }
    delete bundle->presenter;
    delete bundle->view;
    delete bundle;
    lv_obj_set_user_data(root, nullptr);
}

}  // namespace

/** 创建供 UiPageRouter 压栈的根节点：new View/Presenter、首次 Show、根上挂 bundle 与 DELETE 回调。调用方须已持有 Display 锁。 */
void* LvglSettingsPageView::CreateRouterPageRoot(Display* display, LvglTheme* theme)
{
    if (display == nullptr || theme == nullptr) {
        return nullptr;
    }
    auto* view = new LvglSettingsPageView(display, theme);
    auto* presenter = new SettingsPagePresenter(view);
    view->BindTouchPresenter(presenter);
    SettingsPageModel model;
    model.title = "Settings";
    presenter->Show(model);

    lv_obj_t* root = static_cast<lv_obj_t*>(view->RootHandle());
    if (root == nullptr) {
        delete presenter;
        delete view;
        return nullptr;
    }

    auto* bundle = new SettingsMvpBundle{presenter, view};
    lv_obj_set_user_data(root, bundle);
    lv_obj_add_event_cb(root, SettingsRootOnDelete, LV_EVENT_DELETE, nullptr);
    return root;
}

/** 仅保存 Display 与主题指针；LVGL 控件延后到 BuildLayout 创建。 */
LvglSettingsPageView::LvglSettingsPageView(Display* display, LvglTheme* theme)
    : display_(display), theme_(theme) {}

/** 根对象由 LVGL 销毁时不再 lv_obj_del；只清空成员指针。 */
LvglSettingsPageView::~LvglSettingsPageView() {
    root_ = nullptr;
}

/** 创建全屏 panel、标题 label、样式与点击回调；已存在 root_ 时直接返回。 */
void LvglSettingsPageView::BuildLayout(const SettingsPageModel& model)
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
    lv_obj_set_style_bg_color(panel, theme_->background_color(), 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* label = lv_label_create(panel);
    lv_label_set_text(label, model.title.empty() ? "Settings" : model.title.c_str());
    lv_obj_set_style_text_font(label, theme_->text_font()->font(), 0);
    lv_obj_set_style_text_color(label, theme_->text_color(), 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_add_flag(label, LV_OBJ_FLAG_CLICKABLE);

    if (Assets::GetInstance().partition_valid()) {
        void* asset_ptr = nullptr;
        size_t asset_size = 0;
        if (Assets::GetInstance().GetAssetData("Music_Btn.png", asset_ptr, asset_size) && asset_ptr != nullptr && asset_size > 0) {
            auto* heap_copy = static_cast<uint8_t*>(heap_caps_malloc(asset_size, MALLOC_CAP_8BIT));
            if (heap_copy != nullptr) {
                memcpy(heap_copy, asset_ptr, asset_size);
                try {
                    music_btn_image_ = std::make_unique<LvglAllocatedImage>(heap_copy, asset_size);
                    lv_obj_t* img = lv_image_create(panel);
                    lv_image_set_src(img, music_btn_image_->image_dsc());
                    lv_obj_set_size(img, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
                    lv_obj_align(img, LV_ALIGN_CENTER, 0, 32);
                    lv_obj_add_flag(img, LV_OBJ_FLAG_CLICKABLE);
                    if (touch_presenter_ != nullptr) {
                        ui::mvp::LvglPageAttachTouchHandlers(img, touch_presenter_);
                    }
                } catch (const std::runtime_error& e) {
                    ESP_LOGW(TAG, "Music_Btn.png decode failed: %s", e.what());
                    heap_caps_free(heap_copy);
                }
            }
        } else {
            ESP_LOGW(TAG, "Music_Btn.png not in assets (rebuild default assets & flash assets partition)");
        }
    }

    lv_obj_t* sticker_entry = lv_label_create(panel);
    lv_label_set_text(sticker_entry, "贴图对话");
    lv_obj_set_style_text_font(sticker_entry, theme_->text_font()->font(), 0);
    lv_obj_set_style_text_color(sticker_entry, theme_->text_color(), 0);
    lv_obj_align(sticker_entry, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_add_flag(sticker_entry, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(sticker_entry, reinterpret_cast<void*>(kUserDataOpenStickerChat));

    if (touch_presenter_ != nullptr) {
        ui::mvp::LvglPageAttachTouchHandlers(panel, touch_presenter_);
        ui::mvp::LvglPageAttachTouchHandlers(label, touch_presenter_);
        ui::mvp::LvglPageAttachTouchHandlers(sticker_entry, touch_presenter_);
    }

    root_ = panel;
}

/** ISettingsPageView 接口：根据 model 触发一次性布局搭建。 */
void LvglSettingsPageView::Show(const SettingsPageModel& model) {
    BuildLayout(model);
}

}  // namespace ui::settings
