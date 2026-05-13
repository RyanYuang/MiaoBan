#include "lvgl_page_touch_presenter.h"

namespace ui::mvp {

namespace {

void LvglPageTouchDispatch(lv_event_t* e)
{
    if (e == nullptr) {
        return;
    }
    auto* presenter = static_cast<LvglPageTouchPresenter*>(lv_event_get_user_data(e));
    if (presenter == nullptr) {
        return;
    }
    switch (lv_event_get_code(e)) {
        case LV_EVENT_PRESSED:
            presenter->OnPress(e);
            break;
        case LV_EVENT_CLICKED:
            presenter->OnClick(e);
            break;
        case LV_EVENT_PRESSING:
            presenter->OnDrag(e);
            break;
        default:
            break;
    }
}

}  // namespace

void LvglPageAttachTouchHandlers(lv_obj_t* obj, LvglPageTouchPresenter* presenter)
{
    if (obj == nullptr || presenter == nullptr) {
        return;
    }
    lv_obj_add_event_cb(obj, LvglPageTouchDispatch, LV_EVENT_PRESSED, presenter);
    lv_obj_add_event_cb(obj, LvglPageTouchDispatch, LV_EVENT_CLICKED, presenter);
    lv_obj_add_event_cb(obj, LvglPageTouchDispatch, LV_EVENT_PRESSING, presenter);
}

}  // namespace ui::mvp
