#ifndef OYE_BLE_SERVICE_H
#define OYE_BLE_SERVICE_H

#include <esp_err.h>

class OyeBleService {
public:
    static OyeBleService& GetInstance();

    esp_err_t Start();
    esp_err_t Stop();
    bool IsRunning() const { return running_; }

    OyeBleService(const OyeBleService&) = delete;
    OyeBleService& operator=(const OyeBleService&) = delete;

private:
    OyeBleService() = default;

    bool running_ = false;
};

#endif
