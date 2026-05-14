#include "ui_page_router.h"

#include "display.h"
#include "system_info.h"
#include "ui_command_dispatcher.h"
#include "ui_page_ids.h"

#include <cstddef>
#include <string>

#include <sdkconfig.h>

#include <esp_log.h>

#ifndef CONFIG_USE_EMOTE_MESSAGE_STYLE
#include "lvgl_display.h"
#include "lvgl_theme.h"
#include "ui_page_lvgl_registry.h"
#include <lvgl.h>
#endif

namespace {

constexpr char TAG[] = "UiPageRouter";

#ifndef CONFIG_USE_EMOTE_MESSAGE_STYLE
/** 按页面枚举创建该页的 LVGL 根节点；成功返回根指针，失败返回 nullptr。须在已加 Display 锁的上下文中调用。 */
lv_obj_t* CreateLvglPageRoot(UiPageId id, Display* display) {
    auto* theme = dynamic_cast<LvglTheme*>(display->GetTheme());
    if (theme == nullptr) {
        ESP_LOGE(TAG, "no LvglTheme");
        return nullptr;
    }

    switch (id) {
#define UI_PAGE_LVGL_ROOT_CASE(sym, factory) \
    case UI_PAGE_ID(sym): \
        return static_cast<lv_obj_t*>((factory)(display, theme));

        UI_PAGE_LVGL_ROOT_FACTORY_LIST(UI_PAGE_LVGL_ROOT_CASE)
#undef UI_PAGE_LVGL_ROOT_CASE

        case UiPageId::kHome: {
            lv_obj_t* screen = lv_screen_active();
            lv_obj_t* panel = lv_obj_create(screen);
            lv_obj_set_size(panel, LV_HOR_RES, LV_VER_RES);
            lv_obj_align(panel, LV_ALIGN_TOP_LEFT, 0, 0);
            lv_obj_set_style_radius(panel, 0, 0);
            lv_obj_set_style_pad_all(panel, 0, 0);
            lv_obj_set_style_border_width(panel, 0, 0);
            lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
            lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_bg_color(panel, theme->chat_background_color(), 0);

            lv_obj_t* label = lv_label_create(panel);
            lv_label_set_text(label, "Home");
            lv_obj_set_style_text_font(label, theme->text_font()->font(), 0);
            lv_obj_set_style_text_color(label, theme->text_color(), 0);
            lv_obj_center(label);
            return panel;
        }
        default:
            return nullptr;
    }
}
#endif

}  // namespace

/** 返回全局单例，供任意处投递路由命令。 */
UiPageRouter& UiPageRouter::Instance() {
    static UiPageRouter instance;
    return instance;
}

/** 绑定 Display 并清空页面栈；在 SetupUI 之后、首次跳转前调用一次即可。 */
void UiPageRouter::Init(Display* display) {
    display_ = display;
    page_stack_depth_ = 0;
    for (std::size_t i = 0; i < kMaxPageDepth; ++i) {
        page_stack_[i] = nullptr;
    }
}

/** 将「前进到指定页」投递到 UI 命令队列，在 UI 线程串行执行 ApplyNavigateTo。 */
void UiPageRouter::PostNavigateTo(UiPageId id) {
    UiCommandDispatcher::Instance().Post([this, id]() { ApplyNavigateTo(id); });
}

/** 将「返回上一页」投递到 UI 命令队列，在 UI 线程串行执行 ApplyNavigateBack。 */
void UiPageRouter::PostNavigateBack() {
    UiCommandDispatcher::Instance().Post([this]() { ApplyNavigateBack(); });
}

/** 将「关闭全部叠页」投递到 UI 命令队列。 */
void UiPageRouter::PostNavigateCloseAll() {
    UiCommandDispatcher::Instance().Post([this]() { ApplyNavigateCloseAll(); });
}

/**
 * 在 UI 线程执行前进：LVGL 模式下创建整页根、压栈并置顶；表情模式下用通知等轻量展示，不维护 LVGL 栈。
 */
void UiPageRouter::ApplyNavigateTo(UiPageId id) {
    if (display_ == nullptr || id == UiPageId::kNone) {
        return;
    }

#ifndef CONFIG_USE_EMOTE_MESSAGE_STYLE
    auto* lvgl = dynamic_cast<LvglDisplay*>(display_);
    if (lvgl == nullptr || !display_->IsSetupUICalled()) {
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
        case UiPageId::kAbout: {
            std::string msg = "About\n";
            msg += SystemInfo::GetChipModelName();
            msg += "\n";
            msg += SystemInfo::GetMacAddress();
            display_->ShowNotification(msg.c_str(), 60000);
            break;
        }
        case UiPageId::kStickerChat:
            display_->ShowNotification("Sticker chat", 60000);
            break;
        default:
            break;
    }
#endif
}

/**
 * 在 UI 线程执行返回：LVGL 模式下弹出栈顶并 lv_obj_del；表情模式下无栈，仅打日志。
 */
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

void UiPageRouter::ApplyNavigateCloseAll()
{
    if (display_ == nullptr) {
        return;
    }

#ifndef CONFIG_USE_EMOTE_MESSAGE_STYLE
    auto* lvgl = dynamic_cast<LvglDisplay*>(display_);
    if (lvgl == nullptr || !display_->IsSetupUICalled()) {
        ESP_LOGW(TAG, "NavigateCloseAll: not LvglDisplay or SetupUI not called");
        return;
    }
    DisplayLockGuard lock(display_);
    while (!PageStackEmpty()) {
        void* top = PageStackPop();
        if (top != nullptr) {
            lv_obj_del(static_cast<lv_obj_t*>(top));
        }
    }
#else
    ESP_LOGD(TAG, "NavigateCloseAll: emote style has no LVGL page stack");
#endif
}

/** 将一页的根指针压入内部栈；栈满则丢弃本次入栈并打警告日志。 */
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

/** 弹出当前栈顶根指针（不销毁 LVGL 对象）；栈空时返回 nullptr。 */
void* UiPageRouter::PageStackPop() {
    if (page_stack_depth_ == 0) {
        return nullptr;
    }
    void* top = page_stack_[page_stack_depth_ - 1];
    page_stack_[page_stack_depth_ - 1] = nullptr;
    page_stack_depth_--;
    return top;
}

/** 判断内部页面栈是否为空。 */
bool UiPageRouter::PageStackEmpty() const {
    return page_stack_depth_ == 0;
}
