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

bool IsOyeCloudEnabled() {
#ifdef CONFIG_USE_OYE_CLOUD_API
    return true;
#else
    return false;
#endif
}

}  // namespace oye
