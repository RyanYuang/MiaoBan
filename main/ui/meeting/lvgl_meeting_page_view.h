#pragma once

#include "meeting_page_view.h"

#include <lvgl.h>

class Display;
class LvglTheme;

namespace ui::meeting {

class MeetingPagePresenter;

class LvglMeetingPageView final : public IMeetingPageView {
public:
    static void* CreateRouterPageRoot(Display* display, LvglTheme* theme);

    LvglMeetingPageView(Display* display, LvglTheme* theme);
    ~LvglMeetingPageView() override;

    void Show(const MeetingPageModel& model) override;
    void SetStatusLine(const std::string& text) override;

    void* RootHandle() const { return root_; }
    void BindTouchPresenter(MeetingPagePresenter* presenter) { touch_presenter_ = presenter; }

private:
    void BuildLayout(const MeetingPageModel& model);
    void RebuildList(const MeetingPageModel& model);
    void UpdateDetailPanel(const MeetingPageModel& model);

    Display* display_ = nullptr;
    LvglTheme* theme_ = nullptr;
    MeetingPagePresenter* touch_presenter_ = nullptr;
    lv_obj_t* root_ = nullptr;
    lv_obj_t* list_panel_ = nullptr;
    lv_obj_t* detail_panel_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* detail_body_ = nullptr;
};

}  // namespace ui::meeting
