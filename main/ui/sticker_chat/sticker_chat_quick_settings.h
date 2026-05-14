#pragma once

#include <lvgl.h>

class Display;
class LvglTheme;

/**
 * 对话页上的快速设置蒙层：自顶部下拉跟手展开；手指到屏幕下 1/2 并松手后才吸合为全屏，
 * 拖拽经过下 1/2 过程中不强制铺满。
 * 上滑跟手收起。不拥有 parent 下其它子对象，仅释放 ctx 本身。
 */
void* sticker_chat_quick_settings_create(lv_obj_t* parent, Display* display, LvglTheme* theme);
void sticker_chat_quick_settings_destroy(void* ctx);
