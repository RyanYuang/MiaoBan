#pragma once

#include "about_page_view.h"

class Display;
class LvglTheme;

namespace ui::about {

class AboutPagePresenter;

/** LVGL About：布局在 View；CreateRouterPageRoot 供 UiPageRouter 压栈。 */
class LvglAboutPageView final : public IAboutPageView {
public:
    static void* CreateRouterPageRoot(Display* display, LvglTheme* theme);

    LvglAboutPageView(Display* display, LvglTheme* theme);
    ~LvglAboutPageView() override;

    void Show(const AboutPageModel& model) override;

    void* RootHandle() const { return root_; }

    void BindTouchPresenter(AboutPagePresenter* p) { presenter_for_touch_ = p; }

private:
    void BuildLayout(const AboutPageModel& model);

    Display* display_;
    LvglTheme* theme_;
    void* root_ = nullptr;
    AboutPagePresenter* presenter_for_touch_ = nullptr;
};

}  // namespace ui::about
