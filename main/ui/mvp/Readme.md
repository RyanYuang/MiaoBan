# MVP UI 脚手架使用说明

本目录提供 **Passive View** 风格的会话区抽象：`Model` 为纯数据、`IConversationView` 为界面契约、`PresenterBase` 为展示层编排基类。设计目标是把「对话/字幕」相关逻辑与 **LVGL**、具体 `Display` 子类解耦，便于单测与分阶段迁移。

## 目录与文件

| 文件 | 作用 |
|------|------|
| `presenter_base.h` | 不可拷贝的 Presenter 基类；头内注释说明线程与 LVGL 约定。 |
| `conversation_view.h` | `IConversationView`：系统/用户/助手行与清空，**不包含** `lvgl.h`。 |
| `conversation_model.h` | `ChatRole`、`ChatLine`、`ConversationModel`（`std::vector<ChatLine>`）。 |
| `example_conversation_presenter.h/.cc` | 示例 Presenter：`ApplySnapshot` 先 `Clear` 再按 model 逐行调用 View。 |
| `null_conversation_view.h/.cc` | 空实现 View，用于单元测试或未接真机 UI 时占位。 |

构建：在 `main/CMakeLists.txt` 中已将 `ui/mvp` 加入 `INCLUDE_DIRS`，并把 `null_conversation_view.cc`、`example_conversation_presenter.cc` 加入 `SOURCES`。新增 `.cc` 时记得同步维护。

## 分层与依赖

```text
业务 / Application（将来）
        ↓ 只依赖 Presenter 接口与 Model
Presenter（编排：何时显示什么）
        ↓ 持有 IConversationView*
IConversationView 实现（Adapter：转发到 Display 或自绘 LVGL）
        ↓ 可选 #include "display.h"
LcdDisplay / OledDisplay …（现有 LVGL 实现）
```

- **Presenter** 不写 LVGL API，不 `#include <lvgl.h>`。
- **View 实现** 若操作 LVGL，须在 **LVGL 任务上下文** 或 **`DisplayLockGuard`** 内更新控件（与现有 `LcdDisplay::SetChatMessage` 一致）。

## `IConversationView` 语义

与现有 `Display::SetChatMessage(role, content)` 对齐，便于做 Adapter：

- `ShowSystemLine` → `role == "system"`
- `ShowUserLine` → `"user"`
- `ShowAssistantLine` → `"assistant"`
- `Clear` → `ClearChatMessages()`

## 用法一：全量快照（`ConversationModel` + `ExampleConversationPresenter`）

适合「整表重绘」场景（例如设置页历史列表、或微信气泡列表清空后重放）。

```cpp
#include "conversation_model.h"
#include "conversation_view.h"
#include "example_conversation_presenter.h"
#include "null_conversation_view.h"

ui::mvp::NullConversationView view;
ui::mvp::ExampleConversationPresenter presenter(&view);

ui::mvp::ConversationModel model;
model.lines.push_back({ui::mvp::ChatRole::kSystem, "就绪"});
model.lines.push_back({ui::mvp::ChatRole::kUser, "你好"});
presenter.ApplySnapshot(model);
```

当前工程中 **`ExampleConversationPresenter` 未在 `Application::Initialize` 里注册**，仅为示例；接入整机时由你在 `SetupUI()` 之后构造并持有 `Presenter` + `Adapter`。

## 用法二：与现有 `Display` 对接（Adapter）

新增一个类实现 `IConversationView`，内部持有 `Display*`，在各自方法里调用 `SetChatMessage` / `ClearChatMessages()`（若 `LcdDisplay` 已在内部加锁，Adapter 内是否再包一层 `DisplayLockGuard` 需视锁是否可重入而定，避免死锁）。

示意（仅说明结构，非仓库内现成文件）：

```cpp
class LcdConversationViewAdapter : public ui::mvp::IConversationView {
public:
    explicit LcdConversationViewAdapter(Display* d) : display_(d) {}
    void ShowSystemLine(std::string text) override {
        if (display_) display_->SetChatMessage("system", text.c_str());
    }
    // ShowUserLine / ShowAssistantLine / Clear 同理
private:
    Display* display_;
};
```

业务侧只依赖 `IConversationView*`，便于替换为 `NullConversationView` 做测试。

## 增量更新 vs 快照

- **`ApplySnapshot`**：先 `Clear()` 再逐行 `Show*Line`。与微信样式「列表清空再填充」一致；**经典单行字幕**若多次 `Show*Line`，最终效果等同于只保留最后一行，需按产品语义在 Presenter 层合并或只推最后一行。
- **增量**：可自建 Presenter（继承 `PresenterBase` 或独立类），增加 `SetSystemMessage`、`AppendUser` 等方法，内部直接调 `IConversationView`，不必经过 `ConversationModel`。

## 与 `Application` 集成（建议步骤）

1. 在 `display->SetupUI()` **之后** 构造 `LcdConversationViewAdapter(display)` 与你的 `ConversationPresenter`（或沿用/扩展 `ExampleConversationPresenter`）。
2. 将原先 `display->SetChatMessage` / `ClearChatMessages` 的调用改为 Presenter 方法。
3. 板级代码、音频回调等若只有 `Display*`，可封装小函数：优先走 `Application` 上的 Presenter，未就绪则回退 `Display`（避免早于 `Initialize` 调用）。

## 扩展新屏幕的 MVP

1. 定义新的 `IXxxView`（纯虚，无 LVGL）。
2. 定义对应 `XxxModel`（POD / `struct`）。
3. 实现 `XxxPresenter` + 真机 `Adapter` + `NullXxxView`。
4. 在 `CMakeLists.txt` 中加入新 `.cc`。

---

更多架构背景见仓库内对话页迁移方案（若已合并）：**View 留在 `LcdDisplay`，业务经 Presenter + Adapter 收口**。

## 相关模块

- **Settings 示例页**（同样基于 `PresenterBase`）：见 [`../settings/README.md`](../settings/README.md)。
- **全局 UI 命令队列**（专用 `ui_cmd` 任务、`Post`/`Start`）：见 [`../README_UI_QUEUE.md`](../README_UI_QUEUE.md)。
