#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ui::wifi {

struct WifiNetworkRow {
    std::string ssid;
    int8_t rssi = 0;
    bool encrypted = false;
    bool saved = false;
    bool connected = false;
};

struct WifiPageModel {
    std::string title = "Wi-Fi";
    std::string status_line;
    std::vector<WifiNetworkRow> networks;
    bool scanning = false;
};

}  // namespace ui::wifi
