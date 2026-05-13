#include "settings_page_presenter.h"
#include "settings_page_view.h"
#include "ui_page_ids.h"
#include "ui_page_router.h"

#include <cstdint>
#include <lvgl.h>

namespace ui::settings {

namespace {

constexpr uintptr_t kUserDataOpenStickerChat = 0x53544348u;  // 'STCH' — 与 lvgl_settings_page_view.cc 一致

}  // namespace

SettingsPagePresenter::SettingsPagePresenter(ISettingsPageView* view) : view_(view) {}

void SettingsPagePresenter::Show(const SettingsPageModel& model) {
    if (view_ == nullptr) {
        return;
    }
    view_->Show(model);
}

void SettingsPagePresenter::OnClick(lv_event_t* e) {
    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (reinterpret_cast<uintptr_t>(lv_obj_get_user_data(target)) == kUserDataOpenStickerChat) {
        UiPageRouter::Instance().PostNavigateTo(UI_PAGE_ID(StickerChat));
        return;
    }

    const uint32_t now = lv_tick_get();
    constexpr uint32_t kMaxGapMs = 600;
    if (tap_.count == 0 || lv_tick_elaps(tap_.last_tick) > kMaxGapMs) {
        tap_.count = 1;
    } else {
        ++tap_.count;
    }
    tap_.last_tick = now;
    if (tap_.count >= 3) {
        tap_.count = 0;
        NavigateToAbout();
    }
}

void SettingsPagePresenter::NavigateToAbout() {
    UiPageRouter::Instance().PostNavigateTo(UI_PAGE_ID(About));
}

}  // namespace ui::settings
