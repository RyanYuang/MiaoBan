#include "i2c_device.h"

#include <esp_log.h>
#include <cstring>

#define TAG "I2cDevice"
namespace {
constexpr int kI2cRetries = 2;
constexpr int kI2cTimeoutMs = 100;
}


I2cDevice::I2cDevice(i2c_master_bus_handle_t i2c_bus, uint8_t addr) {
    i2c_device_config_t i2c_device_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400 * 1000,
        .scl_wait_us = 0,
        .flags = {
            .disable_ack_check = 0,
        },
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus, &i2c_device_cfg, &i2c_device_));
    assert(i2c_device_ != NULL);
}

void I2cDevice::WriteReg(uint8_t reg, uint8_t value) {
    uint8_t buffer[2] = {reg, value};
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < kI2cRetries; ++attempt) {
        err = i2c_master_transmit(i2c_device_, buffer, sizeof(buffer), kI2cTimeoutMs);
        if (err == ESP_OK) {
            return;
        }
    }
    ESP_LOGE(TAG, "WriteReg failed reg=0x%02x err=%s", reg, esp_err_to_name(err));
}

uint8_t I2cDevice::ReadReg(uint8_t reg) {
    uint8_t buffer[1];
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < kI2cRetries; ++attempt) {
        err = i2c_master_transmit_receive(i2c_device_, &reg, 1, buffer, sizeof(buffer), kI2cTimeoutMs);
        if (err == ESP_OK) {
            return buffer[0];
        }
    }
    ESP_LOGE(TAG, "ReadReg failed reg=0x%02x err=%s", reg, esp_err_to_name(err));
    return buffer[0];
}

void I2cDevice::ReadRegs(uint8_t reg, uint8_t* buffer, size_t length) {
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < kI2cRetries; ++attempt) {
        err = i2c_master_transmit_receive(i2c_device_, &reg, 1, buffer, length, kI2cTimeoutMs);
        if (err == ESP_OK) {
            return;
        }
    }
    std::memset(buffer, 0, length);
    ESP_LOGE(TAG, "ReadRegs failed reg=0x%02x len=%u err=%s", reg, static_cast<unsigned>(length), esp_err_to_name(err));
}