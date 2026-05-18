#ifndef OYE_HTTP_H
#define OYE_HTTP_H

#include <cJSON.h>
#include <esp_err.h>
#include <memory>
#include <string>

class Http;

namespace oye {

struct ApiResult {
    int http_status = 0;
    int code = -1;
    std::string message;
    cJSON* data = nullptr;

    ~ApiResult();
    ApiResult() = default;
    ApiResult(const ApiResult&) = delete;
    ApiResult& operator=(const ApiResult&) = delete;
    ApiResult(ApiResult&& other) noexcept;
    ApiResult& operator=(ApiResult&& other) noexcept;

    bool Ok() const { return code == 0; }
};

std::string BuildUrl(const std::string& path);
std::unique_ptr<Http> NewAuthedHttp(int priority = 0);

/** GET/POST/PATCH with JSON body; parses unified {code,message,data}. */
ApiResult RequestJson(const char* method, const std::string& path, const std::string& body = "");
/** Multipart upload with one file field. */
ApiResult RequestMultipart(const std::string& path, const std::string& field_name,
                           const std::string& filename, const std::string& content_type,
                           const uint8_t* data, size_t size,
                           const std::string& extra_field_name = "",
                           const std::string& extra_field_value = "");

}  // namespace oye

#endif
