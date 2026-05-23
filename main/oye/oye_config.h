#ifndef OYE_CONFIG_H
#define OYE_CONFIG_H

#include <string>

namespace oye {

/** MCU API prefix after base URL, e.g. /api/v1/mcu */
constexpr const char* kApiPrefix = "/api/v1/mcu";

std::string GetApiBaseUrl();
/** WebSocket URL: wss://{host}{kApiPrefix}{resource}?token=... (resource e.g. /voice-chat/ws). */
std::string BuildWebSocketUrl(const std::string& resource_path);
std::string GetAccessToken();
/** Token for WebSocket query param (no "Bearer " prefix). */
std::string GetAccessTokenRaw();
bool HasAccessToken();
bool IsOyeCloudEnabled();

}  // namespace oye

#endif
