#include "sticker_chat_quick_settings.h"

#include "display.h"
#include "lvgl_theme.h"
#include "ui_page_ids.h"
#include "ui_page_router.h"

namespace {

enum class QsDragPhase {
    kNone,
    kMaybeOpen,
    kOpening,
    kDragging,
};

struct StickerChatQuickSettingsCtx {
    Display* display = nullptr;
    LvglTheme* theme = nullptr;
    lv_obj_t* scrim = nullptr;
    lv_obj_t* sheet = nullptr;

    static constexpr unsigned kMaxDragSources = 12;
    lv_obj_t* drag_sources[kMaxDragSources]{};
    unsigned num_drag_sources = 0;

    lv_coord_t max_sheet_h = 0;
    /** 松手时手指 y ≥ 此值（屏高下 1/2）则吸合为全屏展开；拖拽过程中不据此强制铺满。 */
    lv_coord_t release_full_y_min = 0;
    lv_coord_t open_zone_y_max = 0;

    lv_coord_t sheet_h = 0;
    QsDragPhase phase = QsDragPhase::kNone;
    lv_coord_t press_y = 0;
    lv_coord_t sheet_h_at_press = 0;
};

static lv_coord_t clamp_y(lv_coord_t v, lv_coord_t lo, lv_coord_t hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static void pointer_y(lv_coord_t* out_y)
{
    if (out_y == nullptr) {
        return;
    }
    *out_y = 0;
    lv_indev_t* indev = lv_indev_get_act();
    if (indev == nullptr) {
        return;
    }
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    *out_y = pt.y;
}

static void apply_sheet_geometry(StickerChatQuickSettingsCtx* c)
{
    if (c == nullptr || c->display == nullptr || c->scrim == nullptr || c->sheet == nullptr) {
        return;
    }
    DisplayLockGuard lock(c->display);
    if (c->sheet_h <= 0) {
        lv_obj_add_flag(c->scrim, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(c->sheet, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(c->scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(c->sheet, LV_OBJ_FLAG_HIDDEN);

    lv_coord_t h = clamp_y(c->sheet_h, 8, c->max_sheet_h);
    c->sheet_h = h;

    lv_obj_set_height(c->sheet, h);
    lv_obj_set_width(c->sheet, LV_HOR_RES);
    lv_obj_align(c->sheet, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_move_foreground(c->scrim);
    lv_obj_move_foreground(c->sheet);

    const int32_t max_opa = 160;
    int32_t opa = (max_opa * static_cast<int32_t>(h)) / static_cast<int32_t>(c->max_sheet_h);
    if (opa < 0) {
        opa = 0;
    }
    if (opa > max_opa) {
        opa = max_opa;
    }
    lv_obj_set_style_bg_opa(c->scrim, static_cast<lv_opa_t>(opa), 0);
}

static void attach_drag_sources(lv_obj_t* obj, StickerChatQuickSettingsCtx* c);

static void on_gesture(lv_event_t* e)
{
    auto* c = static_cast<StickerChatQuickSettingsCtx*>(lv_event_get_user_data(e));
    if (c == nullptr) {
        return;
    }

    const lv_event_code_t code = lv_event_get_code(e);
    lv_coord_t y = 0;
    pointer_y(&y);

    switch (code) {
        case LV_EVENT_PRESSED: {
            c->press_y = y;
            c->sheet_h_at_press = c->sheet_h;
            if (c->sheet_h <= 0) {
                c->phase = (y <= c->open_zone_y_max) ? QsDragPhase::kMaybeOpen : QsDragPhase::kNone;
            } else {
                c->phase = QsDragPhase::kDragging;
            }
            break;
        }
        case LV_EVENT_PRESSING: {
            if (c->phase == QsDragPhase::kMaybeOpen) {
                if (y > c->press_y + 10) {
                    c->phase = QsDragPhase::kOpening;
                }
            }
            if (c->phase == QsDragPhase::kOpening) {
                c->sheet_h = clamp_y(y, 0, c->max_sheet_h);
                apply_sheet_geometry(c);
            } else if (c->phase == QsDragPhase::kDragging) {
                const lv_coord_t dy = y - c->press_y;
                c->sheet_h = clamp_y(c->sheet_h_at_press + dy, 0, c->max_sheet_h);
                apply_sheet_geometry(c);
            }
            break;
        }
        case LV_EVENT_RELEASED:
        case LV_EVENT_PRESS_LOST: {
            if (c->phase == QsDragPhase::kMaybeOpen) {
                c->phase = QsDragPhase::kNone;
                break;
            }
            if (c->phase == QsDragPhase::kOpening || c->phase == QsDragPhase::kDragging) {
                const lv_coord_t quarter_h = c->max_sheet_h / 4;
                if (y >= c->release_full_y_min) {
                    c->sheet_h = c->max_sheet_h;
                } else if (c->sheet_h >= quarter_h) {
                    c->sheet_h = c->max_sheet_h;
                } else {
                    c->sheet_h = 0;
                }
                apply_sheet_geometry(c);
            }
            c->phase = QsDragPhase::kNone;
            break;
        }
        default:
            break;
    }
}

static void on_full_settings_click(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    UiPageRouter::Instance().PostNavigateTo(UI_PAGE_ID(Settings));
}

static void attach_drag_sources(lv_obj_t* obj, StickerChatQuickSettingsCtx* c)
{
    if (obj == nullptr || c == nullptr) {
        return;
    }
    if (c->num_drag_sources < StickerChatQuickSettingsCtx::kMaxDragSources) {
        c->drag_sources[c->num_drag_sources++] = obj;
    }
    lv_obj_add_event_cb(obj, on_gesture, LV_EVENT_PRESSED, c);
    lv_obj_add_event_cb(obj, on_gesture, LV_EVENT_PRESSING, c);
    lv_obj_add_event_cb(obj, on_gesture, LV_EVENT_RELEASED, c);
    lv_obj_add_event_cb(obj, on_gesture, LV_EVENT_PRESS_LOST, c);
}

}  // namespace

void* sticker_chat_quick_settings_create(lv_obj_t* parent, Display* display, LvglTheme* theme)
{
    if (parent == nullptr || display == nullptr || theme == nullptr) {
        return nullptr;
    }

    auto* c = new StickerChatQuickSettingsCtx;
    c->display = display;
    c->theme = theme;

    const lv_coord_t H = LV_VER_RES;
    c->max_sheet_h = H;
    c->release_full_y_min = H / 2;
    c->open_zone_y_max = (H * 38) / 100;

    c->scrim = lv_obj_create(parent);
    lv_obj_set_size(c->scrim, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(c->scrim, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(c->scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(c->scrim, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(c->scrim, 0, 0);
    lv_obj_set_style_pad_all(c->scrim, 0, 0);
    lv_obj_set_style_radius(c->scrim, 0, 0);
    lv_obj_add_flag(c->scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(c->scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(c->scrim, LV_OBJ_FLAG_SCROLLABLE);

    c->sheet = lv_obj_create(parent);
    lv_obj_set_size(c->sheet, LV_HOR_RES, 0);
    lv_obj_align(c->sheet, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(c->sheet, theme->background_color(), 0);
    lv_obj_set_style_bg_opa(c->sheet, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c->sheet, 0, 0);
    lv_obj_set_style_pad_all(c->sheet, 12, 0);
    lv_obj_set_style_radius(c->sheet, 14, 0);
    lv_obj_add_flag(c->sheet, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(c->sheet, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(c->sheet, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(c->sheet);
    lv_label_set_text(title, "快速设置");
    lv_obj_set_style_text_font(title, theme->text_font()->font(), 0);
    lv_obj_set_style_text_color(title, theme->text_color(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t* hint = lv_label_create(c->sheet);
    lv_label_set_text(hint, "自顶部下滑跟手展开；手指到下半屏后松手则全屏展开，上滑收起");
    lv_obj_set_style_text_font(hint, theme->text_font()->font(), 0);
    lv_obj_set_style_text_color(hint, theme->text_color(), 0);
    lv_obj_set_style_text_opa(hint, LV_OPA_70, 0);
    lv_obj_set_width(hint, LV_HOR_RES - 24);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t* link = lv_label_create(c->sheet);
    lv_label_set_text(link, "完整设置");
    lv_obj_set_style_text_font(link, theme->text_font()->font(), 0);
    lv_obj_set_style_text_color(link, theme->text_color(), 0);
    lv_obj_set_style_text_decor(link, LV_TEXT_DECOR_UNDERLINE, 0);
    lv_obj_align(link, LV_ALIGN_TOP_LEFT, 12, 88);
    lv_obj_add_flag(link, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(link, on_full_settings_click, LV_EVENT_CLICKED, nullptr);

    attach_drag_sources(parent, c);
    attach_drag_sources(c->scrim, c);
    attach_drag_sources(c->sheet, c);
    attach_drag_sources(title, c);
    attach_drag_sources(hint, c);

    return c;
}

void sticker_chat_quick_settings_destroy(void* ctx)
{
    auto* c = static_cast<StickerChatQuickSettingsCtx*>(ctx);
    if (c == nullptr) {
        return;
    }
    for (unsigned i = 0; i < c->num_drag_sources; ++i) {
        lv_obj_t* o = c->drag_sources[i];
        if (o == nullptr) {
            continue;
        }
        while (lv_obj_remove_event_cb(o, on_gesture)) {
        }
    }
    c->num_drag_sources = 0;
    delete c;
}
