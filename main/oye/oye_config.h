#ifndef OYE_CONFIG_H
#define OYE_CONFIG_H

#include <string>

namespace oye {

/** API prefix after base URL, e.g. /api/v1 */
constexpr const char* kApiPrefix = "/api/v1";

std::string GetApiBaseUrl();
std::string GetAccessToken();
/** Token for WebSocket query param (no "Bearer " prefix). */
std::string GetAccessTokenRaw();
bool HasAccessToken();
bool IsOyeCloudEnabled();

}  // namespace oye

#endif
