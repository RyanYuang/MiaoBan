# 页面路由（`UiPageRouter` + `ui_page_ids.h`）

在 **`UiCommandDispatcher`** 的 `ui_cmd` 任务里串行执行：用 **页面 ID** 做「前进」，用 **栈** 记住每一页的 LVGL 根节点，**返回**时删除栈顶节点并出栈，下面一层自动露出。

更底层的队列约定见 [`README_UI_QUEUE.md`](README_UI_QUEUE.md)。

---

## 前置条件

1. 已调用 **`UiCommandDispatcher::Instance().Start()`**（通常在 `Application::Initialize()` 里）。
2. 已调用 **`display->SetupUI()`**（LVGL 主界面树已建好，`LvglDisplay::IsSetupUICalled()` 为真）。

然后对路由做一次初始化（只需一次）：

```cpp
#include "ui_page_router.h"
#include "ui_page_ids.h"

// ...
UiPageRouter::Instance().Init(display);
```

---

## 日常用法：前进与返回

**任意任务**（协议回调、按键、定时器等）不要直接改 LVGL，应投递路由 API（内部已是 `Post`）：

```cpp
#include "ui_page_router.h"
#include "ui_page_ids.h"

// 打开某页（压栈：新页盖在最上面）
UiPageRouter::Instance().PostNavigateTo(UI_PAGE_ID(Settings));

// 再打开一层
UiPageRouter::Instance().PostNavigateTo(UI_PAGE_ID(Home));

// 关闭当前最上层，回到上一层
UiPageRouter::Instance().PostNavigateBack();
```

- **`UI_PAGE_ID(符号)`**：符号必须与 `ui_page_ids.h` 里 `UI_PAGE_ID_LIST` 中的名字一致（例如 `Settings`、`Home`）。
- **`PostNavigateTo`**：入队后在 UI 线程创建该页根节点并 **压栈**，再 **`lv_obj_move_foreground`**。
- **`PostNavigateBack`**：栈非空时 **`lv_obj_del` 栈顶根** 并出栈；栈已空则什么也不做。

栈最大深度为 **`UiPageRouter::kMaxPageDepth`（16）**；满时再 `PostNavigateTo` 会打日志并丢弃本次压栈。

---

## 用宏集中管理「有哪些页面」

所有页面符号在 **`main/ui/ui_page_ids.h`** 的 `UI_PAGE_ID_LIST` 中维护：

```c
#define UI_PAGE_ID_LIST(X) \
    X(Settings, 0) \
    X(Home, 1)
```

- 每加一页：在这里加一行 **`X(页面名, 序号)`**。
- 宏会展开出 **`enum class UiPageId`**（如 `UiPageId::kSettings`）。
- 跳转时统一写 **`UI_PAGE_ID(页面名)`**，避免散落魔法数字。

---

## 新增一页要改哪里（三步）

1. **`ui_page_ids.h`**  
   在 `UI_PAGE_ID_LIST` 增加一项，例如：`X(About, 2)`。

2. **`ui_page_router.cc`**  
   在 **`CreateLvglPageRoot`** 的 `switch (id)` 里增加 **`case UiPageId::kAbout:`**，创建整屏根 `lv_obj_t*`（及其子控件），设置好样式后 **`return panel`**。未知 `id` 应删掉半成品并 **`return nullptr`**。

3. **业务代码**  
   在需要跳转处调用：  
   `UiPageRouter::Instance().PostNavigateTo(UI_PAGE_ID(About));`

若某页需要带参数（例如设置项 key），当前实现是「只传 ID」；可后续扩展为 `PostNavigateTo` 携带 `std::string` 等按值捕获进 lambda（注意线程与生命周期）。

---

## LVGL 与表情模式差异

| 配置 | `PostNavigateTo` | `PostNavigateBack` |
|------|------------------|----------------------|
| **非** `CONFIG_USE_EMOTE_MESSAGE_STYLE`（LVGL 主界面） | 创建整页根、压栈、置顶 | 删栈顶 LVGL 根、出栈 |
| **`CONFIG_USE_EMOTE_MESSAGE_STYLE`**（表情主界面） | 对已在 `switch` 里实现的页用 `ShowNotification` 等，**不维护 LVGL 节点栈** | 仅打调试日志，无栈可弹 |

表情板若要做真正的「多级页面 + 返回」，需要单独设计 emote 侧 UI 状态机，而不是依赖本文件的 LVGL 栈。

---

## 与主界面 `SetupUI` 的关系

- **`SetupUI()`** 仍会创建原来的聊天/状态栏等主界面。
- 路由页是 **叠在主界面之上的全屏根节点**；连续 `PostNavigateTo` 会一层层盖住。
- **`PostNavigateBack`** 只删除 **路由栈里的页**；不会替你删除 `SetupUI` 里那棵树。

若希望启动后**只有**路由页、不要先建完整主 UI，需要另开配置或改启动流程（本 README 不展开）。

---

## 应用内参考调用

`Application::Initialize()` 中在 `SetupUI` 之后有示例：

```cpp
UiPageRouter::Instance().Init(display);
UiPageRouter::Instance().PostNavigateTo(UI_PAGE_ID(Settings));
```

可按产品需求改成首屏 `Home` 或延迟到用户操作后再 `PostNavigateTo`。
