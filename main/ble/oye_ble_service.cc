#include "oye_ble_service.h"

#include "oye_ble_codec.h"
#include "oye_ble_commands.h"

#include <esp_bt.h>
#include <esp_log.h>
#include <esp_nimble_hci.h>
#include <esp_timer.h>
#include <host/ble_gatt.h>
#include <host/ble_hs.h>
#include <host/ble_uuid.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <services/gap/ble_svc_gap.h>
#include <system_info.h>

#include <cstring>
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

static constexpr int kIdleTimeoutSec = 300;

static uint16_t g_cmd_handle;
static uint16_t g_rsp_handle;
static uint16_t g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool g_bonded = false;
static esp_timer_handle_t g_idle_timer = nullptr;
static bool g_host_started = false;

extern "C" void ble_store_config_init(void);

OyeBleService& OyeBleService::GetInstance() {
    static OyeBleService instance;
    return instance;
}

static void StopIdleTimer() {
    if (g_idle_timer != nullptr) {
        esp_timer_stop(g_idle_timer);
    }
}

static void ResetIdleTimer();

static void OnIdleTimeout(void*) {
    ESP_LOGI(TAG, "BLE idle timeout, stopping");
    OyeBleService::GetInstance().Stop();
}

static void EnsureIdleTimer() {
    if (g_idle_timer != nullptr) {
        return;
    }
    esp_timer_create_args_t args = {
        .callback = OnIdleTimeout,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "oye_ble_idle",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&args, &g_idle_timer);
}

static void ResetIdleTimer() {
    EnsureIdleTimer();
    esp_timer_stop(g_idle_timer);
    esp_timer_start_once(g_idle_timer, kIdleTimeoutSec * 1000000LL);
}

static int SendResponse(const oye_device_v1_Envelope& response) {
    if (g_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return BLE_HS_ENOTCONN;
    }
    std::vector<uint8_t> payload;
    if (!oye::ble::EncodeEnvelope(response, payload)) {
        return BLE_HS_EINVAL;
    }
    auto chunks = oye::ble::EncodeChunks(payload.data(), payload.size());
    for (const auto& chunk : chunks) {
        struct os_mbuf* om = ble_hs_mbuf_from_flat(chunk.data(), chunk.size());
        if (om == nullptr) {
            return BLE_HS_ENOMEM;
        }
        int rc = ble_gatts_notify_custom(g_conn_handle, g_rsp_handle, om);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

static void ProcessEnvelopePayload(const std::vector<uint8_t>& payload) {
    oye_device_v1_Envelope request = oye_device_v1_Envelope_init_zero;
    oye_device_v1_Envelope response = oye_device_v1_Envelope_init_zero;
    if (!oye::ble::DecodeEnvelope(payload, request)) {
        response.schema_version = 1;
        response.error = oye_device_v1_ErrorCode_ERR_INVALID_REQUEST;
        strncpy(response.error_message, "bad envelope", sizeof(response.error_message) - 1);
        SendResponse(response);
        return;
    }
    oye::ble::HandleEnvelope(request, response, g_bonded);
    SendResponse(response);
    ResetIdleTimer();
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

    uint16_t om_len = OS_MBUF_PKTLEN(ctxt->om);
    std::vector<uint8_t> buf(om_len);
    int rc = ble_hs_mbuf_to_flat(ctxt->om, buf.data(), buf.size(), nullptr);
    if (rc != 0) {
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    std::vector<uint8_t> complete;
    if (oye::ble::FeedChunk(buf.data(), buf.size(), complete)) {
        ProcessEnvelopePayload(complete);
    }
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
        .access_cb = nullptr,
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

static void StartAdvertising() {
    struct ble_gap_adv_params adv_params = {};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    struct ble_hs_adv_fields fields = {};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = reinterpret_cast<const uint8_t*>(ble_svc_gap_device_name());
    fields.name_len = strlen(ble_svc_gap_device_name());
    fields.name_is_complete = 1;

    ble_uuid128_t svc = kSvcUuid;
    fields.uuids128 = &svc;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields failed: %d", rc);
        return;
    }
    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, nullptr, BLE_HS_FOREVER, &adv_params, nullptr,
                           nullptr);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start failed: %d", rc);
    }
}

static int GapEvent(struct ble_gap_event* event, void* arg) {
    (void)arg;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                g_conn_handle = event->connect.conn_handle;
                g_bonded = false;
                ESP_LOGI(TAG, "connected handle=%d", g_conn_handle);
                ResetIdleTimer();
            } else {
                StartAdvertising();
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "disconnected reason=%d", event->disconnect.reason);
            g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            g_bonded = false;
            oye::ble::ResetChunkAssembler();
            StartAdvertising();
            ResetIdleTimer();
            break;
        case BLE_GAP_EVENT_ENC_CHANGE:
            if (event->enc_change.status == 0) {
                g_bonded = true;
                ESP_LOGI(TAG, "link encrypted (bonded)");
            }
            break;
        case BLE_GAP_EVENT_CONN_UPDATE:
        case BLE_GAP_EVENT_SUBSCRIBE:
            ResetIdleTimer();
            break;
        default:
            break;
    }
    return 0;
}

static void OnReset(int reason) {
    ESP_LOGE(TAG, "NimBLE reset reason=%d", reason);
}

static void OnSync() {
    SetDeviceName();
    int rc = ble_gatts_count_cfg(kSvcs);
    assert(rc == 0);
    rc = ble_gatts_add_svcs(kSvcs);
    assert(rc == 0);
    StartAdvertising();
    ESP_LOGI(TAG, "BLE stack synced, advertising");
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

    ret = esp_nimble_enable(HostTask);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble enable failed: %s", esp_err_to_name(ret));
        esp_nimble_deinit();
        ControllerDeinit();
        return ret;
    }

    g_host_started = true;
    running_ = true;
    ResetIdleTimer();
    ESP_LOGI(TAG, "Oye BLE service started");
    return ESP_OK;
}

esp_err_t OyeBleService::Stop() {
    if (!running_) {
        return ESP_OK;
    }
    StopIdleTimer();
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
    g_bonded = false;
    oye::ble::ResetChunkAssembler();
    ESP_LOGI(TAG, "Oye BLE service stopped");
    return ESP_OK;
}
