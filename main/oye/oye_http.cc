#include "oye_http.h"

#include "oye_config.h"

#include <board.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <http.h>

#include <cstring>
#include <sstream>

namespace oye {

namespace {

constexpr char TAG[] = "OyeHttp";
constexpr size_t kBodyLogMax = 256;
constexpr int kHttpTimeoutMs = 30000;
/** 任务栈在 PSRAM；internal 主要给 socket/TCB/lwIP，看 largest 连续块即可。 */
constexpr size_t kMinLargestInternalBlock = 1280;
constexpr size_t kMinFreeInternalForHttp = 2816;

struct HeapSnapshot {
    size_t free_internal = 0;
    size_t largest_internal = 0;
};

HeapSnapshot SnapshotInternalHeap() {
    HeapSnapshot s;
    s.free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s.largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    return s;
}

void LogHeap(const char* label) {
    const auto s = SnapshotInternalHeap();
    ESP_LOGI(TAG, "%s: free_internal=%u largest_internal=%u", label,
             static_cast<unsigned>(s.free_internal), static_cast<unsigned>(s.largest_internal));
}

/** 请求期间关闭 Wi-Fi 省电，避免已发包但收不到响应头。 */
class HttpTransportGuard {
public:
    HttpTransportGuard() {
        LogHeap("HTTP begin");
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    }
    ~HttpTransportGuard() {
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        LogHeap("HTTP end");
    }
};

void ConfigureHttpClient(Http& http) {
    http.SetTimeout(kHttpTimeoutMs);
    http.SetKeepAlive(false);
}

bool HasEnoughMemoryForHttp(ApiResult& fail) {
    const auto s = SnapshotInternalHeap();
    if (s.largest_internal >= kMinLargestInternalBlock && s.free_internal >= kMinFreeInternalForHttp) {
        return true;
    }
    fail.http_status = 0;
    fail.code = 5001;
    fail.message = "low memory";
    ESP_LOGE(TAG,
             "reject HTTP: free_internal=%u largest_internal=%u (need free>=%u largest>=%u)",
             static_cast<unsigned>(s.free_internal), static_cast<unsigned>(s.largest_internal),
             static_cast<unsigned>(kMinFreeInternalForHttp),
             static_cast<unsigned>(kMinLargestInternalBlock));
    return false;
}

void LogBodyPreview(const char* label, const std::string& body) {
    if (body.empty()) {
        ESP_LOGI(TAG, "%s: (empty)", label);
        return;
    }
    if (body.size() <= kBodyLogMax) {
        ESP_LOGI(TAG, "%s (%u bytes): %s", label, static_cast<unsigned>(body.size()), body.c_str());
        return;
    }
    std::string preview(body.data(), kBodyLogMax);
    ESP_LOGI(TAG, "%s (%u bytes, first %u): %s…", label, static_cast<unsigned>(body.size()),
             static_cast<unsigned>(kBodyLogMax), preview.c_str());
}

void LogApiResultSummary(const char* stage, const ApiResult& res) {
    ESP_LOGI(TAG, "%s: http_status=%d biz_code=%d ok=%d msg=%s data=%s", stage, res.http_status, res.code,
             res.Ok() ? 1 : 0, res.message.c_str(), res.data != nullptr ? "present" : "null");
}

}  // namespace

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
        ESP_LOGW(TAG, "NewAuthedHttp: no access token");
        return nullptr;
    }
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(priority);
    if (http == nullptr) {
        ESP_LOGE(TAG, "NewAuthedHttp: CreateHttp failed");
        return nullptr;
    }
    http->SetHeader("Authorization", GetAccessToken().c_str());
    http->SetHeader("Accept", "application/json");
    http->SetHeader("Content-Type", "application/json; charset=utf-8");
    ConfigureHttpClient(*http);
    return http;
}

static ApiResult ParseResponse(std::unique_ptr<Http>& http) {
    ApiResult result;
    if (http == nullptr) {
        result.code = 5000;
        result.message = "no http";
        ESP_LOGE(TAG, "ParseResponse: no http");
        return result;
    }
    result.http_status = http->GetStatusCode();
    const int transport_err = http->GetLastError();
    if (result.http_status < 0) {
        result.code = 5002;
        result.message = "headers timeout";
        ESP_LOGE(TAG,
                 "ParseResponse: no HTTP headers within %dms (tcp_err=0x%x). Server may have "
                 "replied but device did not receive — check Wi-Fi PS / internal heap.",
                 kHttpTimeoutMs, transport_err);
        http->Close();
        LogApiResultSummary("ParseResponse done", result);
        return result;
    }
    ESP_LOGI(TAG, "ParseResponse: http_status=%d, reading body…", result.http_status);
    std::string body = http->ReadAll();
    if (body.empty() && http->GetLastError() != 0) {
        ESP_LOGW(TAG, "ParseResponse: empty body with tcp_err=0x%x", http->GetLastError());
    }
    http->Close();
    LogBodyPreview("response", body);

    if (body.empty()) {
        result.code = result.http_status >= 200 && result.http_status < 300 ? 0 : 5000;
        result.message = result.http_status >= 200 && result.http_status < 300 ? "empty body" : "read failed";
        ESP_LOGW(TAG, "ParseResponse: empty body, inferred biz_code=%d http=%d", result.code,
                 result.http_status);
        LogApiResultSummary("ParseResponse done", result);
        return result;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        result.code = 5000;
        result.message = "invalid json";
        ESP_LOGE(TAG, "ParseResponse: cJSON_Parse failed");
        LogApiResultSummary("ParseResponse done", result);
        return result;
    }

    auto code_item = cJSON_GetObjectItem(root, "code");
    auto msg_item = cJSON_GetObjectItem(root, "message");
    auto data_item = cJSON_GetObjectItem(root, "data");

    if (cJSON_IsNumber(code_item)) {
        result.code = code_item->valueint;
    } else {
        ESP_LOGW(TAG, "ParseResponse: missing or non-numeric \"code\" (http=%d)", result.http_status);
    }
    if (cJSON_IsString(msg_item)) {
        result.message = msg_item->valuestring;
    }
    if (data_item != nullptr) {
        result.data = cJSON_Duplicate(data_item, true);
        ESP_LOGI(TAG, "ParseResponse: data field type=%d", data_item->type);
    } else {
        ESP_LOGW(TAG, "ParseResponse: no \"data\" field");
    }
    cJSON_Delete(root);
    LogApiResultSummary("ParseResponse done", result);
    return result;
}

ApiResult RequestJson(const char* method, const std::string& path, const std::string& body) {
    ApiResult fail;
    HttpTransportGuard guard;
    ESP_LOGI(TAG, "RequestJson %s %s body_bytes=%u", method, path.c_str(),
             static_cast<unsigned>(body.size()));
    if (!HasEnoughMemoryForHttp(fail)) {
        return fail;
    }
    auto http = NewAuthedHttp(0);
    if (http == nullptr) {
        fail.code = 4010;
        fail.message = "no token";
        ESP_LOGE(TAG, "RequestJson failed: %s", fail.message.c_str());
        return fail;
    }

    std::string url = BuildUrl(path);
    ESP_LOGI(TAG, "RequestJson open %s url=%s", method, url.c_str());
    std::string content = body;
    if (!content.empty()) {
        http->SetContent(std::move(content));
    }
    if (!http->Open(method, url)) {
        fail.code = 5000;
        fail.message = "open failed";
        ESP_LOGE(TAG, "RequestJson: Open failed for %s %s", method, path.c_str());
        return fail;
    }
    ESP_LOGI(TAG, "RequestJson: Open ok, waiting for response…");
    ApiResult res = ParseResponse(http);
    LogApiResultSummary("RequestJson finished", res);
    return res;
}

ApiResult RequestMultipart(const std::string& path, const std::string& field_name,
                           const std::string& filename, const std::string& content_type,
                           const uint8_t* data, size_t size, const std::string& extra_field_name,
                           const std::string& extra_field_value) {
    ApiResult fail;
    HttpTransportGuard guard;
    if (!HasEnoughMemoryForHttp(fail)) {
        return fail;
    }
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
    ESP_LOGI(TAG, "RequestMultipart POST %s file=%s bytes=%u url=%s", path.c_str(), filename.c_str(),
             static_cast<unsigned>(size), url.c_str());
    if (!http->Open("POST", url)) {
        fail.code = 5000;
        fail.message = "open failed";
        ESP_LOGE(TAG, "RequestMultipart: Open failed");
        return fail;
    }

    std::string head = preamble.str();
    http->Write(head.c_str(), head.size());
    if (data != nullptr && size > 0) {
        http->Write(reinterpret_cast<const char*>(data), size);
    }
    http->Write(closing.c_str(), closing.size());
    http->Write("", 0);

    ApiResult res = ParseResponse(http);
    LogApiResultSummary("RequestMultipart finished", res);
    return res;
}

}  // namespace oye
