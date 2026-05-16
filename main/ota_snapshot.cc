#include "ota_snapshot.h"

#include "ota.h"
#include "settings.h"

#include <esp_log.h>
#include <ctime>
#include <mutex>

#define TAG "OtaSnapshot"

OtaSnapshot& OtaSnapshot::GetInstance() {
    static OtaSnapshot instance;
    return instance;
}

void OtaSnapshot::UpdateFromOta(const Ota& ota) {
    std::lock_guard<std::mutex> lock(mutex_);
    data_.current_version = ota.GetCurrentVersion();
    data_.new_version = ota.GetFirmwareVersion();
    data_.firmware_url = ota.GetFirmwareUrl();
    data_.activation_message = ota.GetActivationMessage();
    data_.activation_code = ota.GetActivationCode();
    data_.has_update = ota.HasNewVersion();
    data_.has_activation_code = ota.HasActivationCode();
    data_.has_mqtt_config = ota.HasMqttConfig();
    data_.has_websocket_config = ota.HasWebsocketConfig();
    data_.last_check_epoch = static_cast<int64_t>(time(nullptr));
    SaveToNvs();
}

OtaSnapshotData OtaSnapshot::GetData() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return data_;
}

void OtaSnapshot::LoadFromNvs() {
    Settings settings("ota_cache", false);
    std::lock_guard<std::mutex> lock(mutex_);
    data_.current_version = settings.GetString("cur_ver");
    data_.new_version = settings.GetString("new_ver");
    data_.firmware_url = settings.GetString("fw_url");
    data_.activation_message = settings.GetString("act_msg");
    data_.activation_code = settings.GetString("act_code");
    data_.has_update = settings.GetBool("has_update", false);
    data_.has_activation_code = settings.GetBool("has_act_code", false);
    data_.has_mqtt_config = settings.GetBool("has_mqtt", false);
    data_.has_websocket_config = settings.GetBool("has_ws", false);
    data_.last_check_epoch = settings.GetInt("last_check", 0);
}

void OtaSnapshot::SaveToNvs() const {
    Settings settings("ota_cache", true);
    settings.SetString("cur_ver", data_.current_version);
    settings.SetString("new_ver", data_.new_version);
    settings.SetString("fw_url", data_.firmware_url);
    settings.SetString("act_msg", data_.activation_message);
    settings.SetString("act_code", data_.activation_code);
    settings.SetBool("has_update", data_.has_update);
    settings.SetBool("has_act_code", data_.has_activation_code);
    settings.SetBool("has_mqtt", data_.has_mqtt_config);
    settings.SetBool("has_ws", data_.has_websocket_config);
    settings.SetInt("last_check", static_cast<int32_t>(data_.last_check_epoch));
}

esp_err_t OtaSnapshot::RefreshFromServer() {
    Ota ota;
    esp_err_t err = ota.CheckVersion();
    if (err == ESP_OK) {
        UpdateFromOta(ota);
    }
    return err;
}
