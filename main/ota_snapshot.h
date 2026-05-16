#ifndef OTA_SNAPSHOT_H
#define OTA_SNAPSHOT_H

#include <esp_err.h>
#include <cstdint>
#include <mutex>
#include <string>

class Ota;

struct OtaSnapshotData {
    std::string current_version;
    std::string new_version;
    std::string firmware_url;
    std::string activation_message;
    std::string activation_code;
    bool has_update = false;
    bool has_activation_code = false;
    bool has_mqtt_config = false;
    bool has_websocket_config = false;
    int64_t last_check_epoch = 0;
};

class OtaSnapshot {
public:
    static OtaSnapshot& GetInstance();

    void UpdateFromOta(const Ota& ota);
    OtaSnapshotData GetData() const;
    void LoadFromNvs();
    void SaveToNvs() const;

    /** Run HTTP CheckVersion when Wi-Fi is up; updates cache on success. */
    esp_err_t RefreshFromServer();

private:
    OtaSnapshot() = default;

    mutable std::mutex mutex_;
    OtaSnapshotData data_;
};

#endif
