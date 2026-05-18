#pragma once

#include "wifi_page_model.h"

namespace ui::wifi {

class IWifiPageView {
public:
    virtual ~IWifiPageView() = default;
    virtual void Show(const WifiPageModel& model) = 0;
    virtual void SetStatusLine(const std::string& text) = 0;
};

}  // namespace ui::wifi
