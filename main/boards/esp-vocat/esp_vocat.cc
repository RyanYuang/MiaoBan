/**
 * @file esp_vocat.cc
 * ESP-VoCat 板级支持（BSP）：I2C/QSPI 显示、CST816 屏触摸、机身电容触摸、BMI270、充电与摄像头等。
 * PCB V1.0 / V1.2 在 DetectPcbVersion() 中探测，并改写下方全局 GPIO 变量以匹配布线。
 */
#include "wifi_board.h"
#include "codecs/box_audio_codec.h"
#include "display/lcd_display.h"
#include "display/emote_display.h"
#if !CONFIG_USE_EMOTE_MESSAGE_STYLE
#include "lvgl_display.h"
#include <esp_lvgl_port.h>
#endif
#include "application.h"
#include "button.h"
#include "config.h"
#include "backlight.h"
#include "esp_video.h"

#include <esp_log.h>
#include <esp_timer.h>
#include "esp_idf_version.h"
#include <cinttypes>

#include <driver/i2c_master.h>
#include <cstdlib>
#include "i2c_device.h"
#include "i2c_bus.h"
#include "bmi270_api.h"
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_st77916.h>
#include "esp_lcd_touch_cst816s.h"
#include "touch.h"

extern "C" {
#include "touch_button_sensor.h"
#include "touch_slider_sensor.h"
}

#include "driver/temperature_sensor.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace {
constexpr char TAG[] = "ESP-VoCat";
}

/** BMI270（I2C）：用于摇晃检测，驱动屏幕短时表情反馈；失败则仅关闭该功能。 */
namespace Bmi270Motion {
static bmi270_handle_t bmi_handle_ = nullptr;

esp_err_t Initialize(i2c_bus_handle_t i2c_bus)
{
    if (bmi_handle_) {
        return ESP_OK;
    }
    if (!i2c_bus) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = bmi270_sensor_create(i2c_bus, &bmi_handle_, bmi270_config_file,
                                         BMI2_GYRO_CROSS_SENS_ENABLE | BMI2_CRT_RTOSK_ENABLE);
    if (ret != ESP_OK || !bmi_handle_) {
        ESP_LOGW(TAG, "BMI270 init failed: %s", esp_err_to_name(ret));
        return ret == ESP_OK ? ESP_FAIL : ret;
    }

    const uint8_t sens_list[] = {BMI2_ACCEL};
    int8_t rslt = bmi270_sensor_enable(sens_list, 1, bmi_handle_);
    if (rslt != BMI2_OK) {
        ESP_LOGW(TAG, "BMI270 accel enable failed: %d", rslt);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "BMI270 initialized");
    return ESP_OK;
}

bool ReadAccelRaw(struct bmi2_sens_data& accel)
{
    if (!bmi_handle_) {
        return false;
    }
    int8_t rslt = bmi2_get_sensor_data(&accel, bmi_handle_);
    return rslt == BMI2_OK;
}
} // namespace Bmi270Motion

/** 片内温度传感器句柄：Charge 任务里与电池寄存器一并轮询（tsens_value）。 */
temperature_sensor_handle_t temp_sensor = NULL;

/** ST77916 面板厂商初始化序列，经 QSPI 在 st77916_vendor_config 中下发；更换屏厂需对照数据手册调整。 */
static const st77916_lcd_init_cmd_t vendor_specific_init_yysj[] = {
    {0xF0, (uint8_t []){0x28}, 1, 0},
    {0xF2, (uint8_t []){0x28}, 1, 0},
    {0x73, (uint8_t []){0xF0}, 1, 0},
    {0x7C, (uint8_t []){0xD1}, 1, 0},
    {0x83, (uint8_t []){0xE0}, 1, 0},
    {0x84, (uint8_t []){0x61}, 1, 0},
    {0xF2, (uint8_t []){0x82}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x01}, 1, 0},
    {0xF1, (uint8_t []){0x01}, 1, 0},
    {0xB0, (uint8_t []){0x56}, 1, 0},
    {0xB1, (uint8_t []){0x4D}, 1, 0},
    {0xB2, (uint8_t []){0x24}, 1, 0},
    {0xB4, (uint8_t []){0x87}, 1, 0},
    {0xB5, (uint8_t []){0x44}, 1, 0},
    {0xB6, (uint8_t []){0x8B}, 1, 0},
    {0xB7, (uint8_t []){0x40}, 1, 0},
    {0xB8, (uint8_t []){0x86}, 1, 0},
    {0xBA, (uint8_t []){0x00}, 1, 0},
    {0xBB, (uint8_t []){0x08}, 1, 0},
    {0xBC, (uint8_t []){0x08}, 1, 0},
    {0xBD, (uint8_t []){0x00}, 1, 0},
    {0xC0, (uint8_t []){0x80}, 1, 0},
    {0xC1, (uint8_t []){0x10}, 1, 0},
    {0xC2, (uint8_t []){0x37}, 1, 0},
    {0xC3, (uint8_t []){0x80}, 1, 0},
    {0xC4, (uint8_t []){0x10}, 1, 0},
    {0xC5, (uint8_t []){0x37}, 1, 0},
    {0xC6, (uint8_t []){0xA9}, 1, 0},
    {0xC7, (uint8_t []){0x41}, 1, 0},
    {0xC8, (uint8_t []){0x01}, 1, 0},
    {0xC9, (uint8_t []){0xA9}, 1, 0},
    {0xCA, (uint8_t []){0x41}, 1, 0},
    {0xCB, (uint8_t []){0x01}, 1, 0},
    {0xD0, (uint8_t []){0x91}, 1, 0},
    {0xD1, (uint8_t []){0x68}, 1, 0},
    {0xD2, (uint8_t []){0x68}, 1, 0},
    {0xF5, (uint8_t []){0x00, 0xA5}, 2, 0},
    {0xDD, (uint8_t []){0x4F}, 1, 0},
    {0xDE, (uint8_t []){0x4F}, 1, 0},
    {0xF1, (uint8_t []){0x10}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x02}, 1, 0},
    {0xE0, (uint8_t []){0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34}, 14, 0},
    {0xE1, (uint8_t []){0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33}, 14, 0},
    {0xF0, (uint8_t []){0x10}, 1, 0},
    {0xF3, (uint8_t []){0x10}, 1, 0},
    {0xE0, (uint8_t []){0x07}, 1, 0},
    {0xE1, (uint8_t []){0x00}, 1, 0},
    {0xE2, (uint8_t []){0x00}, 1, 0},
    {0xE3, (uint8_t []){0x00}, 1, 0},
    {0xE4, (uint8_t []){0xE0}, 1, 0},
    {0xE5, (uint8_t []){0x06}, 1, 0},
    {0xE6, (uint8_t []){0x21}, 1, 0},
    {0xE7, (uint8_t []){0x01}, 1, 0},
    {0xE8, (uint8_t []){0x05}, 1, 0},
    {0xE9, (uint8_t []){0x02}, 1, 0},
    {0xEA, (uint8_t []){0xDA}, 1, 0},
    {0xEB, (uint8_t []){0x00}, 1, 0},
    {0xEC, (uint8_t []){0x00}, 1, 0},
    {0xED, (uint8_t []){0x0F}, 1, 0},
    {0xEE, (uint8_t []){0x00}, 1, 0},
    {0xEF, (uint8_t []){0x00}, 1, 0},
    {0xF8, (uint8_t []){0x00}, 1, 0},
    {0xF9, (uint8_t []){0x00}, 1, 0},
    {0xFA, (uint8_t []){0x00}, 1, 0},
    {0xFB, (uint8_t []){0x00}, 1, 0},
    {0xFC, (uint8_t []){0x00}, 1, 0},
    {0xFD, (uint8_t []){0x00}, 1, 0},
    {0xFE, (uint8_t []){0x00}, 1, 0},
    {0xFF, (uint8_t []){0x00}, 1, 0},
    {0x60, (uint8_t []){0x40}, 1, 0},
    {0x61, (uint8_t []){0x04}, 1, 0},
    {0x62, (uint8_t []){0x00}, 1, 0},
    {0x63, (uint8_t []){0x42}, 1, 0},
    {0x64, (uint8_t []){0xD9}, 1, 0},
    {0x65, (uint8_t []){0x00}, 1, 0},
    {0x66, (uint8_t []){0x00}, 1, 0},
    {0x67, (uint8_t []){0x00}, 1, 0},
    {0x68, (uint8_t []){0x00}, 1, 0},
    {0x69, (uint8_t []){0x00}, 1, 0},
    {0x6A, (uint8_t []){0x00}, 1, 0},
    {0x6B, (uint8_t []){0x00}, 1, 0},
    {0x70, (uint8_t []){0x40}, 1, 0},
    {0x71, (uint8_t []){0x03}, 1, 0},
    {0x72, (uint8_t []){0x00}, 1, 0},
    {0x73, (uint8_t []){0x42}, 1, 0},
    {0x74, (uint8_t []){0xD8}, 1, 0},
    {0x75, (uint8_t []){0x00}, 1, 0},
    {0x76, (uint8_t []){0x00}, 1, 0},
    {0x77, (uint8_t []){0x00}, 1, 0},
    {0x78, (uint8_t []){0x00}, 1, 0},
    {0x79, (uint8_t []){0x00}, 1, 0},
    {0x7A, (uint8_t []){0x00}, 1, 0},
    {0x7B, (uint8_t []){0x00}, 1, 0},
    {0x80, (uint8_t []){0x48}, 1, 0},
    {0x81, (uint8_t []){0x00}, 1, 0},
    {0x82, (uint8_t []){0x06}, 1, 0},
    {0x83, (uint8_t []){0x02}, 1, 0},
    {0x84, (uint8_t []){0xD6}, 1, 0},
    {0x85, (uint8_t []){0x04}, 1, 0},
    {0x86, (uint8_t []){0x00}, 1, 0},
    {0x87, (uint8_t []){0x00}, 1, 0},
    {0x88, (uint8_t []){0x48}, 1, 0},
    {0x89, (uint8_t []){0x00}, 1, 0},
    {0x8A, (uint8_t []){0x08}, 1, 0},
    {0x8B, (uint8_t []){0x02}, 1, 0},
    {0x8C, (uint8_t []){0xD8}, 1, 0},
    {0x8D, (uint8_t []){0x04}, 1, 0},
    {0x8E, (uint8_t []){0x00}, 1, 0},
    {0x8F, (uint8_t []){0x00}, 1, 0},
    {0x90, (uint8_t []){0x48}, 1, 0},
    {0x91, (uint8_t []){0x00}, 1, 0},
    {0x92, (uint8_t []){0x0A}, 1, 0},
    {0x93, (uint8_t []){0x02}, 1, 0},
    {0x94, (uint8_t []){0xDA}, 1, 0},
    {0x95, (uint8_t []){0x04}, 1, 0},
    {0x96, (uint8_t []){0x00}, 1, 0},
    {0x97, (uint8_t []){0x00}, 1, 0},
    {0x98, (uint8_t []){0x48}, 1, 0},
    {0x99, (uint8_t []){0x00}, 1, 0},
    {0x9A, (uint8_t []){0x0C}, 1, 0},
    {0x9B, (uint8_t []){0x02}, 1, 0},
    {0x9C, (uint8_t []){0xDC}, 1, 0},
    {0x9D, (uint8_t []){0x04}, 1, 0},
    {0x9E, (uint8_t []){0x00}, 1, 0},
    {0x9F, (uint8_t []){0x00}, 1, 0},
    {0xA0, (uint8_t []){0x48}, 1, 0},
    {0xA1, (uint8_t []){0x00}, 1, 0},
    {0xA2, (uint8_t []){0x05}, 1, 0},
    {0xA3, (uint8_t []){0x02}, 1, 0},
    {0xA4, (uint8_t []){0xD5}, 1, 0},
    {0xA5, (uint8_t []){0x04}, 1, 0},
    {0xA6, (uint8_t []){0x00}, 1, 0},
    {0xA7, (uint8_t []){0x00}, 1, 0},
    {0xA8, (uint8_t []){0x48}, 1, 0},
    {0xA9, (uint8_t []){0x00}, 1, 0},
    {0xAA, (uint8_t []){0x07}, 1, 0},
    {0xAB, (uint8_t []){0x02}, 1, 0},
    {0xAC, (uint8_t []){0xD7}, 1, 0},
    {0xAD, (uint8_t []){0x04}, 1, 0},
    {0xAE, (uint8_t []){0x00}, 1, 0},
    {0xAF, (uint8_t []){0x00}, 1, 0},
    {0xB0, (uint8_t []){0x48}, 1, 0},
    {0xB1, (uint8_t []){0x00}, 1, 0},
    {0xB2, (uint8_t []){0x09}, 1, 0},
    {0xB3, (uint8_t []){0x02}, 1, 0},
    {0xB4, (uint8_t []){0xD9}, 1, 0},
    {0xB5, (uint8_t []){0x04}, 1, 0},
    {0xB6, (uint8_t []){0x00}, 1, 0},
    {0xB7, (uint8_t []){0x00}, 1, 0},
    {0xB8, (uint8_t []){0x48}, 1, 0},
    {0xB9, (uint8_t []){0x00}, 1, 0},
    {0xBA, (uint8_t []){0x0B}, 1, 0},
    {0xBB, (uint8_t []){0x02}, 1, 0},
    {0xBC, (uint8_t []){0xDB}, 1, 0},
    {0xBD, (uint8_t []){0x04}, 1, 0},
    {0xBE, (uint8_t []){0x00}, 1, 0},
    {0xBF, (uint8_t []){0x00}, 1, 0},
    {0xC0, (uint8_t []){0x10}, 1, 0},
    {0xC1, (uint8_t []){0x47}, 1, 0},
    {0xC2, (uint8_t []){0x56}, 1, 0},
    {0xC3, (uint8_t []){0x65}, 1, 0},
    {0xC4, (uint8_t []){0x74}, 1, 0},
    {0xC5, (uint8_t []){0x88}, 1, 0},
    {0xC6, (uint8_t []){0x99}, 1, 0},
    {0xC7, (uint8_t []){0x01}, 1, 0},
    {0xC8, (uint8_t []){0xBB}, 1, 0},
    {0xC9, (uint8_t []){0xAA}, 1, 0},
    {0xD0, (uint8_t []){0x10}, 1, 0},
    {0xD1, (uint8_t []){0x47}, 1, 0},
    {0xD2, (uint8_t []){0x56}, 1, 0},
    {0xD3, (uint8_t []){0x65}, 1, 0},
    {0xD4, (uint8_t []){0x74}, 1, 0},
    {0xD5, (uint8_t []){0x88}, 1, 0},
    {0xD6, (uint8_t []){0x99}, 1, 0},
    {0xD7, (uint8_t []){0x01}, 1, 0},
    {0xD8, (uint8_t []){0xBB}, 1, 0},
    {0xD9, (uint8_t []){0xAA}, 1, 0},
    {0xF3, (uint8_t []){0x01}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0x21, (uint8_t []){}, 0, 0},
    {0x11, (uint8_t []){}, 0, 0},
    {0x00, (uint8_t []){}, 0, 120},
};

/** 最近一次片内温度读数（摄氏度），由 Charge::Printcharge 更新。 */
float tsens_value;

/** 默认按 V1.0 引脚；DetectPcbVersion() 判定为 V1.2 时改为 _2 后缀宏对应 GPIO。 */
gpio_num_t AUDIO_I2S_GPIO_DIN = AUDIO_I2S_GPIO_DIN_1;
gpio_num_t AUDIO_CODEC_PA_PIN = AUDIO_CODEC_PA_PIN_1;
gpio_num_t QSPI_PIN_NUM_LCD_RST = QSPI_PIN_NUM_LCD_RST_1;
gpio_num_t TOUCH_PAD2 = TOUCH_PAD2_1;
gpio_num_t UART1_TX = UART1_TX_1;
gpio_num_t UART1_RX = UART1_RX_1;

/** 电池侧充电管理芯片（I2C 0x55），周期读寄存器；电压/电流字段预留。 */
class Charge : public I2cDevice {
public:
    Charge(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr)
    {
        read_buffer_ = new uint8_t[8];
    }
    ~Charge()
    {
        delete[] read_buffer_;
    }
    void Printcharge()
    {
        const char* FunctionName = "Charge::Printcharge";
        // ReadRegs(0x08, read_buffer_, 2);
        // ReadRegs(0x0c, read_buffer_ + 2, 2);
        ESP_ERROR_CHECK(temperature_sensor_get_celsius(temp_sensor, &tsens_value));

        int16_t voltage = static_cast<uint16_t>(read_buffer_[1] << 8 | read_buffer_[0]);
        int16_t current = static_cast<int16_t>(read_buffer_[3] << 8 | read_buffer_[2]);
        (void)voltage;
        (void)current; /* 预留：上报电量/限流策略时可使用 */
    }
    static void TaskFunction(void *pvParameters)
    {
        const char* FunctionName = "Charge::TaskFunction";
        ESP_LOGI(FunctionName, "RyanYuang Battery task started");
        Charge* charge = static_cast<Charge*>(pvParameters);
        ESP_LOGI(FunctionName, "RyanYuang Battery task initialized");
        while (true) {
            charge->Printcharge();
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        ESP_LOGI(FunctionName, "RyanYuang Battery task ended");
    }

private:
    uint8_t* read_buffer_ = nullptr;
};

/** CST816S 屏上电容触摸（I2C 0x15）：INT 脚触发二值信号量，任务内读寄存器解析按压/抬起。 */
class Cst816s : public I2cDevice {
public:
    struct TouchPoint_t {
        int num = 0;
        int x = -1;
        int y = -1;
    };

    enum TouchEvent {
        TOUCH_NONE,
        TOUCH_PRESS,
        TOUCH_RELEASE,
        TOUCH_HOLD
    };

    Cst816s(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr)
    {
        read_buffer_ = new uint8_t[6];
        was_touched_ = false;
        press_count_ = 0;

        touch_isr_mux_ = xSemaphoreCreateBinary(); /* ISR 与 touch 任务之间同步 */
        if (touch_isr_mux_ == NULL) {
            ESP_LOGE(TAG, "Failed to create touch semaphore");
        }
    }

    ~Cst816s()
    {
        delete[] read_buffer_;

        if (touch_isr_mux_ != NULL) {
            vSemaphoreDelete(touch_isr_mux_);
            touch_isr_mux_ = NULL;
        }
    }

    void UpdateTouchPoint()
    {
        ReadRegs(0x02, read_buffer_, 6);
        tp_.num = read_buffer_[0] & 0x0F;
        tp_.x = ((read_buffer_[1] & 0x0F) << 8) | read_buffer_[2];
        tp_.y = ((read_buffer_[3] & 0x0F) << 8) | read_buffer_[4];
    }

    const TouchPoint_t &GetTouchPoint()
    {
        return tp_;
    }

    TouchEvent CheckTouchEvent()
    {
        bool is_touched = (tp_.num > 0);
        TouchEvent event = TOUCH_NONE;

        if (is_touched && !was_touched_) {
            press_count_++;
            event = TOUCH_PRESS;
            ESP_LOGI(TAG, "TOUCH PRESS - count: %d, x: %d, y: %d", press_count_, tp_.x, tp_.y);
        } else if (!is_touched && was_touched_) {
            event = TOUCH_RELEASE;
            ESP_LOGI(TAG, "TOUCH RELEASE - total presses: %d", press_count_);
        } else if (is_touched && was_touched_) {
            event = TOUCH_HOLD;
            ESP_LOGD(TAG, "TOUCH HOLD - x: %d, y: %d", tp_.x, tp_.y);
        }

        was_touched_ = is_touched;
        return event;
    }

    int GetPressCount() const
    {
        return press_count_;
    }

    void ResetPressCount()
    {
        press_count_ = 0;
    }

    SemaphoreHandle_t GetTouchSemaphore()
    {
        return touch_isr_mux_;
    }

    bool WaitForTouchEvent(TickType_t timeout = portMAX_DELAY)
    {
        if (touch_isr_mux_ != NULL) {
            return xSemaphoreTake(touch_isr_mux_, timeout) == pdTRUE;
        }
        return false;
    }

    void NotifyTouchEvent()
    {
        if (touch_isr_mux_ != NULL) {
            BaseType_t xHigherPriorityTaskWoken = pdFALSE;
            xSemaphoreGiveFromISR(touch_isr_mux_, &xHigherPriorityTaskWoken);
            portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
        }
    }

private:
    uint8_t* read_buffer_ = nullptr;
    TouchPoint_t tp_;
    bool was_touched_;
    int press_count_;
    SemaphoreHandle_t touch_isr_mux_;
};

/** VoCat 整机：继承 WifiBoard，完成上电初始化并向 Application 提供 Audio/Display/Camera/Backlight。 */
class EspVocat : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    i2c_bus_handle_t shared_i2c_bus_handle_ = nullptr;
    Cst816s* cst816s_ = nullptr;
    Charge* charge_;
    Button boot_button_;
    Display* display_ = nullptr;
    PwmBacklight* backlight_ = nullptr;
    esp_timer_handle_t touchpad_timer_; /**< 当前未使用，可删或用于触摸轮询定时器 */
    esp_lcd_touch_handle_t tp = nullptr; /**< LVGL 模式：esp_lcd_touch CST816 句柄；Emote 模式保持 nullptr */
    bool cst816s_tp_isr_registered_ = false;
    EspVideo* camera_ = nullptr;
    TaskHandle_t charge_task_handle_ = nullptr;
    TaskHandle_t touch_task_handle_ = nullptr;
    TaskHandle_t imu_task_handle_ = nullptr;
    TaskHandle_t touch_slider_task_handle_ = nullptr;
    esp_timer_handle_t emotion_reset_timer_ = nullptr;
    bool bmi270_ready_ = false;
    touch_slider_handle_t touch_slider_handle_ = nullptr;
    touch_button_handle_t touch_button_handle_ = nullptr;

    /** 单次表情展示结束后回到 neutral，避免界面长期停留在临时表情。 */
    static void emotion_reset_timer_callback(void* arg)
    {
        auto* self = static_cast<EspVocat*>(arg);
        if (self && self->display_ != nullptr) {
            self->display_->SetEmotion("neutral");
        }
    }

    void ShowTemporaryEmotion(const char* emotion, uint32_t duration_ms)
    {
        if (display_ == nullptr || emotion == nullptr) {
            return;
        }
        display_->SetEmotion(emotion);
        if (emotion_reset_timer_ != nullptr) {
            esp_timer_stop(emotion_reset_timer_);
            esp_timer_start_once(emotion_reset_timer_, static_cast<uint64_t>(duration_ms) * 1000ULL);
        }
    }

    /** 机身触摸条/触摸键反馈：限频触发 happy 表情，防止连续中断刷屏。 */
    void ShowHappyTouchFeedback()
    {
        static int64_t s_last_us = 0;
        constexpr int64_t kCooldownUs = 1200000;
        const int64_t now = esp_timer_get_time();
        if ((now - s_last_us) < kCooldownUs) {
            return;
        }
        s_last_us = now;
        ShowTemporaryEmotion("happy", 2000);
    }

    /** 轮询加速度变化，超过阈值且冷却结束则显示短时“困惑”类表情（作摇晃反馈）。 */
    static void imu_event_task(void* arg)
    {
        auto* self = static_cast<EspVocat*>(arg);
        if (self == nullptr || !self->bmi270_ready_) {
            vTaskDelete(NULL);
            return;
        }

        struct bmi2_sens_data prev = {};
        struct bmi2_sens_data cur = {};
        bool has_prev = false;
        int64_t last_shake_ms = 0;
        constexpr int kShakeDeltaThreshold = 20000;
        constexpr int64_t kShakeCooldownMs = 2000;

        while (true) {
            if (Bmi270Motion::ReadAccelRaw(cur)) {
                if (has_prev) {
                    int dx = abs(static_cast<int>(cur.acc.x) - static_cast<int>(prev.acc.x));
                    int dy = abs(static_cast<int>(cur.acc.y) - static_cast<int>(prev.acc.y));
                    int dz = abs(static_cast<int>(cur.acc.z) - static_cast<int>(prev.acc.z));
                    int shake_score = dx + dy + dz;

                    int64_t now_ms = esp_timer_get_time() / 1000;
                    if (shake_score > kShakeDeltaThreshold && (now_ms - last_shake_ms) > kShakeCooldownMs) {
                        last_shake_ms = now_ms;
                        // "dizzy/nauseated" are not guaranteed in current assets, use supported fallback.
                        self->ShowTemporaryEmotion("confused", 1800);
                    }
                }
                prev = cur;
                has_prev = true;
            }
            vTaskDelay(pdMS_TO_TICKS(80));
        }
    }

    /** I2C0 主总线：codec、CST816、充电 IC、BMI270 共用；并安装片内温度传感器。 */
    void InitializeI2c()
    {
        i2c_config_t i2c_cfg = {
            .mode = I2C_MODE_MASTER,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .sda_pullup_en = true,
            .scl_pullup_en = true,
            .master = {
                .clk_speed = 400000,
            },
            .clk_flags = 0,
        };
        shared_i2c_bus_handle_ = i2c_bus_create(I2C_NUM_0, &i2c_cfg);
        if (!shared_i2c_bus_handle_) {
            ESP_LOGE(TAG, "Failed to create shared I2C bus");
            ESP_ERROR_CHECK(ESP_FAIL);
        }
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0) && !CONFIG_I2C_BUS_BACKWARD_CONFIG
        i2c_bus_ = i2c_bus_get_internal_bus_handle(shared_i2c_bus_handle_);
#else
#error "ESP-VoCat board requires i2c_bus_get_internal_bus_handle() support"
#endif
        if (!i2c_bus_) {
            ESP_LOGE(TAG, "Failed to get I2C master handle");
            ESP_ERROR_CHECK(ESP_FAIL);
        }

        temperature_sensor_config_t temp_sensor_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 50);
        ESP_ERROR_CHECK(temperature_sensor_install(&temp_sensor_config, &temp_sensor));
        ESP_ERROR_CHECK(temperature_sensor_enable(temp_sensor));
    }

    /**
     * 通过 CORDEC_POWER_CTRL 上拉策略 + codec(0x18) 是否应答区分 V1.0 / V1.2。
     * V1.2 时切换 AUDIO/QSPI_RST/TOUCH_PAD2/UART 等全局 GPIO 变量。
     */
    uint8_t DetectPcbVersion()
        {
            gpio_config_t gpio_conf = {
                .pin_bit_mask = (1ULL << CORDEC_POWER_CTRL),
                .mode = GPIO_MODE_OUTPUT,
                .pull_up_en = GPIO_PULLUP_DISABLE,
                .pull_down_en = GPIO_PULLDOWN_DISABLE,
                .intr_type = GPIO_INTR_DISABLE
            };
            ESP_ERROR_CHECK(gpio_config(&gpio_conf));
            ESP_ERROR_CHECK(gpio_set_level(CORDEC_POWER_CTRL, 0));
            vTaskDelay(pdMS_TO_TICKS(50));

            bool codec_alive = (i2c_master_probe(i2c_bus_, 0x18, 100) == ESP_OK);
            uint8_t pcb_version = 0;
            if (codec_alive) {
                ESP_LOGI(TAG, "PCB version V1.0");
                pcb_version = 0;
            } else {
                ESP_ERROR_CHECK(gpio_set_level(CORDEC_POWER_CTRL, 1));
                vTaskDelay(pdMS_TO_TICKS(50));
                codec_alive = (i2c_master_probe(i2c_bus_, 0x18, 100) == ESP_OK);
                if (codec_alive) {
                    ESP_LOGI(TAG, "PCB version V1.2");
                    pcb_version = 1;
                    AUDIO_I2S_GPIO_DIN = AUDIO_I2S_GPIO_DIN_2;
                    AUDIO_CODEC_PA_PIN = AUDIO_CODEC_PA_PIN_2;
                    QSPI_PIN_NUM_LCD_RST = QSPI_PIN_NUM_LCD_RST_2;
                    TOUCH_PAD2 = TOUCH_PAD2_2;
                    UART1_TX = UART1_TX_2;
                    UART1_RX = UART1_RX_2;
                } else {
                    ESP_LOGE(TAG, "PCB version detection error");
                }
            }
            return pcb_version;
        }

    /** TP_INT 边沿中断：仅唤醒 touch_event_task，具体坐标在任务里 I2C 读取。 */
    static void touch_isr_callback(void* arg)
    {
        Cst816s* touchpad = static_cast<Cst816s*>(arg);
        if (touchpad != nullptr) {
            touchpad->NotifyTouchEvent();
        }
    }

    /**
     * 屏触摸释放：启动阶段进入 WiFi 配网，否则切换对话开/关（与 BOOT 键逻辑一致）。
     */
    static void touch_event_task(void* arg)
    {
        Cst816s* touchpad = static_cast<Cst816s*>(arg);
        if (touchpad == nullptr) {
            ESP_LOGE(TAG, "Invalid touchpad pointer in touch_event_task");
            vTaskDelete(NULL);
            return;
        }

        while (true) {
            if (touchpad->WaitForTouchEvent()) {
                auto &app = Application::GetInstance();
                auto &board = (EspVocat &)Board::GetInstance();

                ESP_LOGD(TAG, "Touch event, TP_PIN_NUM_INT: %d", gpio_get_level(TP_PIN_NUM_INT));
                touchpad->UpdateTouchPoint();
                auto touch_event = touchpad->CheckTouchEvent();

                if (touch_event == Cst816s::TOUCH_RELEASE) {
                    if (app.GetDeviceState() == kDeviceStateStarting) {
                        board.EnterWifiConfigMode();
                    } else {
                        app.ToggleChatState();
                    }
                }
            }
        }
    }

    void InitializeCharge()
    {
        const char* FunctionName = "InitializeCharge";
        ESP_LOGI(FunctionName, "RyanYuang Initializing Charge");
        charge_ = new Charge(i2c_bus_, 0x55); /* 电量计/充电芯片 I2C 地址 */
        ESP_LOGI(FunctionName, "RyanYuang Charge initialized");
        xTaskCreatePinnedToCore(Charge::TaskFunction, "batterydecTask", 3 * 1024, charge_, 6, &charge_task_handle_, 0);
        ESP_LOGI(FunctionName, "RyanYuang Battery task created");
    }

    /** CST816：仅 Emote 模式自建 I2C 轮询任务 + INT 唤醒；LVGL 模式改用 esp_lcd_touch + lvgl_port（见 InitializeLvglPanelTouch）。 */
#if CONFIG_USE_EMOTE_MESSAGE_STYLE
    void InitializeCst816sTouchPad()
    {
        cst816s_ = new Cst816s(i2c_bus_, 0x15);

        xTaskCreatePinnedToCore(touch_event_task, "touch_task", 4 * 1024, cst816s_, 5, &touch_task_handle_, 1);

        const gpio_config_t int_gpio_config = {
            .pin_bit_mask = (1ULL << TP_PIN_NUM_INT),
            .mode = GPIO_MODE_INPUT,
            .intr_type = GPIO_INTR_ANYEDGE
        };
        gpio_config(&int_gpio_config);
        esp_err_t isr_svc = gpio_install_isr_service(0);
        if (isr_svc != ESP_OK && isr_svc != ESP_ERR_INVALID_STATE) {
            ESP_ERROR_CHECK(isr_svc);
        }
        gpio_intr_enable(TP_PIN_NUM_INT);
        gpio_isr_handler_add(TP_PIN_NUM_INT, EspVocat::touch_isr_callback, cst816s_);
        cst816s_tp_isr_registered_ = true;
    }
#else
    /** LVGL：CST816 经 esp_lcd_touch 接入 `lvgl_port`，坐标与面板 swap/mirror 一致。须在 SpiLcdDisplay 创建之后调用。 */
    void InitializeLvglPanelTouch()
    {
        auto* lvgl = dynamic_cast<LvglDisplay*>(display_);
        if (lvgl == nullptr || lvgl->GetLvglDisplayHandle() == nullptr) {
            ESP_LOGE(TAG, "InitializeLvglPanelTouch: no LvglDisplay");
            return;
        }

        esp_lcd_touch_config_t tp_cfg = {
            .x_max = static_cast<uint16_t>(DISPLAY_WIDTH - 1),
            .y_max = static_cast<uint16_t>(DISPLAY_HEIGHT - 1),
            .rst_gpio_num = TP_PIN_NUM_RST,
            .int_gpio_num = TP_PIN_NUM_INT,
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = DISPLAY_SWAP_XY,
                .mirror_x = DISPLAY_MIRROR_X,
                .mirror_y = DISPLAY_MIRROR_Y,
            },
            .process_coordinates = nullptr,
            .interrupt_callback = nullptr,
            .user_data = nullptr,
        };

        esp_lcd_panel_io_handle_t tp_io_handle = nullptr;
        esp_lcd_panel_io_i2c_config_t tp_io_config = {
            .dev_addr = ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .control_phase_bytes = 1,
            .dc_bit_offset = 0,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 0,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 1,
            },
            .scl_speed_hz = 400 * 1000,
        };

        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io_handle));
        ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_cst816s(tp_io_handle, &tp_cfg, &tp));

        lvgl_port_touch_cfg_t touch_cfg = {};
        touch_cfg.disp = lvgl->GetLvglDisplayHandle();
        touch_cfg.handle = tp;
        lv_indev_t* touch_indev = lvgl_port_add_touch(&touch_cfg);
        if (touch_indev == nullptr) {
            ESP_LOGE(TAG, "lvgl_port_add_touch failed");
        } else {
            ESP_LOGI(TAG, "LVGL touch: CST816S via esp_lcd_touch + lvgl_port_add_touch");
        }
    }
#endif

    /** BMI270 走 i2c_bus 抽象层句柄（与裸 i2c_bus_ 同源总线）。 */
    void InitializeBmi270()
    {
        esp_err_t imu_ret = Bmi270Motion::Initialize(shared_i2c_bus_handle_);
        if (imu_ret == ESP_OK) {
            bmi270_ready_ = true;
            xTaskCreatePinnedToCore(imu_event_task, "imu_task", 4 * 1024, this, 4, &imu_task_handle_, 1);
        } else {
            ESP_LOGW(TAG, "BMI270 unavailable, shake emotion disabled");
        }
    }

    /** ESP32-S3 触摸传感器通道号与 GPIO1..14 一致，其它脚返回 0 表示不可用。 */
    static uint32_t TouchChannelFromPadGpio(gpio_num_t gpio)
    {
        if (gpio == GPIO_NUM_NC) {
            return 0;
        }
        if (gpio >= GPIO_NUM_1 && gpio <= GPIO_NUM_14) {
            return static_cast<uint32_t>(gpio);
        }
        return 0;
    }

    /** 双 Pad：左右滑或松手时触发 happy 表情（忽略纯位置上报）。 */
    static void touch_slider_event_callback(touch_slider_handle_t handle, touch_slider_event_t event, int32_t data, void* cb_arg)
    {
        (void)handle;
        auto* self = static_cast<EspVocat*>(cb_arg);
        if (self == nullptr || self->display_ == nullptr) {
            return;
        }
        if (event != TOUCH_SLIDER_EVENT_POSITION) {
            ESP_LOGI(TAG, "Touch slider evt=%d data=%" PRId32, static_cast<int>(event), data);
        }

        bool gesture = false;
        if (event == TOUCH_SLIDER_EVENT_LEFT_SWIPE || event == TOUCH_SLIDER_EVENT_RIGHT_SWIPE) {
            gesture = true;
        } else if (event == TOUCH_SLIDER_EVENT_RELEASE) {
            gesture = true;
        }

        if (!gesture) {
            return;
        }

        self->ShowHappyTouchFeedback();
    }

    /** 单 Pad（V1.0）：按下 ACTIVE 时表情反馈。 */
    static void touch_button_event_callback(touch_button_handle_t handle, uint32_t channel, touch_state_t state, void* cb_arg)
    {
        (void)handle;
        auto* self = static_cast<EspVocat*>(cb_arg);
        if (self == nullptr || self->display_ == nullptr) {
            return;
        }
        if (state == TOUCH_STATE_ACTIVE) {
            ESP_LOGI(TAG, "Touch button ACTIVE ch=%" PRIu32, channel);
            self->ShowHappyTouchFeedback();
        }
    }

    /** 机身电容触摸库需在任务中周期性 handle_events（20ms）。 */
    static void touch_cap_poll_task(void* arg)
    {
        auto* self = static_cast<EspVocat*>(arg);
        while (true) {
            if (self != nullptr) {
                if (self->touch_slider_handle_ != nullptr) {
                    touch_slider_sensor_handle_events(self->touch_slider_handle_);
                } else if (self->touch_button_handle_ != nullptr) {
                    touch_button_sensor_handle_events(self->touch_button_handle_);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    /**
     * 机身触摸：V1.2 双通道建 slider；V1.0 仅 PAD1 建 button。依赖 TOUCH_PAD1/2 与 ADC 触摸通道对应关系。
     */
    void InitializeCapacitiveTouchPads()
    {
        if (TOUCH_PAD1 == GPIO_NUM_NC) {
            ESP_LOGW(TAG, "Capacitive touch disabled: TOUCH_PAD1 NC");
            return;
        }

        const uint32_t ch1 = TouchChannelFromPadGpio(TOUCH_PAD1);
        if (ch1 == 0) {
            ESP_LOGW(TAG, "TOUCH_PAD1 GPIO %d is not a touch channel (expect GPIO1..GPIO14)", (int)TOUCH_PAD1);
            return;
        }

        if (TOUCH_PAD2 != GPIO_NUM_NC) {
            const uint32_t ch2 = TouchChannelFromPadGpio(TOUCH_PAD2);
            if (ch2 == 0) {
                ESP_LOGW(TAG, "TOUCH_PAD2 GPIO %d is not a touch channel", (int)TOUCH_PAD2);
                return;
            }

            static uint32_t slider_ch[2];
            static float slider_thr[2];
            slider_ch[0] = ch1;
            slider_ch[1] = ch2;
            slider_thr[0] = 0.004f;
            slider_thr[1] = 0.006f;

            touch_slider_config_t sld_cfg = {
                .channel_num = 2,
                .channel_list = slider_ch,
                .channel_threshold = slider_thr,
                .channel_gold_value = nullptr,
                .debounce_times = 1,
                .filter_reset_times = 5,
                .position_range = 10000,
                .calculate_window = 2,
                .swipe_threshold = 28.f,
                .swipe_hysterisis = 22.f,
                .swipe_alpha = 0.9f,
                .skip_lowlevel_init = false,
            };
            esp_err_t err = touch_slider_sensor_create(&sld_cfg, &touch_slider_handle_, touch_slider_event_callback, this);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "touch_slider_sensor_create failed: %s", esp_err_to_name(err));
                touch_slider_handle_ = nullptr;
                return;
            }
            xTaskCreatePinnedToCore(touch_cap_poll_task, "touch_cap", 3072, this, 3, &touch_slider_task_handle_, 1);
            ESP_LOGI(TAG, "Touch slider (PCB v1.2+): PAD1 GPIO%d ch%u, PAD2 GPIO%d ch%u",
                     (int)TOUCH_PAD1, (unsigned)slider_ch[0], (int)TOUCH_PAD2, (unsigned)slider_ch[1]);
            return;
        }

        static uint32_t btn_ch[1];
        static float btn_thr[1];
        btn_ch[0] = ch1;
        btn_thr[0] = 0.004f;

        touch_button_config_t btn_cfg = {
            .channel_num = 1,
            .channel_list = btn_ch,
            .channel_threshold = btn_thr,
            .channel_gold_value = nullptr,
            .debounce_times = 2,
            .skip_lowlevel_init = false,
        };
        esp_err_t err = touch_button_sensor_create(&btn_cfg, &touch_button_handle_, touch_button_event_callback, this);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "touch_button_sensor_create failed: %s", esp_err_to_name(err));
            touch_button_handle_ = nullptr;
            return;
        }
        xTaskCreatePinnedToCore(touch_cap_poll_task, "touch_cap", 3072, this, 3, &touch_slider_task_handle_, 1);
        ESP_LOGI(TAG, "Touch button (PCB v1.0): TOUCH_PAD1 GPIO%d ch%u", (int)TOUCH_PAD1, (unsigned)btn_ch[0]);
    }

    /** LCD 面板 QSPI 总线（与 ST77916 panel_io 共用 QSPI_LCD_HOST）。 */
    void InitializeSpi()
    {
        const spi_bus_config_t bus_config = TAIJIPI_ST77916_PANEL_BUS_QSPI_CONFIG(QSPI_PIN_NUM_LCD_PCLK,
                                                                                  QSPI_PIN_NUM_LCD_DATA0,
                                                                                  QSPI_PIN_NUM_LCD_DATA1,
                                                                                  QSPI_PIN_NUM_LCD_DATA2,
                                                                                  QSPI_PIN_NUM_LCD_DATA3,
                                                                                  QSPI_LCD_H_RES * 80 * sizeof(uint16_t));
        ESP_ERROR_CHECK(spi_bus_initialize(QSPI_LCD_HOST, &bus_config, SPI_DMA_CH_AUTO));
    }

    /** pcb_version 影响 RST 有效电平（flags.reset_active_high）；其余镜像/交换见 DISPLAY_* 宏。 */
    void InitializeSt77916Display(uint8_t pcb_version)
    {

        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        const esp_lcd_panel_io_spi_config_t io_config = ST77916_PANEL_IO_QSPI_CONFIG(QSPI_PIN_NUM_LCD_CS, NULL, NULL);
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)QSPI_LCD_HOST, &io_config, &panel_io));
        st77916_vendor_config_t vendor_config = {
            .init_cmds = vendor_specific_init_yysj,
            .init_cmds_size = sizeof(vendor_specific_init_yysj) / sizeof(st77916_lcd_init_cmd_t),
            .flags = {
                .use_qspi_interface = 1,
            },
        };
        const esp_lcd_panel_dev_config_t panel_config = {
            .reset_gpio_num = QSPI_PIN_NUM_LCD_RST,
            .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
            .bits_per_pixel = QSPI_LCD_BIT_PER_PIXEL,
            .flags = {
                .reset_active_high = pcb_version,
            },
            .vendor_config = &vendor_config,
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_st77916(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_disp_on_off(panel, true);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

#if CONFIG_USE_EMOTE_MESSAGE_STYLE
        display_ = new emote::EmoteDisplay(panel, panel_io, DISPLAY_WIDTH, DISPLAY_HEIGHT);
#else
        display_ = new SpiLcdDisplay(panel_io, panel,
            DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
#endif
        backlight_ = new PwmBacklight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        backlight_->RestoreBrightness();
    }

    /** BOOT：行为同屏触摸释放；POWER_CTRL 拉低维持外设供电策略（依硬件设计）。 */
    void InitializeButtons()
    {
        boot_button_.OnClick([this]() {
            auto &app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                ESP_LOGI(TAG, "Boot button pressed, enter WiFi configuration mode");
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
        gpio_config_t power_gpio_config = {
            .pin_bit_mask = (BIT64(POWER_CTRL)),
            .mode = GPIO_MODE_OUTPUT,

        };
        ESP_ERROR_CHECK(gpio_config(&power_gpio_config));

        gpio_set_level(POWER_CTRL, 0);
    }

#ifdef CONFIG_ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE
    /** USB UVC 摄像头：需打开 menuconfig 中对应选项后才会编译进固件。 */
    void InitializeCamera() {
        esp_video_init_usb_uvc_config_t usb_uvc_config = {
            .uvc = {
                .uvc_dev_num = 1,
                .task_stack = 4096,
                .task_priority = 5,
                .task_affinity = -1,
            },
            .usb = {
                .init_usb_host_lib = true,
                .task_stack = 4096,
                .task_priority = 5,
                .task_affinity = -1,
            },
        };

        esp_video_init_config_t video_config = {
            .usb_uvc = &usb_uvc_config,
        };

        camera_ = new EspVideo(video_config);
    }
#endif // CONFIG_ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE

public:
    /** 单例析构：停任务、删触摸传感器、拆 ISR；背光/相机见类内英文说明。 */
    ~EspVocat() {
        if (charge_task_handle_ != nullptr) {
            vTaskDelete(charge_task_handle_);
        }
        if (touch_task_handle_ != nullptr) {
            vTaskDelete(touch_task_handle_);
        }
        if (imu_task_handle_ != nullptr) {
            vTaskDelete(imu_task_handle_);
        }
        if (touch_slider_task_handle_ != nullptr) {
            vTaskDelete(touch_slider_task_handle_);
            touch_slider_task_handle_ = nullptr;
        }
        if (touch_slider_handle_ != nullptr) {
            touch_slider_sensor_delete(touch_slider_handle_);
            touch_slider_handle_ = nullptr;
        }
        if (touch_button_handle_ != nullptr) {
            touch_button_sensor_delete(touch_button_handle_);
            touch_button_handle_ = nullptr;
        }

        delete charge_;
#if !CONFIG_USE_EMOTE_MESSAGE_STYLE
        if (tp != nullptr) {
            esp_lcd_touch_del(tp);
            tp = nullptr;
        }
#endif
        delete cst816s_;
        delete display_;
        // Note: backlight_ (PwmBacklight) and camera_ (EspVideo) are not deleted here
        // because their base classes (Backlight, Camera) don't have virtual destructors.
        // Since EspVocat is a singleton that lives for the device lifetime, this is acceptable.

        if (cst816s_tp_isr_registered_) {
            gpio_isr_handler_remove(TP_PIN_NUM_INT);
            cst816s_tp_isr_registered_ = false;
        }
        if (emotion_reset_timer_ != nullptr) {
            esp_timer_stop(emotion_reset_timer_);
            esp_timer_delete(emotion_reset_timer_);
            emotion_reset_timer_ = nullptr;
        }

        if (temp_sensor != NULL) {
            temperature_sensor_disable(temp_sensor);
            temperature_sensor_uninstall(temp_sensor);
            temp_sensor = NULL;
        }
    }

    /**
     * 上电顺序：I2C → 判板 → 充电/屏触摸/IMU → QSPI 屏 → 按键与机身触摸 →（可选）UVC。
     */
    EspVocat() : boot_button_(BOOT_BUTTON_GPIO)
    {
        const esp_timer_create_args_t emotion_timer_args = {
            .callback = &EspVocat::emotion_reset_timer_callback,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "emotion_rst",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&emotion_timer_args, &emotion_reset_timer_));

        InitializeI2c();
        ESP_LOGI(TAG, "RyanYuang Initializing I2C");
        uint8_t pcb_version = DetectPcbVersion();
        ESP_LOGI(TAG, "RyanYuang PCB version: %d", pcb_version);
        InitializeCharge();
        ESP_LOGI(TAG, "RyanYuang Initializing Charge");
#if CONFIG_USE_EMOTE_MESSAGE_STYLE
        InitializeCst816sTouchPad();
        ESP_LOGI(TAG, "RyanYuang Initializing CST816s TouchPad (emote)");
#endif
        InitializeBmi270();
        ESP_LOGI(TAG, "RyanYuang Initializing BMI270");

        InitializeSpi();
        InitializeSt77916Display(pcb_version);
#if !CONFIG_USE_EMOTE_MESSAGE_STYLE
        InitializeLvglPanelTouch();
#endif
        InitializeButtons();
        InitializeCapacitiveTouchPads();
#ifdef CONFIG_ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE
        InitializeCamera();
#endif // CONFIG_ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE
    }

    /** ES8311 + ES7210，GPIO 以 DetectPcbVersion 可能改写过的全局变量为准。 */
    virtual AudioCodec* GetAudioCodec() override
    {
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            ESP_LOGI(TAG,
                     "AudioCodec cfg: in_sr=%d out_sr=%d mclk=%d bclk=%d ws=%d din=%d dout=%d es7210=0x%02x es8311=0x%02x",
                     AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                     (int)AUDIO_I2S_GPIO_MCLK, (int)AUDIO_I2S_GPIO_BCLK, (int)AUDIO_I2S_GPIO_WS,
                     (int)AUDIO_I2S_GPIO_DIN, (int)AUDIO_I2S_GPIO_DOUT,
                     AUDIO_CODEC_ES7210_ADDR, AUDIO_CODEC_ES8311_ADDR);
        }
        static BoxAudioCodec audio_codec(
            i2c_bus_,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR,
            AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override
    {
        return display_;
    }

    /** 供上层调试或其它模块直接访问屏触摸控制器（一般仅用 Board 封装即可）。 */
    Cst816s* GetTouchpad()
    {
        return cst816s_;
    }

    virtual Backlight* GetBacklight() override
    {
        return backlight_;
    }

    virtual Camera* GetCamera() override {
        return camera_;
    }
};

/** 编译目标为 esp-vocat 时注册为全局 Board::GetInstance() 实现。 */
DECLARE_BOARD(EspVocat);
