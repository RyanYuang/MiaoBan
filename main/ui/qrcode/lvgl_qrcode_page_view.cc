#include "lvgl_qrcode_page_view.h"

#include "display.h"
#include "lvgl_theme.h"
#include "ui_page_router.h"

namespace ui::qrcode {

namespace {

constexpr uintptr_t kUserDataBack = 0x4241434Bu;

struct QrPageBundle {
    LvglQrCodePageView* view = nullptr;
    bool swipe_active = false;
    lv_coord_t swipe_x0 = 0;
    lv_coord_t swipe_y0 = 0;
};

static void QrPageRootOnDelete(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    lv_obj_t* root = static_cast<lv_obj_t*>(lv_event_get_target(e));
    auto* bundle = static_cast<QrPageBundle*>(lv_obj_get_user_data(root));
    if (bundle == nullptr) {
        return;
    }
    delete bundle->view;
    delete bundle;
    lv_obj_set_user_data(root, nullptr);
}

static void on_back_click(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (reinterpret_cast<uintptr_t>(lv_obj_get_user_data(target)) != kUserDataBack) {
        return;
    }
    UiPageRouter::Instance().PostNavigateBack();
}

static void pointer_xy(lv_coord_t* out_x, lv_coord_t* out_y)
{
    if (out_x != nullptr) {
        *out_x = 0;
    }
    if (out_y != nullptr) {
        *out_y = 0;
    }
    lv_indev_t* indev = lv_indev_get_act();
    if (indev == nullptr) {
        return;
    }
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    if (out_x != nullptr) {
        *out_x = pt.x;
    }
    if (out_y != nullptr) {
        *out_y = pt.y;
    }
}

/** 在面板及直接子控件上注册：从左向右水平滑动松手后返回上一页。 */
static void on_swipe_back_gesture(lv_event_t* e)
{
    auto* bundle = static_cast<QrPageBundle*>(lv_event_get_user_data(e));
    if (bundle == nullptr) {
        return;
    }

    const lv_event_code_t code = lv_event_get_code(e);
    lv_coord_t x = 0;
    lv_coord_t y = 0;
    pointer_xy(&x, &y);

    switch (code) {
        case LV_EVENT_PRESSED:
            bundle->swipe_active = true;
            bundle->swipe_x0 = x;
            bundle->swipe_y0 = y;
            break;
        case LV_EVENT_RELEASED: {
            if (!bundle->swipe_active) {
                break;
            }
            const lv_coord_t dx = x - bundle->swipe_x0;
            const lv_coord_t dy = y - bundle->swipe_y0;
            const lv_coord_t ady = dy >= 0 ? dy : -dy;
            bundle->swipe_active = false;
            if (dx > 56 && dx > ady) {
                UiPageRouter::Instance().PostNavigateBack();
            }
            break;
        }
        case LV_EVENT_PRESS_LOST:
            bundle->swipe_active = false;
            break;
        default:
            break;
    }
}

static void register_swipe_back_on_panel_tree(lv_obj_t* panel, QrPageBundle* bundle)
{
    if (panel == nullptr || bundle == nullptr) {
        return;
    }
    lv_obj_add_event_cb(panel, on_swipe_back_gesture, LV_EVENT_PRESSED, bundle);
    lv_obj_add_event_cb(panel, on_swipe_back_gesture, LV_EVENT_RELEASED, bundle);
    lv_obj_add_event_cb(panel, on_swipe_back_gesture, LV_EVENT_PRESS_LOST, bundle);
    const uint32_t n = lv_obj_get_child_cnt(panel);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* ch = lv_obj_get_child(panel, i);
        lv_obj_add_event_cb(ch, on_swipe_back_gesture, LV_EVENT_PRESSED, bundle);
        lv_obj_add_event_cb(ch, on_swipe_back_gesture, LV_EVENT_RELEASED, bundle);
        lv_obj_add_event_cb(ch, on_swipe_back_gesture, LV_EVENT_PRESS_LOST, bundle);
    }
}

/** 在 parent 内创建 8×8 简易黑白块，模拟二维码外观。 */
static void create_mock_qr_pattern(lv_obj_t* parent, lv_coord_t cell_px)
{
    const int n = 8;
    const lv_coord_t gap = 2;
    const lv_coord_t step = cell_px + gap;
    for (int r = 0; r < n; ++r) {
        for (int c = 0; c < n; ++c) {
            lv_obj_t* cell = lv_obj_create(parent);
            lv_obj_set_size(cell, cell_px, cell_px);
            lv_obj_set_pos(cell, c * step, r * step);
            lv_obj_set_style_radius(cell, 0, 0);
            lv_obj_set_style_border_width(cell, 0, 0);
            lv_obj_set_style_pad_all(cell, 0, 0);
            const bool black = (((r + c * 3) % 5) < 2) || (r < 2 && c < 2) || (r < 2 && c >= n - 2) || (r >= n - 2 && c < 2);
            lv_obj_set_style_bg_color(cell, black ? lv_color_black() : lv_color_white(), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        }
    }
}

}  // namespace

void* LvglQrCodePageView::CreateRouterPageRoot(Display* display, LvglTheme* theme)
{
    if (display == nullptr || theme == nullptr) {
        return nullptr;
    }
    auto* view = new LvglQrCodePageView(display, theme);
    view->BuildLayout();

    lv_obj_t* root = static_cast<lv_obj_t*>(view->RootHandle());
    if (root == nullptr) {
        delete view;
        return nullptr;
    }
    auto* bundle = new QrPageBundle{view};
    lv_obj_set_user_data(root, bundle);
    lv_obj_add_event_cb(root, QrPageRootOnDelete, LV_EVENT_DELETE, nullptr);
    register_swipe_back_on_panel_tree(root, bundle);
    return root;
}

LvglQrCodePageView::LvglQrCodePageView(Display* display, LvglTheme* theme)
    : display_(display), theme_(theme) {}

LvglQrCodePageView::~LvglQrCodePageView()
{
    root_ = nullptr;
}

void LvglQrCodePageView::BuildLayout()
{
    if (display_ == nullptr || theme_ == nullptr || root_ != nullptr) {
        return;
    }

    lv_obj_t* screen = lv_screen_active();
    lv_obj_t* panel = lv_obj_create(screen);
    lv_obj_set_size(panel, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(panel, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(panel, theme_->background_color(), 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* back_lbl = lv_label_create(panel);
    lv_label_set_text(back_lbl, "返回");
    lv_obj_set_style_text_font(back_lbl, theme_->text_font()->font(), 0);
    lv_obj_set_style_text_color(back_lbl, theme_->text_color(), 0);
    lv_obj_align(back_lbl, LV_ALIGN_TOP_LEFT, 12, 12);
    lv_obj_add_flag(back_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(back_lbl, reinterpret_cast<void*>(kUserDataBack));
    lv_obj_add_event_cb(back_lbl, on_back_click, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, "二维码");
    lv_obj_set_style_text_font(title, theme_->text_font()->font(), 0);
    lv_obj_set_style_text_color(title, theme_->text_color(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t* url = lv_label_create(panel);
    lv_label_set_text(url, "https://example.com/device/mock");
    lv_obj_set_style_text_font(url, theme_->text_font()->font(), 0);
    lv_obj_set_style_text_color(url, theme_->text_color(), 0);
    lv_obj_set_style_text_opa(url, LV_OPA_70, 0);
    lv_obj_set_width(url, LV_HOR_RES - 24);
    lv_label_set_long_mode(url, LV_LABEL_LONG_WRAP);
    lv_obj_align(url, LV_ALIGN_TOP_MID, 0, 52);

    lv_obj_t* qr_box = lv_obj_create(panel);
    const lv_coord_t cell = 10;
    const int n = 8;
    const lv_coord_t gap = 2;
    const lv_coord_t box_w = n * cell + (n - 1) * gap;
    lv_obj_set_size(qr_box, box_w, box_w);
    lv_obj_set_style_bg_opa(qr_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(qr_box, 0, 0);
    lv_obj_set_style_pad_all(qr_box, 0, 0);
    lv_obj_set_style_radius(qr_box, 0, 0);
    lv_obj_align(qr_box, LV_ALIGN_CENTER, 0, 8);
    create_mock_qr_pattern(qr_box, cell);

    lv_obj_t* note = lv_label_create(panel);
    lv_label_set_text(note, "Mock：占位图案，非真实编码数据");
    lv_obj_set_style_text_font(note, theme_->text_font()->font(), 0);
    lv_obj_set_style_text_color(note, theme_->text_color(), 0);
    lv_obj_set_style_text_opa(note, LV_OPA_60, 0);
    lv_obj_align(note, LV_ALIGN_BOTTOM_MID, 0, -24);

    root_ = panel;
}

}  // namespace ui::qrcode
