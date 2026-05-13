#pragma once

#include "settings_page_model.h"

namespace ui::settings {

/** Settings 全屏页 View 契约：无 LVGL 类型。 */
class ISettingsPageView {
public:
    virtual ~ISettingsPageView() = default;

    virtual void Show(const SettingsPageModel& model) = 0;
};

}  // namespace ui::settings
