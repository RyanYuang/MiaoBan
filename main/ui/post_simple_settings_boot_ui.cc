#include "post_simple_settings_boot_ui.h"

#include "display.h"
#include "ui_command_dispatcher.h"

#include <esp_log.h>

#ifndef CONFIG_USE_EMOTE_MESSAGE_STYLE
#include "lvgl_display.h"
#include <lvgl.h>
#endif

namespace {

constexpr char kTag[] = "SettingsBootUi";

}  // namespace

void PostSimpleSettingsBootUi(Display* display) {
    if (display == nullptr) {
        return;
    }
    UiCommandDispatcher::Instance().Post([display]() {
#ifndef CONFIG_USE_EMOTE_MESSAGE_STYLE
        auto* lvgl_display = dynamic_cast<LvglDisplay*>(display);
        if (lvgl_display != nullptr && lvgl_display->IsSetupUICalled()) {
            DisplayLockGuard lock(display);
            lv_obj_t* screen = lv_screen_active();
            lv_obj_t* panel = lv_obj_create(screen);
            lv_obj_set_size(panel, LV_HOR_RES, LV_VER_RES);
            lv_obj_align(panel, LV_ALIGN_TOP_LEFT, 0, 0);
            lv_obj_set_style_radius(panel, 0, 0);
            lv_obj_set_style_pad_all(panel, 0, 0);
            lv_obj_set_style_border_width(panel, 0, 0);
            lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(panel, lv_color_black(), 0);
            lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t* label = lv_label_create(panel);
            lv_label_set_text(label, "Settings");
            lv_obj_set_style_text_color(label, lv_color_white(), 0);
            lv_obj_center(label);
            lv_obj_move_foreground(panel);
            ESP_LOGI(kTag, "LVGL boot overlay (Settings)");
            return;
        }
#endif
        display->ShowNotification("Settings", 60000);
        ESP_LOGI(kTag, "fallback ShowNotification(Settings)");
    });
}
