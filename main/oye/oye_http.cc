#include "oye_http.h"

#include "oye_config.h"

#include <board.h>
#include <esp_log.h>
#include <http.h>

#include <cstring>
#include <sstream>

namespace oye {

ApiResult::~ApiResult() {
    if (data != nullptr) {
        cJSON_Delete(data);
        data = nullptr;
    }
}

ApiResult::ApiResult(ApiResult&& other) noexcept
    : http_status(other.http_status),
      code(other.code),
      message(std::move(other.message)),
      data(other.data) {
    other.data = nullptr;
}

ApiResult& ApiResult::operator=(ApiResult&& other) noexcept {
    if (this != &other) {
        if (data != nullptr) {
            cJSON_Delete(data);
        }
        http_status = other.http_status;
        code = other.code;
        message = std::move(other.message);
        data = other.data;
        other.data = nullptr;
    }
    return *this;
}

std::string BuildUrl(const std::string& path) {
    std::string base = GetApiBaseUrl();
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    std::string p = path;
    if (p.empty() || p.front() != '/') {
        p = std::string(kApiPrefix) + "/" + p;
    } else if (p.rfind(kApiPrefix, 0) != 0) {
        p = std::string(kApiPrefix) + p;
    }
    return base + p;
}

std::unique_ptr<Http> NewAuthedHttp(int priority) {
    if (!HasAccessToken()) {
        return nullptr;
    }
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(priority);
    if (http == nullptr) {
        return nullptr;
    }
    http->SetHeader("Authorization", GetAccessToken().c_str());
    http->SetHeader("Accept", "application/json");
    http->SetHeader("Content-Type", "application/json; charset=utf-8");
    return http;
}

static ApiResult ParseResponse(std::unique_ptr<Http>& http) {
    ApiResult result;
    if (http == nullptr) {
        result.code = 5000;
        result.message = "no http";
        return result;
    }
    result.http_status = http->GetStatusCode();
    std::string body = http->ReadAll();
    http->Close();

    if (body.empty()) {
        result.code = result.http_status >= 200 && result.http_status < 300 ? 0 : 5000;
        result.message = "empty body";
        return result;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        result.code = 5000;
        result.message = "invalid json";
        return result;
    }

    auto code_item = cJSON_GetObjectItem(root, "code");
    auto msg_item = cJSON_GetObjectItem(root, "message");
    auto data_item = cJSON_GetObjectItem(root, "data");

    if (cJSON_IsNumber(code_item)) {
        result.code = code_item->valueint;
    }
    if (cJSON_IsString(msg_item)) {
        result.message = msg_item->valuestring;
    }
    if (data_item != nullptr) {
        result.data = cJSON_Duplicate(data_item, true);
    }
    cJSON_Delete(root);
    return result;
}

ApiResult RequestJson(const char* method, const std::string& path, const std::string& body) {
    ApiResult fail;
    auto http = NewAuthedHttp(0);
    if (http == nullptr) {
        fail.code = 4010;
        fail.message = "no token";
        return fail;
    }

    std::string url = BuildUrl(path);
    std::string content = body;
    if (!content.empty()) {
        http->SetContent(std::move(content));
    }
    if (!http->Open(method, url)) {
        fail.code = 5000;
        fail.message = "open failed";
        return fail;
    }
    return ParseResponse(http);
}

ApiResult RequestMultipart(const std::string& path, const std::string& field_name,
                           const std::string& filename, const std::string& content_type,
                           const uint8_t* data, size_t size, const std::string& extra_field_name,
                           const std::string& extra_field_value) {
    ApiResult fail;
    auto http = NewAuthedHttp(0);
    if (http == nullptr) {
        fail.code = 4010;
        fail.message = "no token";
        return fail;
    }

    const std::string boundary = "OyeBoundary";
    std::ostringstream preamble;
    if (!extra_field_name.empty()) {
        preamble << "--" << boundary << "\r\n"
                 << "Content-Disposition: form-data; name=\"" << extra_field_name << "\"\r\n\r\n"
                 << extra_field_value << "\r\n";
    }
    preamble << "--" << boundary << "\r\n"
             << "Content-Disposition: form-data; name=\"" << field_name << "\"; filename=\"" << filename
             << "\"\r\n"
             << "Content-Type: " << content_type << "\r\n\r\n";

    std::string closing = "\r\n--" + boundary + "--\r\n";

    std::string content_type_hdr = "multipart/form-data; boundary=" + boundary;
    http->SetHeader("Content-Type", content_type_hdr.c_str());
    http->SetHeader("Authorization", GetAccessToken().c_str());

    std::string url = BuildUrl(path);
    if (!http->Open("POST", url)) {
        fail.code = 5000;
        fail.message = "open failed";
        return fail;
    }

    std::string head = preamble.str();
    http->Write(head.c_str(), head.size());
    if (data != nullptr && size > 0) {
        http->Write(reinterpret_cast<const char*>(data), size);
    }
    http->Write(closing.c_str(), closing.size());
    http->Write("", 0);

    return ParseResponse(http);
}

}  // namespace oye
