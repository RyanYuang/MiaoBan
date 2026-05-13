#pragma once

#include <cstdint>

/**
 * 页面列表：每加一页在此写一行 X(符号, 序号)，并用 UI_PAGE_ID(符号) 取枚举值投递跳转。
 * 序号仅用于稳定排序/日志；路由以枚举为准。
 *
 * LVGL 整页根：非 emote 模式下还要在 `ui_page_lvgl_registry.h` 的
 * `UI_PAGE_LVGL_ROOT_FACTORY_LIST` 里追加一行 `X(符号, CreateRouterPageRoot)`。
 */
#define UI_PAGE_ID_LIST(X) \
    X(Settings, 0)        \
    X(Home, 1)            \
    X(About, 2)

enum class UiPageId : uint8_t {
#define UI_PAGE_EXPAND_ENUM(sym, ord) k##sym = ord,
    UI_PAGE_ID_LIST(UI_PAGE_EXPAND_ENUM)
#undef UI_PAGE_EXPAND_ENUM
        kNone = 0xFF,
};

/** 页面枚举宏：UiPageRouter::PostNavigateTo(UI_PAGE_ID(Settings)); */
#define UI_PAGE_ID(sym) UiPageId::k##sym
