# Settings / About 路由页（MVP）

## Settings（`ui/settings/`）

| 文件 | 角色 |
|------|------|
| `settings_page_model.h` | Model：`SettingsPageModel`（`title`）。 |
| `settings_page_view.h` | View 接口：`ISettingsPageView`，无 LVGL。 |
| `settings_page_presenter.h/.cc` | Presenter：继承 `LvglPageTouchPresenter`，在 `OnClick` 等里处理 LVGL 事件；路由跳转仍用 `UiPageRouter`。 |
| `lvgl_settings_page_view.h/.cc` | LVGL：**`BuildLayout`** 内完成全屏布局与三击；**`CreateRouterPageRoot`** 内组装 MVP、根节点 `user_data` 与 `DELETE` 释放。 |

## About（`ui/about/`）

| 文件 | 角色 |
|------|------|
| `about_page_model.h` | Model：`AboutPageModel`（`title` / `body`）。 |
| `about_page_view.h` | View 接口：`IAboutPageView`。 |
| `about_page_presenter.h/.cc` | Presenter：继承 `LvglPageTouchPresenter`（可按控件重写 `OnPress` / `OnClick` / `OnDrag`）。 |
| `lvgl_about_page_view.h/.cc` | LVGL：**`BuildLayout`** 内完成可滚动布局；**`CreateRouterPageRoot`** 内用 `SystemInfo` 填 Model、组装 MVP 与 `DELETE` 释放。 |

Presenter 基类：`ui/mvp/presenter_base.h`。

路由接入：`ui_page_router.cc` 在 LVGL 模式下通过 **`ui_page_lvgl_registry.h`** 里的 `UI_PAGE_LVGL_ROOT_FACTORY_LIST` 展开各页的 `CreateRouterPageRoot`；`Home` 等仍在本文件 `switch` 中手写。

## 线程约定

工厂与 View 的 `Show` 在 **`DisplayLockGuard` 已持有** 的前提下调用（由 `UiPageRouter::ApplyNavigateTo` 保证），View 内不再二次加锁。
