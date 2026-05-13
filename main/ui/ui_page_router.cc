#include "ui_page_router.h"

#include "display.h"
#include "ui_command_dispatcher.h"

#include <cstddef>

#include <sdkconfig.h>

#include <esp_log.h>

#ifndef CONFIG_USE_EMOTE_MESSAGE_STYLE
#include "lvgl_display.h"
#include "lvgl_theme.h"
#include <lvgl.h>
#endif

namespace {

constexpr char TAG[] = "UiPageRouter";

#ifndef CONFIG_USE_EMOTE_MESSAGE_STYLE
lv_obj_t* CreateLvglPageRoot(UiPageId id, Display* display) {
    auto* theme = dynamic_cast<LvglTheme*>(display->GetTheme());
    if (theme == nullptr) {
        ESP_LOGE(TAG, "no LvglTheme");
        return nullptr;
    }

    lv_obj_t* screen = lv_screen_active();
    lv_obj_t* panel = lv_obj_create(screen);
    lv_obj_set_size(panel, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(panel, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* label = lv_label_create(panel);

    switch (id) {
        case UiPageId::kSettings:
            lv_obj_set_style_bg_color(panel, theme->background_color(), 0);
            lv_label_set_text(label, "Settings");
            break;
        case UiPageId::kHome:
            lv_obj_set_style_bg_color(panel, theme->chat_background_color(), 0);
            lv_label_set_text(label, "Home");
            break;
        default:
            lv_obj_del(panel);
            return nullptr;
    }

    lv_obj_set_style_text_font(label, theme->text_font()->font(), 0);
    lv_obj_set_style_text_color(label, theme->text_color(), 0);
    lv_obj_center(label);
    return panel;
}
#endif

}  // namespace

UiPageRouter& UiPageRouter::Instance() {
    static UiPageRouter instance;
    return instance;
}

void UiPageRouter::Init(Display* display) {
    display_ = display;
    page_stack_depth_ = 0;
    for (std::size_t i = 0; i < kMaxPageDepth; ++i) {
        page_stack_[i] = nullptr;
    }
}

void UiPageRouter::PostNavigateTo(UiPageId id) {
    UiCommandDispatcher::Instance().Post([this, id]() { ApplyNavigateTo(id); });
}

void UiPageRouter::PostNavigateBack() {
    UiCommandDispatcher::Instance().Post([this]() { ApplyNavigateBack(); });
}

void UiPageRouter::ApplyNavigateTo(UiPageId id) {
    if (display_ == nullptr || id == UiPageId::kNone) {
        return;
    }

#ifndef CONFIG_USE_EMOTE_MESSAGE_STYLE
    auto* lvgl = dynamic_cast<LvglDisplay*>(display_);
    if (lvgl == nullptr || !lvgl->IsSetupUICalled()) {
        ESP_LOGW(TAG, "NavigateTo: not LvglDisplay or SetupUI not called");
        return;
    }
    DisplayLockGuard lock(display_);
    lv_obj_t* root = CreateLvglPageRoot(id, display_);
    if (root == nullptr) {
        return;
    }
    PageStackPush(root);
    lv_obj_move_foreground(root);
#else
    switch (id) {
        case UiPageId::kSettings:
            display_->ShowNotification("Settings", 60000);
            break;
        case UiPageId::kHome:
            display_->ShowNotification("Home", 60000);
            break;
        default:
            break;
    }
#endif
}

void UiPageRouter::ApplyNavigateBack() {
    if (display_ == nullptr) {
        return;
    }

#ifndef CONFIG_USE_EMOTE_MESSAGE_STYLE
    if (PageStackEmpty()) {
        ESP_LOGD(TAG, "NavigateBack: stack empty");
        return;
    }
    DisplayLockGuard lock(display_);
    void* top = PageStackPop();
    if (top != nullptr) {
        lv_obj_del(static_cast<lv_obj_t*>(top));
    }
#else
    ESP_LOGD(TAG, "NavigateBack: emote style has no LVGL page stack");
#endif
}

void UiPageRouter::PageStackPush(void* root) {
    if (root == nullptr) {
        return;
    }
    if (page_stack_depth_ >= kMaxPageDepth) {
        ESP_LOGW(TAG, "page stack full (%u), drop push", static_cast<unsigned>(kMaxPageDepth));
        return;
    }
    page_stack_[page_stack_depth_++] = root;
}

void* UiPageRouter::PageStackPop() {
    if (page_stack_depth_ == 0) {
        return nullptr;
    }
    void* top = page_stack_[page_stack_depth_ - 1];
    page_stack_[page_stack_depth_ - 1] = nullptr;
    page_stack_depth_--;
    return top;
}

bool UiPageRouter::PageStackEmpty() const {
    return page_stack_depth_ == 0;
}
