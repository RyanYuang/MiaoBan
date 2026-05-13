#pragma once

#include "about_page_model.h"

namespace ui::about {

class IAboutPageView {
public:
    virtual ~IAboutPageView() = default;
    virtual void Show(const AboutPageModel& model) = 0;
};

}  // namespace ui::about
