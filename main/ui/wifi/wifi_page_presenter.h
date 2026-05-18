#pragma once

#include "lvgl_page_touch_presenter.h"
#include "wifi_page_model.h"

namespace ui::wifi {

/** 在 WifiStation 注册事件前调用一次，确保 SCAN_DONE 时优先缓存 AP 列表。 */
void RegisterWifiScanEventHandler();

class IWifiPageView;

class WifiPagePresenter final : public ui::mvp::LvglPageTouchPresenter {
public:
    explicit WifiPagePresenter(IWifiPageView* view);

    void Show(const WifiPageModel& model);
    void OnShow();

    void OnClick(lv_event_t* e) override;

private:
    void NavigateBack();
    void StartScan();
    void ConnectToNetwork(int row_index);
    void RunBackgroundTask(void (*worker)(WifiPagePresenter* self));

    static void ScanTask(WifiPagePresenter* self);
    static void ConnectTask(WifiPagePresenter* self);

    static bool IsSsidSaved(const std::string& ssid);
    static std::string FindSavedPassword(const std::string& ssid);

    IWifiPageView* view_;
    WifiPageModel model_;
    bool busy_ = false;
    int pending_connect_index_ = -1;
};

}  // namespace ui::wifi
