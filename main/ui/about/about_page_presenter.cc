#include "about_page_presenter.h"

#include "about_page_view.h"

namespace ui::about {

AboutPagePresenter::AboutPagePresenter(IAboutPageView* view)
    : view_(view) {}

void AboutPagePresenter::Show(const AboutPageModel& model) {
    if (view_ == nullptr) {
        return;
    }
    view_->Show(model);
}

}  // namespace ui::about
