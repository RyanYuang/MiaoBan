#include "oye_ble_commands.h"

#include "application.h"
#include <board.h>
#include <esp_heap_caps.h>
#include <esp_app_desc.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include <ssid_manager.h>
#include <system_info.h>
#include <wifi_manager.h>

#include <cstring>

#if CONFIG_USE_OYE_BLE_PROVISIONING
#include <host/ble_store.h>
#endif

#include "ota_snapshot.h"
#include "settings.h"

namespace oye::ble {

namespace {

void SetError(oye_device_v1_Envelope& response, oye_device_v1_ErrorCode code, const char* msg) {
    response.error = code;
    strncpy(response.error_message, msg ? msg : "", sizeof(response.error_message) - 1);
}

bool RequiresBond(oye_device_v1_Command cmd) {
    switch (cmd) {
        case oye_device_v1_Command_CMD_SET_WIFI:
        case oye_device_v1_Command_CMD_SET_USER_TOKEN:
        case oye_device_v1_Command_CMD_UNPAIR:
            return true;
        default:
            return false;
    }
}

void HandleGetDeviceInfo(oye_device_v1_Envelope& response) {
    oye_device_v1_DeviceInfo info = oye_device_v1_DeviceInfo_init_zero;
    strncpy(info.mac, SystemInfo::GetMacAddress().c_str(), sizeof(info.mac) - 1);
    strncpy(info.uuid, Board::GetInstance().GetUuid().c_str(), sizeof(info.uuid) - 1);
#ifdef BOARD_NAME
    strncpy(info.board_name, BOARD_NAME, sizeof(info.board_name) - 1);
#else
    strncpy(info.board_name, "MiaoBan", sizeof(info.board_name) - 1);
#endif
    auto app_desc = esp_app_get_description();
    strncpy(info.fw_version, app_desc->version, sizeof(info.fw_version) - 1);
    strncpy(info.chip_model, SystemInfo::GetChipModelName().c_str(), sizeof(info.chip_model) - 1);

    pb_ostream_t stream =
        pb_ostream_from_buffer(response.payload.bytes, sizeof(response.payload.bytes));
    if (!pb_encode(&stream, oye_device_v1_DeviceInfo_fields, &info)) {
        SetError(response, oye_device_v1_ErrorCode_ERR_INTERNAL, "encode DeviceInfo");
        return;
    }
    response.payload.size = stream.bytes_written;
}

void HandleGetOtaInfo(oye_device_v1_Envelope& response) {
    auto data = OtaSnapshot::GetInstance().GetData();
    oye_device_v1_OtaInfo info = oye_device_v1_OtaInfo_init_zero;
    strncpy(info.current_version, data.current_version.c_str(), sizeof(info.current_version) - 1);
    info.has_update = data.has_update;
    strncpy(info.new_version, data.new_version.c_str(), sizeof(info.new_version) - 1);
    strncpy(info.firmware_url, data.firmware_url.c_str(), sizeof(info.firmware_url) - 1);
    info.last_check_epoch = data.last_check_epoch;
    info.needs_network = data.last_check_epoch == 0;

    pb_ostream_t stream =
        pb_ostream_from_buffer(response.payload.bytes, sizeof(response.payload.bytes));
    if (!pb_encode(&stream, oye_device_v1_OtaInfo_fields, &info)) {
        SetError(response, oye_device_v1_ErrorCode_ERR_INTERNAL, "encode OtaInfo");
    } else {
        response.payload.size = stream.bytes_written;
    }
}

void HandleGetWifi(oye_device_v1_Envelope& response) {
    auto& wifi = WifiManager::GetInstance();
    const auto& saved = SsidManager::GetInstance().GetSsidList();

    oye_device_v1_WifiInfo info = oye_device_v1_WifiInfo_init_zero;
    info.connected = wifi.IsInitialized() && wifi.IsConnected();
    info.in_config_mode = wifi.IsInitialized() && wifi.IsConfigMode();
    info.has_saved_credentials = !saved.empty();
    info.saved_network_count = static_cast<uint32_t>(saved.size());

    if (!saved.empty()) {
        strncpy(info.saved_ssid, saved.front().ssid.c_str(), sizeof(info.saved_ssid) - 1);
    }
    if (info.connected) {
        strncpy(info.current_ssid, wifi.GetSsid().c_str(), sizeof(info.current_ssid) - 1);
        strncpy(info.ip_address, wifi.GetIpAddress().c_str(), sizeof(info.ip_address) - 1);
        info.rssi = wifi.GetRssi();
        info.channel = wifi.GetChannel();
    }

    pb_ostream_t stream =
        pb_ostream_from_buffer(response.payload.bytes, sizeof(response.payload.bytes));
    if (!pb_encode(&stream, oye_device_v1_WifiInfo_fields, &info)) {
        SetError(response, oye_device_v1_ErrorCode_ERR_INTERNAL, "encode WifiInfo");
    } else {
        response.payload.size = stream.bytes_written;
    }
}

void HandleGetUserInfo(oye_device_v1_Envelope& response) {
    auto data = OtaSnapshot::GetInstance().GetData();
    Settings user("user", false);
    bool has_token = !user.GetString("access_token").empty();

    oye_device_v1_UserInfo info = oye_device_v1_UserInfo_init_zero;
    info.activated = !data.has_activation_code && has_token;
    strncpy(info.activation_code, data.activation_code.c_str(), sizeof(info.activation_code) - 1);
    strncpy(info.activation_message, data.activation_message.c_str(),
            sizeof(info.activation_message) - 1);
    info.has_token = has_token;

    pb_ostream_t stream =
        pb_ostream_from_buffer(response.payload.bytes, sizeof(response.payload.bytes));
    if (!pb_encode(&stream, oye_device_v1_UserInfo_fields, &info)) {
        SetError(response, oye_device_v1_ErrorCode_ERR_INTERNAL, "encode UserInfo");
    } else {
        response.payload.size = stream.bytes_written;
    }
}

void HandleSetWifi(const oye_device_v1_Envelope& request, oye_device_v1_Envelope& response) {
    oye_device_v1_SetWifiRequest req = oye_device_v1_SetWifiRequest_init_zero;
    pb_istream_t stream =
        pb_istream_from_buffer(request.payload.bytes, request.payload.size);
    if (!pb_decode(&stream, oye_device_v1_SetWifiRequest_fields, &req)) {
        SetError(response, oye_device_v1_ErrorCode_ERR_INVALID_REQUEST, "bad SetWifiRequest");
        return;
    }
    if (req.ssid[0] == '\0') {
        SetError(response, oye_device_v1_ErrorCode_ERR_INVALID_REQUEST, "empty ssid");
        return;
    }

    SsidManager::GetInstance().AddSsid(req.ssid, req.password);
    auto& wifi = WifiManager::GetInstance();
    if (wifi.IsConfigMode()) {
        wifi.StopConfigAp();
    }
    wifi.StopStation();
    wifi.StartStation();
}

void HandleSetUserToken(const oye_device_v1_Envelope& request, oye_device_v1_Envelope& response) {
    oye_device_v1_SetUserTokenRequest req = oye_device_v1_SetUserTokenRequest_init_zero;
    pb_istream_t stream =
        pb_istream_from_buffer(request.payload.bytes, request.payload.size);
    if (!pb_decode(&stream, oye_device_v1_SetUserTokenRequest_fields, &req)) {
        SetError(response, oye_device_v1_ErrorCode_ERR_INVALID_REQUEST, "bad SetUserTokenRequest");
        return;
    }
    Settings user("user", true);
    user.SetString("access_token", req.access_token);
}

void HandleRefreshOta(oye_device_v1_Envelope& response) {
    auto& wifi = WifiManager::GetInstance();
    if (!wifi.IsInitialized() || !wifi.IsConnected()) {
        SetError(response, oye_device_v1_ErrorCode_ERR_NO_NETWORK, "Wi-Fi not connected");
        return;
    }
    if (Application::GetInstance().GetDeviceState() == kDeviceStateActivating) {
        SetError(response, oye_device_v1_ErrorCode_ERR_BUSY, "activation in progress");
        return;
    }
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < 16384) {
        SetError(response, oye_device_v1_ErrorCode_ERR_BUSY, "low memory");
        return;
    }
    esp_err_t err = OtaSnapshot::GetInstance().RefreshFromServer();
    if (err == ESP_ERR_INVALID_STATE) {
        SetError(response, oye_device_v1_ErrorCode_ERR_BUSY, "ota check in progress");
        return;
    }
    if (err == ESP_ERR_NO_MEM) {
        SetError(response, oye_device_v1_ErrorCode_ERR_BUSY, "low memory");
        return;
    }
    if (err != ESP_OK) {
        SetError(response, oye_device_v1_ErrorCode_ERR_INTERNAL, "CheckVersion failed");
        return;
    }
    HandleGetOtaInfo(response);
}

}  // namespace

void HandleEnvelope(const oye_device_v1_Envelope& request, oye_device_v1_Envelope& response,
                    bool bonded) {
    response = oye_device_v1_Envelope_init_zero;
    response.schema_version = 1;
    response.request_id = request.request_id;
    response.cmd = request.cmd;
    response.error = oye_device_v1_ErrorCode_ERR_OK;

    if (RequiresBond(request.cmd) && !bonded) {
        SetError(response, oye_device_v1_ErrorCode_ERR_NOT_AUTHORIZED, "bond required");
        return;
    }

    switch (request.cmd) {
        case oye_device_v1_Command_CMD_GET_DEVICE_INFO:
            HandleGetDeviceInfo(response);
            break;
        case oye_device_v1_Command_CMD_GET_OTA_INFO:
            HandleGetOtaInfo(response);
            break;
        case oye_device_v1_Command_CMD_GET_USER_INFO:
            HandleGetUserInfo(response);
            break;
        case oye_device_v1_Command_CMD_GET_WIFI:
            HandleGetWifi(response);
            break;
        case oye_device_v1_Command_CMD_SET_WIFI:
            HandleSetWifi(request, response);
            break;
        case oye_device_v1_Command_CMD_SET_USER_TOKEN:
            HandleSetUserToken(request, response);
            break;
        case oye_device_v1_Command_CMD_REFRESH_OTA:
            HandleRefreshOta(response);
            break;
        case oye_device_v1_Command_CMD_START_PAIRING:
            break;
        case oye_device_v1_Command_CMD_UNPAIR: {
            Settings user("user", true);
            user.EraseAll();
#if CONFIG_USE_OYE_BLE_PROVISIONING
            ble_store_clear();
#endif
            break;
        }
        default:
            SetError(response, oye_device_v1_ErrorCode_ERR_INVALID_REQUEST, "unknown cmd");
            break;
    }
}

}  // namespace oye::ble
