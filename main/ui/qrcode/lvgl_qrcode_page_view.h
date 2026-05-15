#pragma once

#include <lvgl.h>

class Display;
class LvglTheme;

namespace ui::qrcode {

/** 二维码展示页（Mock 图案 + 文案），CreateRouterPageRoot 供 UiPageRouter 压栈。 */
class LvglQrCodePageView {
public:
    static void* CreateRouterPageRoot(Display* display, LvglTheme* theme);

    LvglQrCodePageView(Display* display, LvglTheme* theme);
    ~LvglQrCodePageView();

    void BuildLayout();

    void* RootHandle() const { return root_; }

private:
    Display* display_;
    LvglTheme* theme_;
    void* root_ = nullptr;
};

}  // namespace ui::qrcode
