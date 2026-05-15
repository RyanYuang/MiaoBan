#pragma once

#include <lvgl.h>

class Display;
class LvglTheme;

/**
 * 对话页上的快速设置蒙层：自顶部下拉跟手展开；下半屏松手吸合全屏，上半屏松手收起。
 * 不拥有 parent 下其它子对象，仅释放 ctx 本身。
 */
void* sticker_chat_quick_settings_create(lv_obj_t* parent, Display* display, LvglTheme* theme);
void sticker_chat_quick_settings_destroy(void* ctx);
