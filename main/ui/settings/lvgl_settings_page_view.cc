#include "lvgl_settings_page_view.h"

#include "assets.h"
#include "display.h"
#include "lvgl_image.h"
#include "lvgl_page_touch_presenter.h"
#include "lvgl_theme.h"
#include "settings_page_model.h"
#include "settings_page_presenter.h"
#include "system_info.h"
#include "ui_page_router.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <src/misc/cache/instance/lv_image_cache.h>
#include <cstdint>
#include <cstring>
#include <stdexcept>

#define TAG "LvglSettings"

namespace ui::settings {

namespace {

/** 与 `settings_page_presenter.cc` 中一致：点击该 label 进入贴图对话页。 */
constexpr uintptr_t kUserDataOpenStickerChat = 0x53544348u;  // 'STCH'
constexpr uintptr_t kUserDataOpenMeeting = 0x4D545047u;      // 'MTPG'

/** 挂在根节点 user_data 上，根 DELETE 时一并释放 Presenter 与 View。 */
struct SettingsMvpBundle {
    SettingsPagePresenter* presenter = nullptr;
    LvglSettingsPageView* view = nullptr;
    bool swipe_active = false;
    lv_coord_t swipe_x0 = 0;
    lv_coord_t swipe_y0 = 0;
};

static void pointer_xy(lv_coord_t* out_x, lv_coord_t* out_y)
{
    if (out_x != nullptr) {
        *out_x = 0;
    }
    if (out_y != nullptr) {
        *out_y = 0;
    }
    lv_indev_t* indev = lv_indev_get_act();
    if (indev == nullptr) {
        return;
    }
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    if (out_x != nullptr) {
        *out_x = pt.x;
    }
    if (out_y != nullptr) {
        *out_y = pt.y;
    }
}

/** 从左向右水平滑动松手后返回上一页（与 WiFi / 会议页一致）。 */
static void on_swipe_back_gesture(lv_event_t* e)
{
    auto* bundle = static_cast<SettingsMvpBundle*>(lv_event_get_user_data(e));
    if (bundle == nullptr) {
        return;
    }

    const lv_event_code_t code = lv_event_get_code(e);
    lv_coord_t x = 0;
    lv_coord_t y = 0;
    pointer_xy(&x, &y);

    switch (code) {
        case LV_EVENT_PRESSED:
            bundle->swipe_active = true;
            bundle->swipe_x0 = x;
            bundle->swipe_y0 = y;
            break;
        case LV_EVENT_RELEASED: {
            if (!bundle->swipe_active) {
                break;
            }
            const lv_coord_t dx = x - bundle->swipe_x0;
            const lv_coord_t dy = y - bundle->swipe_y0;
            const lv_coord_t ady = dy >= 0 ? dy : -dy;
            bundle->swipe_active = false;
            if (dx > 56 && dx > ady) {
                UiPageRouter::Instance().NavigateBackFromInput();
            }
            break;
        }
        case LV_EVENT_PRESS_LOST:
            bundle->swipe_active = false;
            break;
        default:
            break;
    }
}

static void attach_swipe_back(lv_obj_t* obj, SettingsMvpBundle* bundle)
{
    if (obj == nullptr || bundle == nullptr) {
        return;
    }
    lv_obj_add_event_cb(obj, on_swipe_back_gesture, LV_EVENT_PRESSED, bundle);
    lv_obj_add_event_cb(obj, on_swipe_back_gesture, LV_EVENT_RELEASED, bundle);
    lv_obj_add_event_cb(obj, on_swipe_back_gesture, LV_EVENT_PRESS_LOST, bundle);
}

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
    const InternalHeapStats before = SystemInfo::GetInternalHeapStats();
    if (bundle->view != nullptr) {
        bundle->view->ReleasePageAssets();
    }
    delete bundle->presenter;
    delete bundle->view;
    delete bundle;
    lv_obj_set_user_data(root, nullptr);
    const InternalHeapStats after = SystemInfo::GetInternalHeapStats();
    const int delta_free = static_cast<int>(after.free_bytes) - static_cast<int>(before.free_bytes);
    const int delta_largest = static_cast<int>(after.largest_block) - static_cast<int>(before.largest_block);
    ESP_LOGI(TAG, "settings page freed: free %+d largest %+d (now free=%u largest=%u)", delta_free, delta_largest,
             static_cast<unsigned>(after.free_bytes), static_cast<unsigned>(after.largest_block));
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
    SystemInfo::LogInternalHeap("settings before layout");
    presenter->Show(model);
    SystemInfo::LogInternalHeap("settings after layout");

    lv_obj_t* root = static_cast<lv_obj_t*>(view->RootHandle());
    if (root == nullptr) {
        delete presenter;
        delete view;
        return nullptr;
    }

    auto* bundle = new SettingsMvpBundle{presenter, view};
    lv_obj_set_user_data(root, bundle);
    lv_obj_add_event_cb(root, SettingsRootOnDelete, LV_EVENT_DELETE, nullptr);
    attach_swipe_back(root, bundle);
    SystemInfo::LogInternalHeap("settings page created");
    return root;
}

/** 仅保存 Display 与主题指针；LVGL 控件延后到 BuildLayout 创建。 */
LvglSettingsPageView::LvglSettingsPageView(Display* display, LvglTheme* theme)
    : display_(display), theme_(theme) {}

void LvglSettingsPageView::ReleasePageAssets() {
    if (music_btn_image_ == nullptr) {
        return;
    }
    const void* src = music_btn_image_->image_dsc();
    if (src != nullptr) {
        lv_image_cache_drop(src);
    }
    music_btn_image_.reset();
}

/** 根对象由 LVGL 销毁时不再 lv_obj_del；只清空成员指针。 */
LvglSettingsPageView::~LvglSettingsPageView() {
    ReleasePageAssets();
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
            uint32_t alloc_caps = MALLOC_CAP_8BIT;
#if CONFIG_SPIRAM
            alloc_caps |= MALLOC_CAP_SPIRAM;
#endif
            auto* heap_copy = static_cast<uint8_t*>(heap_caps_malloc(asset_size, alloc_caps));
            if (heap_copy == nullptr && (alloc_caps & MALLOC_CAP_SPIRAM) != 0) {
                heap_copy = static_cast<uint8_t*>(heap_caps_malloc(asset_size, MALLOC_CAP_8BIT));
            }
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
    lv_obj_align(sticker_entry, LV_ALIGN_BOTTOM_MID, 0, -36);
    lv_obj_add_flag(sticker_entry, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(sticker_entry, reinterpret_cast<void*>(kUserDataOpenStickerChat));

#if CONFIG_USE_OYE_CLOUD_API
    lv_obj_t* meeting_entry = lv_label_create(panel);
    lv_label_set_text(meeting_entry, "会议纪要");
    lv_obj_set_style_text_font(meeting_entry, theme_->text_font()->font(), 0);
    lv_obj_set_style_text_color(meeting_entry, theme_->text_color(), 0);
    lv_obj_align(meeting_entry, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_add_flag(meeting_entry, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(meeting_entry, reinterpret_cast<void*>(kUserDataOpenMeeting));
#endif

    if (touch_presenter_ != nullptr) {
        ui::mvp::LvglPageAttachTouchHandlers(panel, touch_presenter_);
        ui::mvp::LvglPageAttachTouchHandlers(label, touch_presenter_);
        ui::mvp::LvglPageAttachTouchHandlers(sticker_entry, touch_presenter_);
#if CONFIG_USE_OYE_CLOUD_API
        ui::mvp::LvglPageAttachTouchHandlers(meeting_entry, touch_presenter_);
#endif
    }

    root_ = panel;
}

/** ISettingsPageView 接口：根据 model 触发一次性布局搭建。 */
void LvglSettingsPageView::Show(const SettingsPageModel& model) {
    BuildLayout(model);
}

}  // namespace ui::settings
