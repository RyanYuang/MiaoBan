#pragma once

#include "lvgl_page_touch_presenter.h"
#include "settings_page_model.h"

namespace ui::settings {

class ISettingsPageView;

/** 编排 Settings 页展示；触摸经 LVGL `lv_event_t` 进入 `OnPress` / `OnClick` / `OnDrag`。 */
class SettingsPagePresenter final : public ui::mvp::LvglPageTouchPresenter {
public:
    explicit SettingsPagePresenter(ISettingsPageView* view);

    void Show(const SettingsPageModel& model);

    void OnClick(lv_event_t* e) override;

private:
    void NavigateToAbout();

    ISettingsPageView* view_;
    struct TapTracker {
        uint8_t count = 0;
        uint32_t last_tick = 0;
    } tap_{};
};

}  // namespace ui::settings
