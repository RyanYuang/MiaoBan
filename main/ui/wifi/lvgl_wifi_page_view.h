#pragma once

#include "wifi_page_view.h"

#include <lvgl.h>

class Display;
class LvglTheme;

namespace ui::wifi {

class WifiPagePresenter;

class LvglWifiPageView final : public IWifiPageView {
public:
    static void* CreateRouterPageRoot(Display* display, LvglTheme* theme);

    LvglWifiPageView(Display* display, LvglTheme* theme);
    ~LvglWifiPageView() override;

    void Show(const WifiPageModel& model) override;
    void SetStatusLine(const std::string& text) override;

    void* RootHandle() const { return root_; }
    void BindTouchPresenter(WifiPagePresenter* presenter) { touch_presenter_ = presenter; }

private:
    void BuildLayout(const WifiPageModel& model);
    void RebuildList(const WifiPageModel& model);

    Display* display_ = nullptr;
    LvglTheme* theme_ = nullptr;
    WifiPagePresenter* touch_presenter_ = nullptr;
    lv_obj_t* root_ = nullptr;
    lv_obj_t* list_panel_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
};

}  // namespace ui::wifi
