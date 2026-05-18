#include "oye_ble_service.h"

#include "oye_ble_codec.h"
#include "oye_ble_commands.h"

#include <esp_bt.h>
#include <esp_log.h>
#include <esp_nimble_hci.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <host/ble_gatt.h>
#include <host/ble_hs.h>
#include <host/ble_uuid.h>
#include <host/util/util.h>
#include <nimble/nimble_npl.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <services/gap/ble_svc_gap.h>
#include <system_info.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#define TAG "OyeBle"

/* 6fa50001-0000-1000-8000-00805f9b34fb */
static const ble_uuid128_t kSvcUuid =
    BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00,
                     0x01, 0x00, 0xa5, 0x6f);

/* cmd: ...0002 */
static const ble_uuid128_t kCmdUuid =
    BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00,
                     0x02, 0x00, 0xa5, 0x6f);

/* rsp: ...0003 */
static const ble_uuid128_t kRspUuid =
    BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00,
                     0x03, 0x00, 0xa5, 0x6f);

static constexpr UBaseType_t kWorkerPriority = 5;
static constexpr uint32_t kWorkerStackWords = 8192;
static constexpr size_t kCmdQueueDepth = 4;

static uint16_t g_cmd_handle;
static uint16_t g_rsp_handle;
static uint16_t g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool g_bonded = false;
static bool g_host_started = false;
static uint8_t g_own_addr_type = BLE_OWN_ADDR_PUBLIC;

static QueueHandle_t g_cmd_queue = nullptr;
static TaskHandle_t g_worker_task = nullptr;
static volatile bool g_worker_running = false;

static SemaphoreHandle_t g_notify_sem = nullptr;
static struct ble_npl_event g_notify_ev;
static std::vector<std::vector<uint8_t>> g_notify_chunks;

extern "C" void ble_store_config_init(void);

OyeBleService& OyeBleService::GetInstance() {
    static OyeBleService instance;
    return instance;
}

static void NotifyEvHandler(struct ble_npl_event* ev);

static void InitNotifyEvent() {
    static bool inited = false;
    if (inited) {
        return;
    }
    ble_npl_event_init(&g_notify_ev, NotifyEvHandler, nullptr);
    g_notify_sem = xSemaphoreCreateBinary();
    inited = true;
}

static void NotifyEvHandler(struct ble_npl_event* ev) {
    (void)ev;
    const uint16_t conn = g_conn_handle;
    for (const auto& chunk : g_notify_chunks) {
        if (conn == BLE_HS_CONN_HANDLE_NONE) {
            break;
        }
        struct os_mbuf* om = ble_hs_mbuf_from_flat(chunk.data(), chunk.size());
        if (om == nullptr) {
            ESP_LOGE(TAG, "notify mbuf alloc failed");
            break;
        }
        int rc = ble_gatts_notify_custom(conn, g_rsp_handle, om);
        if (rc != 0) {
            ESP_LOGE(TAG, "notify failed: %d", rc);
            break;
        }
    }
    g_notify_chunks.clear();
    if (g_notify_sem != nullptr) {
        xSemaphoreGive(g_notify_sem);
    }
}

static int SendResponse(const oye_device_v1_Envelope& response) {
    if (g_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return BLE_HS_ENOTCONN;
    }

    std::vector<uint8_t> payload;
    if (!oye::ble::EncodeEnvelope(response, payload)) {
        return BLE_HS_EINVAL;
    }

    g_notify_chunks = oye::ble::EncodeChunks(payload.data(), payload.size());
    if (g_notify_chunks.empty() && !payload.empty()) {
        return BLE_HS_EINVAL;
    }

    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &g_notify_ev);
    if (xSemaphoreTake(g_notify_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "notify wait timeout");
        g_notify_chunks.clear();
        return BLE_HS_ETIMEOUT;
    }
    return 0;
}

static void ProcessEnvelopePayload(const std::vector<uint8_t>& payload) {
    auto request = std::make_unique<oye_device_v1_Envelope>();
    auto response = std::make_unique<oye_device_v1_Envelope>();
    *request = oye_device_v1_Envelope_init_zero;
    *response = oye_device_v1_Envelope_init_zero;

    if (!oye::ble::DecodeEnvelope(payload, *request)) {
        response->schema_version = 1;
        response->error = oye_device_v1_ErrorCode_ERR_INVALID_REQUEST;
        strncpy(response->error_message, "bad envelope", sizeof(response->error_message) - 1);
        SendResponse(*response);
        return;
    }

    oye::ble::HandleEnvelope(*request, *response, g_bonded);
    SendResponse(*response);
}

static void DrainCmdQueue() {
    if (g_cmd_queue == nullptr) {
        return;
    }
    std::vector<uint8_t>* job = nullptr;
    while (xQueueReceive(g_cmd_queue, &job, 0) == pdTRUE) {
        delete job;
    }
}

static bool EnqueueCmdJob(std::vector<uint8_t>* job) {
    if (g_cmd_queue == nullptr || job == nullptr) {
        delete job;
        return false;
    }
    if (xQueueSend(g_cmd_queue, &job, 0) != pdTRUE) {
        ESP_LOGW(TAG, "cmd queue full, dropping request");
        delete job;
        return false;
    }
    return true;
}

static void WorkerTask(void* arg) {
    (void)arg;
    ESP_LOGI(TAG, "cmd worker started");
    while (g_worker_running) {
        std::vector<uint8_t>* job = nullptr;
        if (xQueueReceive(g_cmd_queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (job == nullptr) {
            break;
        }
        ProcessEnvelopePayload(*job);
        delete job;
    }
    ESP_LOGI(TAG, "cmd worker stopped");
    g_worker_task = nullptr;
    vTaskDelete(nullptr);
}

static esp_err_t StartWorker() {
    if (g_notify_sem == nullptr) {
        g_notify_sem = xSemaphoreCreateBinary();
        if (g_notify_sem == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (g_cmd_queue == nullptr) {
        g_cmd_queue = xQueueCreate(kCmdQueueDepth, sizeof(std::vector<uint8_t>*));
        if (g_cmd_queue == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (g_worker_task != nullptr) {
        return ESP_OK;
    }
    g_worker_running = true;
    BaseType_t ok = xTaskCreate(WorkerTask, "oye_ble_cmd", kWorkerStackWords, nullptr,
                                kWorkerPriority, &g_worker_task);
    if (ok != pdPASS) {
        g_worker_running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void StopWorker() {
    if (g_worker_task == nullptr) {
        return;
    }
    g_worker_running = false;
    std::vector<uint8_t>* sentinel = nullptr;
    xQueueSend(g_cmd_queue, &sentinel, portMAX_DELAY);
    for (int i = 0; i < 50 && g_worker_task != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    DrainCmdQueue();
}

static int RspChrAccess(uint16_t conn_handle, uint16_t attr_handle,
                        struct ble_gatt_access_ctxt* ctxt, void* arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    switch (ctxt->op) {
        case BLE_GATT_ACCESS_OP_READ_CHR:
            return BLE_ATT_ERR_READ_NOT_PERMITTED;
        case BLE_GATT_ACCESS_OP_WRITE_CHR:
            return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
        default:
            return 0;
    }
}

static int GattAccess(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt* ctxt, void* arg) {
    (void)conn_handle;
    (void)arg;

    if (attr_handle != g_cmd_handle) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    const uint16_t om_len = OS_MBUF_PKTLEN(ctxt->om);
    if (om_len == 0) {
        return 0;
    }

    std::vector<uint8_t> buf(om_len);
    const int rc = ble_hs_mbuf_to_flat(ctxt->om, buf.data(), buf.size(), nullptr);
    if (rc != 0) {
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    std::vector<uint8_t> complete;
    if (!oye::ble::FeedChunk(buf.data(), buf.size(), complete)) {
        return 0;
    }

    EnqueueCmdJob(new std::vector<uint8_t>(std::move(complete)));
    return 0;
}

static const struct ble_gatt_chr_def kChars[] = {
    {
        .uuid = &kCmdUuid.u,
        .access_cb = GattAccess,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
        .val_handle = &g_cmd_handle,
    },
    {
        .uuid = &kRspUuid.u,
        .access_cb = RspChrAccess,
        .flags = BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &g_rsp_handle,
    },
    {0},
};

static const struct ble_gatt_svc_def kSvcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &kSvcUuid.u,
        .characteristics = kChars,
    },
    {0},
};

static void GattRegister(struct ble_gatt_register_ctxt* ctxt, void* arg) {
    (void)arg;
    char buf[BLE_UUID_STR_LEN];
    switch (ctxt->op) {
        case BLE_GATT_REGISTER_OP_SVC:
            ESP_LOGI(TAG, "registered service %s", ble_uuid_to_str(ctxt->svc.svc_def->uuid, buf));
            break;
        case BLE_GATT_REGISTER_OP_CHR:
            ESP_LOGI(TAG, "registered chr %s val_handle=%d",
                     ble_uuid_to_str(ctxt->chr.chr_def->uuid, buf), ctxt->chr.val_handle);
            break;
        default:
            break;
    }
}

static void SetDeviceName() {
    std::string mac = SystemInfo::GetMacAddress();
    std::string suffix = "0000";
    if (mac.size() >= 5) {
        suffix = mac.substr(mac.size() - 5);
        for (auto& c : suffix) {
            if (c == ':') {
                c = '0';
            }
        }
    }
    std::string name = "Oye-" + suffix;
    ble_svc_gap_device_name_set(name.c_str());
}

static int GapEvent(struct ble_gap_event* event, void* arg);

static bool StartAdvertising() {
    int rc = ble_hs_id_infer_auto(0, &g_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer addr type failed: %d", rc);
        return false;
    }

    struct ble_gap_adv_params adv_params = {};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    struct ble_hs_adv_fields adv_fields = {};
    adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv_fields.name = reinterpret_cast<const uint8_t*>(ble_svc_gap_device_name());
    adv_fields.name_len = strlen(ble_svc_gap_device_name());
    adv_fields.name_is_complete = 1;

    rc = ble_gap_adv_set_fields(&adv_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields failed: %d", rc);
        return false;
    }

    ble_uuid128_t svc = kSvcUuid;
    struct ble_hs_adv_fields rsp_fields = {};
    rsp_fields.uuids128 = &svc;
    rsp_fields.num_uuids128 = 1;
    rsp_fields.uuids128_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_rsp_set_fields failed: %d", rc);
        return false;
    }

    rc = ble_gap_adv_start(g_own_addr_type, nullptr, BLE_HS_FOREVER, &adv_params, GapEvent,
                           nullptr);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start failed: %d", rc);
        return false;
    }
    return true;
}

static int GapEvent(struct ble_gap_event* event, void* arg) {
    (void)arg;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                g_conn_handle = event->connect.conn_handle;
                g_bonded = false;
                ESP_LOGI(TAG, "connected handle=%d", g_conn_handle);
            } else {
                StartAdvertising();
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "disconnected reason=%d", event->disconnect.reason);
            g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            g_bonded = false;
            oye::ble::ResetChunkAssembler();
            DrainCmdQueue();
            StartAdvertising();
            break;
        case BLE_GAP_EVENT_ENC_CHANGE:
            if (event->enc_change.status == 0) {
                g_bonded = true;
                ESP_LOGI(TAG, "link encrypted (bonded)");
            }
            break;
        case BLE_GAP_EVENT_CONN_UPDATE:
        case BLE_GAP_EVENT_SUBSCRIBE:
            break;
        default:
            break;
    }
    return 0;
}

static void OnReset(int reason) {
    ESP_LOGE(TAG, "NimBLE reset reason=%d", reason);
}

static int GattSvrInit() {
    int rc = ble_gatts_count_cfg(kSvcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts_count_cfg failed: %d", rc);
        return rc;
    }
    rc = ble_gatts_add_svcs(kSvcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts_add_svcs failed: %d", rc);
    }
    return rc;
}

static void OnSync() {
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure_addr failed: %d", rc);
        return;
    }
    if (StartAdvertising()) {
        ESP_LOGI(TAG, "BLE stack synced, advertising");
    } else {
        ESP_LOGE(TAG, "BLE stack synced but advertising failed");
    }
}

static void HostTask(void* param) {
    (void)param;
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static esp_err_t ControllerInit() {
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        return ret;
    }
    return esp_bt_controller_enable(ESP_BT_MODE_BLE);
}

static esp_err_t ControllerDeinit() {
    esp_err_t ret = esp_bt_controller_disable();
    if (ret != ESP_OK) {
        return ret;
    }
    return esp_bt_controller_deinit();
}

esp_err_t OyeBleService::Start() {
    if (running_) {
        return ESP_OK;
    }

    esp_err_t ret = ControllerInit();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "controller init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_nimble_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble init failed: %s", esp_err_to_name(ret));
        ControllerDeinit();
        return ret;
    }

    InitNotifyEvent();

    ble_hs_cfg.reset_cb = OnReset;
    ble_hs_cfg.sync_cb = OnSync;
    ble_hs_cfg.gatts_register_cb = GattRegister;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_mitm = 0;

    ble_svc_gap_init();
    SetDeviceName();
    ble_store_config_init();

    int rc = GattSvrInit();
    if (rc != 0) {
        esp_nimble_deinit();
        ControllerDeinit();
        return ESP_FAIL;
    }

    ret = StartWorker();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "worker start failed: %s", esp_err_to_name(ret));
        esp_nimble_deinit();
        ControllerDeinit();
        return ret;
    }

    nimble_port_freertos_init(HostTask);

    g_host_started = true;
    running_ = true;
    ESP_LOGI(TAG, "Oye BLE service started");
    return ESP_OK;
}

esp_err_t OyeBleService::Stop() {
    if (!running_) {
        return ESP_OK;
    }
    running_ = false;

    if (g_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(g_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    }
    ble_gap_adv_stop();

    if (g_host_started) {
        nimble_port_stop();
        esp_nimble_deinit();
        g_host_started = false;
    }
    ControllerDeinit();
    StopWorker();

    g_bonded = false;
    oye::ble::ResetChunkAssembler();
    ESP_LOGI(TAG, "Oye BLE service stopped");
    return ESP_OK;
}
