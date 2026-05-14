#pragma once

#include "ui_page_ids.h"

#include <cstddef>

class Display;

/**
 * 页面路由：任意任务调用 PostNavigateTo / PostNavigateBack，由 UiCommandDispatcher 串行执行。
 * LVGL 设备：每个「前进」在栈顶挂载整页根节点；返回时 lv_obj_del 当前顶并出栈。
 * 表情等非 LVGL 主屏：仅对已知页做有限展示，栈深度不按 LVGL 节点维护。
 */
class UiPageRouter {
public:
    static UiPageRouter& Instance();

    /** 在 Display 就绪后调用一次（例如 SetupUI 之后）。 */
    void Init(Display* display);

    /** 入队：前进到 id，新建页面根并压栈（盖在上一页 / 主界面上方）。 */
    void PostNavigateTo(UiPageId id);

    /** 入队：弹出并销毁栈顶页面根；栈空则无操作。 */
    void PostNavigateBack();

    /** 入队：循环出栈直到空，关掉所有叠在主页上的整页（露出 LcdDisplay 主界面）。 */
    void PostNavigateCloseAll();

    UiPageRouter(const UiPageRouter&) = delete;
    UiPageRouter& operator=(const UiPageRouter&) = delete;

private:
    UiPageRouter() = default;

    void ApplyNavigateTo(UiPageId id);
    void ApplyNavigateBack();
    void ApplyNavigateCloseAll();

    Display* display_ = nullptr;
    /** LVGL 下存 lv_obj_t*；非 LVGL 路径不压栈。 */
    void* PageStackPop();
    void PageStackPush(void* root);
    bool PageStackEmpty() const;

    static constexpr std::size_t kMaxPageDepth = 16;
    void* page_stack_[kMaxPageDepth]{};
    std::size_t page_stack_depth_ = 0;
};
