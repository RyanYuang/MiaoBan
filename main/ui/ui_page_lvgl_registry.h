#pragma once

/**
 * LVGL 整页根工厂登记（供 `UiPageRouter::CreateLvglPageRoot` 展开为 `case`）。
 *
 * 新增一页：
 * 1. 在 `ui_page_ids.h` 的 `UI_PAGE_ID_LIST` 里加一项；
 * 2. 在此列表追加一行：`UI_PAGE_LVGL_FACTORY(X, 符号, View类)`（展开为 `X(符号, View::CreateRouterPageRoot)`），工厂签名为 `void*(Display*, LvglTheme*)`；
 * 3. 若为新模块，在下方 `#include` 对应头文件。
 *
 * 未在此列表的枚举（如 `Home`）在 `ui_page_router.cc` 里手写分支。
 */

#include "lvgl_about_page_view.h"
#include "lvgl_settings_page_view.h"

/** 登记 LVGL 整页：`X(枚举符号, View::CreateRouterPageRoot)`。 */
#define UI_PAGE_LVGL_FACTORY(X, sym, ViewClass) X(sym, ViewClass::CreateRouterPageRoot)

#define UI_PAGE_LVGL_ROOT_FACTORY_LIST(X) \
    UI_PAGE_LVGL_FACTORY(X, Settings, ui::settings::LvglSettingsPageView) \
    UI_PAGE_LVGL_FACTORY(X, About, ui::about::LvglAboutPageView)
