#pragma once

/**
 * LVGL 专用：整页 Presenter 的按压 / 点击 / 拖拽（PRESSING）回调。
 * 仅在非 emote、主界面为 LVGL 的工程路径下使用；接口直接使用 lv_event_t。
 */

#include "mvp/presenter_base.h"

#include <lvgl.h>

namespace ui::mvp {

/** 子类重写以处理触摸；默认空实现。 */
class LvglPageTouchPresenter : public PresenterBase {
public:
    ~LvglPageTouchPresenter() override = default;

    virtual void OnPress(lv_event_t* e) { (void)e; }
    virtual void OnClick(lv_event_t* e) { (void)e; }
    /** 对应 LV_EVENT_PRESSING（按住移动）。 */
    virtual void OnDrag(lv_event_t* e) { (void)e; }
};

/** 在控件上注册 PRESSED / CLICKED / PRESSING，转发到 presenter。可多次调用以覆盖多个控件。 */
void LvglPageAttachTouchHandlers(lv_obj_t* obj, LvglPageTouchPresenter* presenter);

}  // namespace ui::mvp
