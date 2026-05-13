#pragma once

#include "about_page_model.h"
#include "lvgl_page_touch_presenter.h"

namespace ui::about {

class IAboutPageView;

class AboutPagePresenter final : public ui::mvp::LvglPageTouchPresenter {
public:
    explicit AboutPagePresenter(IAboutPageView* view);

    void Show(const AboutPageModel& model);

private:
    IAboutPageView* view_;
};

}  // namespace ui::about
