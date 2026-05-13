#pragma once

class Display;

/** 在 UI 线程上 Post：全屏遮罩 + “Settings” 文案（LVGL 板为 lv_label；表情板走 ShowNotification）。 */
void PostSimpleSettingsBootUi(Display* display);
