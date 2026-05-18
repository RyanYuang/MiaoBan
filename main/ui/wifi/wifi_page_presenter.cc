#include "wifi_page_presenter.h"

#include "ui_command_dispatcher.h"
#include "ui_page_router.h"
#include "wifi_page_view.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lvgl.h>
#include <ssid_manager.h>
#include <wifi_manager.h>

namespace ui::wifi {

namespace {

constexpr char TAG[] = "WifiPresenter";
constexpr uintptr_t kUserDataBack = 0x4241434Bu;
constexpr uintptr_t kUserDataRefresh = 0x52454652u;
constexpr uintptr_t kUserDataRowBase = 0x57494630u;
constexpr int kMaxScanResults = 24;
constexpr int kScanWaitTimeoutMs = 12000;
constexpr int kMaxScanAttempts = 2;
constexpr uint32_t kWifiUiTaskStackWords = 4096;

struct ScanAp {
    std::string ssid;
    int8_t rssi = 0;
    bool encrypted = false;
};

SemaphoreHandle_t s_scan_done_sem = nullptr;
esp_event_handler_instance_t s_scan_handler = nullptr;
/** 仅在本页发起的扫描进行中时为 true，避免 WifiStation 等触发的 SCAN_DONE 误唤醒等待。 */
std::atomic<bool> s_scan_wait_active{false};
std::mutex s_scan_cache_mutex;
std::vector<ScanAp> s_scan_cached_aps;

/** 根据 WifiManager 状态刷新 model.status_line（未初始化 / 配网 / 已连接 / 未连接）。 */
static void UpdateCurrentConnectionStatus(WifiPageModel& model)
{
    auto& wifi = WifiManager::GetInstance();
    if (!wifi.IsInitialized()) {
        model.status_line = "Wi-Fi 未初始化";
        return;
    }
    if (wifi.IsConfigMode()) {
        model.status_line = "配网模式：" + wifi.GetApSsid();
        return;
    }
    if (wifi.IsConnected()) {
        model.status_line = "已连接：" + wifi.GetSsid() + "  " + wifi.GetIpAddress();
        return;
    }
    model.status_line = "未连接";
}

/** 从驱动读取当前扫描缓存，去重（同 SSID 保留更强信号）。须在 SCAN_DONE 后尽快调用。 */
static std::vector<ScanAp> FetchScanResultsFromDriver()
{
    std::vector<ScanAp> results;
    uint16_t ap_count = 0;
    if (esp_wifi_scan_get_ap_num(&ap_count) != ESP_OK || ap_count == 0) {
        return results;
    }

    std::vector<wifi_ap_record_t> records(ap_count);
    if (esp_wifi_scan_get_ap_records(&ap_count, records.data()) != ESP_OK) {
        return results;
    }

    for (int i = 0; i < ap_count; ++i) {
        const char* raw_ssid = reinterpret_cast<const char*>(records[i].ssid);
        if (raw_ssid[0] == '\0') {
            continue;
        }
        ScanAp ap;
        ap.ssid = raw_ssid;
        ap.rssi = records[i].rssi;
        ap.encrypted = records[i].authmode != WIFI_AUTH_OPEN;

        auto it = std::find_if(results.begin(), results.end(),
                               [&](const ScanAp& x) { return x.ssid == ap.ssid; });
        if (it == results.end()) {
            results.push_back(ap);
        } else if (ap.rssi > it->rssi) {
            *it = ap;
        }
    }
    std::sort(results.begin(), results.end(),
              [](const ScanAp& a, const ScanAp& b) { return a.rssi > b.rssi; });
    if (static_cast<int>(results.size()) > kMaxScanResults) {
        results.resize(kMaxScanResults);
    }
    return results;
}

static void ClearScanCache()
{
    std::lock_guard<std::mutex> lock(s_scan_cache_mutex);
    s_scan_cached_aps.clear();
}

/** 取出回调里缓存的 AP 列表（取出后清空缓存）。 */
static void TakeScanCache(std::vector<ScanAp>& out)
{
    std::lock_guard<std::mutex> lock(s_scan_cache_mutex);
    out = std::move(s_scan_cached_aps);
    s_scan_cached_aps.clear();
}

static void OnWifiScanDone(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    (void)arg;
    if (event_base != WIFI_EVENT || event_id != WIFI_EVENT_SCAN_DONE) {
        return;
    }
    if (!s_scan_wait_active.load(std::memory_order_acquire) || s_scan_done_sem == nullptr) {
        return;
    }

    /* 在事件任务里立即读 AP，避免 WifiStation 连网后缓存被清空 */
    std::vector<ScanAp> cached = FetchScanResultsFromDriver();
    ESP_LOGI(TAG, "SCAN_DONE: cached %u AP(s)", static_cast<unsigned>(cached.size()));

    {
        std::lock_guard<std::mutex> lock(s_scan_cache_mutex);
        s_scan_cached_aps = std::move(cached);
    }

    s_scan_wait_active.store(false, std::memory_order_release);
    xSemaphoreGive(s_scan_done_sem);
}

static void EnsureScanDoneHandler()
{
    if (s_scan_done_sem != nullptr) {
        return;
    }
    s_scan_done_sem = xSemaphoreCreateBinary();
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, OnWifiScanDone, nullptr,
                                        &s_scan_handler);
}

/**
 * 非阻塞启动 Wi-Fi 扫描并等待 SCAN_DONE。
 * 使用 block=false，避免在已连接 / 省电模式下 esp_wifi_scan_start(..., true) 长时间阻塞。
 * 仅在本函数置位 s_scan_wait_active 期间响应 SCAN_DONE，避免 WifiStation 误唤醒。
 *
 * @param scan_config 扫描参数（信道、时长等）
 * @param error_out   失败时写入中文提示（可为 nullptr）
 * @return 扫描成功且收到 SCAN_DONE 返回 true
 */
static bool RunWifiScan(const wifi_scan_config_t& scan_config, std::string* error_out)
{
    EnsureScanDoneHandler();
    ClearScanCache();
    xSemaphoreTake(s_scan_done_sem, 0);
    s_scan_wait_active.store(false, std::memory_order_release);

    s_scan_wait_active.store(true, std::memory_order_release);
    esp_err_t err = esp_wifi_scan_start(&scan_config, false);
    if (err == ESP_ERR_WIFI_STATE) {
        /* 驱动忙：停掉残留扫描后再试一次 */
        esp_wifi_scan_stop();
        vTaskDelay(pdMS_TO_TICKS(80));
        err = esp_wifi_scan_start(&scan_config, false);
    }
    if (err != ESP_OK) {
        s_scan_wait_active.store(false, std::memory_order_release);
        ESP_LOGE(TAG, "scan start failed: %s", esp_err_to_name(err));
        if (error_out != nullptr) {
            *error_out = "扫描失败";
        }
        return false;
    }

    ESP_LOGI(TAG, "scan started, wait up to %d ms", kScanWaitTimeoutMs);
    if (xSemaphoreTake(s_scan_done_sem, pdMS_TO_TICKS(kScanWaitTimeoutMs)) != pdTRUE) {
        s_scan_wait_active.store(false, std::memory_order_release);
        esp_wifi_scan_stop();
        ESP_LOGW(TAG, "scan timeout");
        if (error_out != nullptr) {
            *error_out = "扫描超时";
        }
        return false;
    }
    ESP_LOGI(TAG, "scan done (waiting on cached AP list)");
    return true;
}

}  // namespace

void RegisterWifiScanEventHandler()
{
    EnsureScanDoneHandler();
}

/** 构造 Presenter 并绑定 View。 */
WifiPagePresenter::WifiPagePresenter(IWifiPageView* view) : view_(view) {}

/** 更新内部 model 并驱动 View 刷新界面。 */
void WifiPagePresenter::Show(const WifiPageModel& model)
{
    model_ = model;
    if (view_ != nullptr) {
        view_->Show(model_);
    }
}

/** 页面首次展示：检查 Wi-Fi 就绪后触发一次扫描。 */
void WifiPagePresenter::OnShow()
{
    ESP_LOGI(TAG, "OnShow (free int=%u)", static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
    auto& wifi = WifiManager::GetInstance();
    if (!wifi.IsInitialized()) {
        model_.status_line = "Wi-Fi 未就绪";
        model_.scanning = false;
        Show(model_);
        return;
    }
    UpdateCurrentConnectionStatus(model_);
    StartScan();
}

/** 处理返回、重新扫描、列表行点击等触摸事件（依据控件 user_data 区分）。 */
void WifiPagePresenter::OnClick(lv_event_t* e)
{
    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const uintptr_t ud = reinterpret_cast<uintptr_t>(lv_obj_get_user_data(target));

    if (ud == kUserDataBack) {
        NavigateBack();
        return;
    }
    if (ud == kUserDataRefresh) {
        StartScan();
        return;
    }
    if (ud >= kUserDataRowBase && ud < kUserDataRowBase + 100) {
        const int row = static_cast<int>(ud - kUserDataRowBase);
        ConnectToNetwork(row);
    }
}

/** 投递路由命令，关闭当前页并回到上一页。 */
void WifiPagePresenter::NavigateBack()
{
    UiPageRouter::Instance().PostNavigateBack();
}

/** 在后台执行 esp_wifi 扫描，扫描期间置 scanning 并更新列表占位。 */
void WifiPagePresenter::StartScan()
{
    if (busy_) {
        ESP_LOGW(TAG, "StartScan ignored: busy");
        return;
    }
    auto& wifi = WifiManager::GetInstance();
    if (!wifi.IsInitialized()) {
        model_.status_line = "Wi-Fi 未就绪";
        model_.scanning = false;
        Show(model_);
        return;
    }

    busy_ = true;
    model_.scanning = true;
    model_.networks.clear();
    UpdateCurrentConnectionStatus(model_);
    if (model_.status_line.find("扫描") == std::string::npos) {
        model_.status_line += " · 扫描中…";
    }
    Show(model_);
    ESP_LOGI(TAG, "StartScan -> background task");
    RunBackgroundTask(ScanTask);
}

/** 用户点选列表项：已连接则提示；加密未保存则引导 App 配网；否则后台连接。 */
void WifiPagePresenter::ConnectToNetwork(int row_index)
{
    if (busy_ || row_index < 0 || row_index >= static_cast<int>(model_.networks.size())) {
        return;
    }

    const WifiNetworkRow& row = model_.networks[row_index];
    if (row.connected) {
        model_.status_line = "已连接：" + row.ssid;
        Show(model_);
        return;
    }

    if (row.encrypted && !row.saved) {
        model_.status_line = "请先在 App 中配置：" + row.ssid;
        Show(model_);
        return;
    }

    pending_connect_index_ = row_index;
    busy_ = true;
    model_.status_line = "正在连接 " + row.ssid + "…";
    Show(model_);
    RunBackgroundTask(ConnectTask);
}

/** 在独立 FreeRTOS 任务中执行 worker，避免阻塞 LVGL / UI 线程。 */
void WifiPagePresenter::RunBackgroundTask(void (*worker)(WifiPagePresenter* self))
{
    struct TaskCtx {
        WifiPagePresenter* self;
        void (*worker)(WifiPagePresenter*);
    };
    auto* ctx = new TaskCtx{this, worker};
    if (ctx == nullptr) {
        ESP_LOGE(TAG, "RunBackgroundTask: alloc ctx failed");
        UiCommandDispatcher::Instance().Post([this]() {
            busy_ = false;
            model_.scanning = false;
            model_.status_line = "内存不足，操作失败";
            Show(model_);
        });
        return;
    }

    const BaseType_t ok = xTaskCreate(
        [](void* p) {
            auto* task_ctx = static_cast<TaskCtx*>(p);
            ESP_LOGI(TAG, "wifi_ui task running");
            if (task_ctx != nullptr) {
                if (task_ctx->worker != nullptr && task_ctx->self != nullptr) {
                    task_ctx->worker(task_ctx->self);
                }
                delete task_ctx;
            }
            vTaskDelete(nullptr);
        },
        "wifi_ui",
        kWifiUiTaskStackWords,
        ctx,
        5,
        nullptr);

    if (ok == pdPASS) {
        return;
    }

    ESP_LOGE(TAG, "wifi_ui task create failed (free int=%u, need~%u words stack)",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(kWifiUiTaskStackWords));
    delete ctx;

    UiCommandDispatcher::Instance().Post([this]() {
        busy_ = false;
        model_.scanning = false;
        model_.status_line = "内存不足，操作失败";
        Show(model_);
    });
}

/** 判断 SSID 是否已在 SsidManager（NVS）中保存。 */
bool WifiPagePresenter::IsSsidSaved(const std::string& ssid)
{
    for (const auto& item : SsidManager::GetInstance().GetSsidList()) {
        if (item.ssid == ssid) {
            return true;
        }
    }
    return false;
}

/** 从已保存列表中查找指定 SSID 的密码；未找到返回空串。 */
std::string WifiPagePresenter::FindSavedPassword(const std::string& ssid)
{
    for (const auto& item : SsidManager::GetInstance().GetSsidList()) {
        if (item.ssid == ssid) {
            return item.password;
        }
    }
    return "";
}

/**
 * 后台扫描任务：非阻塞 esp_wifi_scan_start + 等待 SCAN_DONE，去重并按 RSSI 排序；
 * 完成后通过 UiCommandDispatcher 回 UI 线程更新 model 与列表。
 */
void WifiPagePresenter::ScanTask(WifiPagePresenter* self)
{
    ESP_LOGI(TAG, "ScanTask begin");
    std::vector<ScanAp> results;
    std::string error;

    auto& wifi = WifiManager::GetInstance();
    if (!wifi.IsInitialized()) {
        error = "Wi-Fi 未就绪";
    } else {
        wifi_scan_config_t scan_config = {};
        scan_config.show_hidden = false;
        scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
        scan_config.scan_time.active.min = 120;
        scan_config.scan_time.active.max = 400;

        for (int attempt = 0; attempt < kMaxScanAttempts; ++attempt) {
            if (attempt > 0) {
                ESP_LOGW(TAG, "scan empty, retry %d/%d", attempt + 1, kMaxScanAttempts);
                vTaskDelay(pdMS_TO_TICKS(300));
            }
            error.clear();
            if (!RunWifiScan(scan_config, &error)) {
                ESP_LOGE(TAG, "scan failed: %s", error.c_str());
                break;
            }
            TakeScanCache(results);
            if (!results.empty()) {
                break;
            }
        }
    }

    const std::string connected_ssid =
        wifi.IsInitialized() && wifi.IsConnected() ? wifi.GetSsid() : std::string();

    UiCommandDispatcher::Instance().Post([self, results = std::move(results), error, connected_ssid]() mutable {
        self->busy_ = false;
        self->model_.scanning = false;
        self->model_.networks.clear();

        if (!error.empty()) {
            self->model_.status_line = error;
            self->Show(self->model_);
            return;
        }

        UpdateCurrentConnectionStatus(self->model_);
        for (const ScanAp& ap : results) {
            WifiNetworkRow row;
            row.ssid = ap.ssid;
            row.rssi = ap.rssi;
            row.encrypted = ap.encrypted;
            row.saved = IsSsidSaved(ap.ssid);
            row.connected = !connected_ssid.empty() && connected_ssid == ap.ssid;
            self->model_.networks.push_back(row);
        }

        if (self->model_.networks.empty()) {
            self->model_.status_line = "未找到可用 Wi-Fi";
        }
        ESP_LOGI(TAG, "ScanTask UI update: %u AP(s), status=%s",
                 static_cast<unsigned>(self->model_.networks.size()), self->model_.status_line.c_str());
        self->Show(self->model_);
    });
}

/**
 * 后台连接任务：写入 SsidManager，退出配网 AP（若在配网模式），重启 Station；
 * 结果投递到 UI 线程更新状态，成功后再触发一次扫描以刷新列表。
 */
void WifiPagePresenter::ConnectTask(WifiPagePresenter* self)
{
    const int index = self->pending_connect_index_;
    self->pending_connect_index_ = -1;

    std::string ssid;
    std::string password;
    std::string error;

    if (index < 0 || index >= static_cast<int>(self->model_.networks.size())) {
        error = "无效网络";
    } else {
        const WifiNetworkRow& row = self->model_.networks[index];
        ssid = row.ssid;
        if (row.encrypted) {
            password = FindSavedPassword(ssid);
            if (password.empty()) {
                error = "请先在 App 中配置密码";
            }
        }
    }

    if (error.empty()) {
        SsidManager::GetInstance().AddSsid(ssid, password);
        auto& wifi = WifiManager::GetInstance();
        if (wifi.IsConfigMode()) {
            wifi.StopConfigAp();
        }
        wifi.StopStation();
        wifi.StartStation();
    }

    UiCommandDispatcher::Instance().Post([self, ssid, error]() {
        self->busy_ = false;
        if (!error.empty()) {
            self->model_.status_line = error;
        } else {
            self->model_.status_line = "正在连接 " + ssid + "…";
            auto& wifi = WifiManager::GetInstance();
            if (wifi.IsConnected() && wifi.GetSsid() == ssid) {
                self->model_.status_line = "已连接：" + ssid + "  " + wifi.GetIpAddress();
            }
        }
        self->Show(self->model_);
        if (error.empty()) {
            self->StartScan();
        }
    });
}

}  // namespace ui::wifi
