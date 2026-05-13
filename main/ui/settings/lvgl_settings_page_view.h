#pragma once

#include "settings_page_view.h"

#include <lvgl.h>

class Display;
class LvglTheme;

namespace ui::settings {

class SettingsPagePresenter;

/** LVGL 全屏 Settings：布局与控件均在 View 内初始化；CreateRouterPageRoot 供 UiPageRouter 压栈。 */
class LvglSettingsPageView final : public ISettingsPageView {
public:
    /** 组装 MVP 并返回根 `lv_obj_t*`；根 DELETE 时释放。调用方须已持有 Display 锁。 */
    static void* CreateRouterPageRoot(Display* display, LvglTheme* theme);

    LvglSettingsPageView(Display* display, LvglTheme* theme);
    ~LvglSettingsPageView() override;

    void Show(const SettingsPageModel& model) override;

    void* RootHandle() const { return root_; }

    /** CreateRouterPageRoot 在 Show 前绑定，用于挂载 LVGL 触摸回调到 Presenter。 */
    void BindTouchPresenter(SettingsPagePresenter* presenter) { touch_presenter_ = presenter; }

private:
    void BuildLayout(const SettingsPageModel& model);

    Display* display_;
    LvglTheme* theme_;
    void* root_ = nullptr;
    SettingsPagePresenter* touch_presenter_ = nullptr;
};

}  // namespace ui::settings
