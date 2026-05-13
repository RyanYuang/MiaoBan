#include "settings_page_presenter.h"

#include "settings_page_view.h"
#include "ui_page_ids.h"
#include "ui_page_router.h"

#include <lvgl.h>

namespace ui::settings {

SettingsPagePresenter::SettingsPagePresenter(ISettingsPageView* view) : view_(view) {}

void SettingsPagePresenter::Show(const SettingsPageModel& model) {
    if (view_ == nullptr) {
        return;
    }
    view_->Show(model);
}

void SettingsPagePresenter::OnClick(lv_event_t* e) {
    (void)e;
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
