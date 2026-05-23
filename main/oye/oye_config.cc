#include "oye_config.h"

#include "settings.h"

#ifdef CONFIG_OYE_API_BASE_URL
#include <sdkconfig.h>
#endif

namespace oye {

std::string GetApiBaseUrl() {
    Settings settings("oye", false);
    std::string url = settings.GetString("api_base_url");
    if (!url.empty()) {
        return url;
    }
#ifdef CONFIG_OYE_API_BASE_URL
    return CONFIG_OYE_API_BASE_URL;
#endif
    return "http://192.168.11.7:8000";
}

std::string GetAccessToken() {
    Settings user("user", false);
    std::string token = user.GetString("access_token");
    if (token.empty()) {
        return "";
    }
    if (token.find(' ') == std::string::npos) {
        return "Bearer " + token;
    }
    return token;
}

std::string GetAccessTokenRaw() {
    Settings user("user", false);
    std::string token = user.GetString("access_token");
    const std::string prefix = "Bearer ";
    if (token.rfind(prefix, 0) == 0) {
        return token.substr(prefix.size());
    }
    return token;
}

bool HasAccessToken() {
    return !GetAccessTokenRaw().empty();
}

std::string BuildWebSocketUrl(const std::string& resource_path) {
    std::string base = GetApiBaseUrl();
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    std::string ws_base;
    if (base.rfind("https://", 0) == 0) {
        ws_base = "wss://" + base.substr(8);
    } else if (base.rfind("http://", 0) == 0) {
        ws_base = "ws://" + base.substr(7);
    } else {
        ws_base = "ws://" + base;
    }
    std::string path = resource_path;
    if (path.empty() || path.front() != '/') {
        path = "/" + path;
    }
    return ws_base + std::string(kApiPrefix) + path + "?token=" + GetAccessTokenRaw();
}

bool IsOyeCloudEnabled() {
#ifdef CONFIG_USE_OYE_CLOUD_API
    return true;
#else
    return false;
#endif
}

}  // namespace oye
