#pragma once

#include "meeting_page_model.h"

namespace ui::meeting {

class IMeetingPageView {
public:
    virtual ~IMeetingPageView() = default;
    virtual void Show(const MeetingPageModel& model) = 0;
    virtual void SetStatusLine(const std::string& text) = 0;
};

}  // namespace ui::meeting
