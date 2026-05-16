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

struct StickerChatQuickSettingsCtx;

struct QsBtnCtx {
    StickerChatQuickSettingsCtx* qs = nullptr;
    int idx = 0;
};

struct StickerChatQuickSettingsCtx {
    Display* display = nullptr;
    LvglTheme* theme = nullptr;
    lv_obj_t* scrim = nullptr;
    lv_obj_t* sheet = nullptr;

    static constexpr int kQuickBtnCount = 6;
    lv_obj_t* quick_btns[kQuickBtnCount]{};
    bool quick_on[kQuickBtnCount]{};
    QsBtnCtx quick_btn_ctx[kQuickBtnCount]{};

    static constexpr unsigned kMaxDragSources = 16;
    lv_obj_t* drag_sources[kMaxDragSources]{};
    unsigned num_drag_sources = 0;

    lv_coord_t max_sheet_h = 0;
    /** 屏高 1/2 分界线：松手时 y < 此值（上半屏）则收起，y ≥ 此值（下半屏）则吸合全屏。 */
    lv_coord_t release_full_y_min = 0;
    lv_coord_t open_zone_y_max = 0;

    lv_coord_t sheet_h = 0;
    /** 拖动/动画过程中用于局部 invalidate 的上一次高度。 */
    lv_coord_t sheet_h_painted = 0;
    QsDragPhase phase = QsDragPhase::kNone;
    lv_coord_t press_y = 0;
    lv_coord_t sheet_h_at_press = 0;
};

/** 跟手拖动时 scrim 固定半透明，避免每帧改 bg_opa 触发全屏重绘。 */
static constexpr lv_opa_t kDragScrimOpa = 100;

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

static void apply_round_toggle_visual(lv_obj_t* btn, bool on)
{
    if (btn == nullptr) {
        return;
    }
    lv_obj_set_size(btn, 80, 80);
    lv_obj_set_style_radius(btn, 40, 0);
    if (on) {
        lv_obj_set_style_bg_color(btn, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
    } else {
        lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(btn, lv_color_black(), 0);
        lv_obj_set_style_border_width(btn, 5, 0);
        lv_obj_set_style_border_opa(btn, LV_OPA_COVER, 0);
    }
}

static void on_quick_btn_click(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    auto* bc = static_cast<QsBtnCtx*>(lv_event_get_user_data(e));
    if (bc == nullptr || bc->qs == nullptr) {
        return;
    }
    StickerChatQuickSettingsCtx* c = bc->qs;
    const int idx = bc->idx;
    if (idx < 0 || idx >= StickerChatQuickSettingsCtx::kQuickBtnCount) {
        return;
    }
    if (idx == 5) {
        UiPageRouter::Instance().PostNavigateTo(UI_PAGE_ID(QrCode));
        return;
    }
    c->quick_on[idx] = !c->quick_on[idx];
    apply_round_toggle_visual(c->quick_btns[idx], c->quick_on[idx]);
    lv_obj_t* lab = lv_obj_get_child(c->quick_btns[idx], 0);
    if (lab != nullptr) {
        lv_obj_set_style_text_color(lab, c->quick_on[idx] ? lv_color_white() : lv_color_black(), 0);
    }
}

static void apply_sheet_geometry_full(StickerChatQuickSettingsCtx* c);
static void apply_sheet_geometry_drag(StickerChatQuickSettingsCtx* c, lv_coord_t new_h);

static void invalidate_height_band(lv_obj_t* obj, lv_coord_t y0, lv_coord_t y1)
{
    if (obj == nullptr || y1 <= y0) {
        return;
    }
    lv_area_t area = {
        .x1 = 0,
        .y1 = y0,
        .x2 = static_cast<lv_coord_t>(lv_obj_get_width(obj) - 1),
        .y2 = static_cast<lv_coord_t>(y1 - 1),
    };
    lv_obj_invalidate_area(obj, &area);
}

static void invalidate_sheet_height_delta(StickerChatQuickSettingsCtx* c, lv_coord_t old_h, lv_coord_t new_h)
{
    if (c == nullptr || c->sheet == nullptr || old_h == new_h) {
        return;
    }
    if (new_h > old_h) {
        invalidate_height_band(c->sheet, old_h, new_h);
        return;
    }
    lv_obj_t* parent = lv_obj_get_parent(c->sheet);
    if (parent != nullptr) {
        invalidate_height_band(parent, new_h, old_h);
    }
    lv_obj_invalidate(c->sheet);
}

static void qs_sheet_h_anim_exec(void* var, int32_t v)
{
    auto* c = static_cast<StickerChatQuickSettingsCtx*>(var);
    if (c == nullptr) {
        return;
    }
    apply_sheet_geometry_drag(c, static_cast<lv_coord_t>(v));
}

static void qs_sheet_h_anim_completed(lv_anim_t* a)
{
    if (a == nullptr) {
        return;
    }
    auto* c = static_cast<StickerChatQuickSettingsCtx*>(a->var);
    apply_sheet_geometry_full(c);
}

/** 松手吸合/收起：时长随行程变化，避免短距离也拖很久。 */
static uint32_t qs_snap_duration_ms(lv_coord_t span, lv_coord_t max_h)
{
    if (span <= 0 || max_h <= 0) {
        return 0;
    }
    uint32_t ms = 120 + (static_cast<uint32_t>(span) * 240) / static_cast<uint32_t>(max_h);
    if (ms > 360) {
        ms = 360;
    }
    return ms;
}

static void qs_animate_sheet_h_to(StickerChatQuickSettingsCtx* c, lv_coord_t target_h)
{
    if (c == nullptr) {
        return;
    }
    lv_anim_delete(c, qs_sheet_h_anim_exec);
    const lv_coord_t from = c->sheet_h;
    if (from == target_h) {
        apply_sheet_geometry_full(c);
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, c);
    lv_anim_set_exec_cb(&a, qs_sheet_h_anim_exec);
    lv_anim_set_completed_cb(&a, qs_sheet_h_anim_completed);
    lv_anim_set_values(&a, static_cast<int32_t>(from), static_cast<int32_t>(target_h));
    lv_anim_set_duration(&a, qs_snap_duration_ms(from > target_h ? from - target_h : target_h - from, c->max_sheet_h));
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/** 跟手拖动 / 吸合动画：只改 sheet 高度 + 局部 invalidate，scrim 保持固定透明度。 */
static void apply_sheet_geometry_drag(StickerChatQuickSettingsCtx* c, lv_coord_t new_h)
{
    if (c == nullptr || c->display == nullptr || c->scrim == nullptr || c->sheet == nullptr) {
        return;
    }
    DisplayLockGuard lock(c->display);

    lv_coord_t h = clamp_y(new_h, 0, c->max_sheet_h);
    if (h > 0 && h < 8) {
        h = 8;
    }
    if (h <= 0) {
        if (c->sheet_h_painted > 0) {
            lv_obj_t* parent = lv_obj_get_parent(c->sheet);
            if (parent != nullptr) {
                invalidate_height_band(parent, 0, c->sheet_h_painted);
            }
        }
        lv_obj_add_flag(c->scrim, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(c->sheet, LV_OBJ_FLAG_HIDDEN);
        c->sheet_h = 0;
        c->sheet_h_painted = 0;
        return;
    }

    const lv_coord_t old_h = c->sheet_h_painted;
    const bool was_hidden = lv_obj_has_flag(c->sheet, LV_OBJ_FLAG_HIDDEN);

    if (was_hidden) {
        lv_obj_remove_flag(c->scrim, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(c->sheet, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_opa(c->scrim, kDragScrimOpa, 0);
        lv_obj_move_foreground(c->scrim);
        lv_obj_move_foreground(c->sheet);
    }

    lv_obj_set_height(c->sheet, h);
    lv_obj_align(c->sheet, LV_ALIGN_TOP_MID, 0, 0);

    if (was_hidden || old_h <= 0) {
        lv_obj_invalidate(c->sheet);
    } else {
        invalidate_sheet_height_delta(c, old_h, h);
    }

    c->sheet_h = h;
    c->sheet_h_painted = h;
}

/** 松手吸合结束或状态对齐：同步 scrim 透明度、宽度与 z-order。 */
static void apply_sheet_geometry_full(StickerChatQuickSettingsCtx* c)
{
    if (c == nullptr || c->display == nullptr || c->scrim == nullptr || c->sheet == nullptr) {
        return;
    }
    DisplayLockGuard lock(c->display);
    if (c->sheet_h <= 0) {
        if (c->sheet_h_painted > 0) {
            lv_obj_t* parent = lv_obj_get_parent(c->sheet);
            if (parent != nullptr) {
                invalidate_height_band(parent, 0, c->sheet_h_painted);
            }
        }
        lv_obj_add_flag(c->scrim, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(c->sheet, LV_OBJ_FLAG_HIDDEN);
        c->sheet_h_painted = 0;
        return;
    }

    lv_obj_remove_flag(c->scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(c->sheet, LV_OBJ_FLAG_HIDDEN);

    const lv_coord_t h = clamp_y(c->sheet_h, 8, c->max_sheet_h);
    c->sheet_h = h;

    lv_obj_set_height(c->sheet, h);
    lv_obj_set_width(c->sheet, LV_HOR_RES);
    lv_obj_align(c->sheet, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_move_foreground(c->scrim);
    lv_obj_move_foreground(c->sheet);

    constexpr int32_t kMaxScrimOpa = 160;
    int32_t opa = (kMaxScrimOpa * static_cast<int32_t>(h)) / static_cast<int32_t>(c->max_sheet_h);
    if (opa < 0) {
        opa = 0;
    }
    if (opa > kMaxScrimOpa) {
        opa = kMaxScrimOpa;
    }
    lv_obj_set_style_bg_opa(c->scrim, static_cast<lv_opa_t>(opa), 0);
    c->sheet_h_painted = h;
    lv_obj_invalidate(c->scrim);
    lv_obj_invalidate(c->sheet);
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
            lv_anim_delete(c, qs_sheet_h_anim_exec);
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
                apply_sheet_geometry_drag(c, clamp_y(y, 0, c->max_sheet_h));
            } else if (c->phase == QsDragPhase::kDragging) {
                const lv_coord_t dy = y - c->press_y;
                apply_sheet_geometry_drag(c, clamp_y(c->sheet_h_at_press + dy, 0, c->max_sheet_h));
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
                if (y < c->release_full_y_min) {
                    qs_animate_sheet_h_to(c, 0);
                } else {
                    qs_animate_sheet_h_to(c, c->max_sheet_h);
                }
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
    lv_obj_set_style_pad_all(c->sheet, 10, 0);
    lv_obj_set_style_radius(c->sheet, 14, 0);
    lv_obj_add_flag(c->sheet, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(c->sheet, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(c->sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(c->sheet, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c->sheet, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(c->sheet, 6, 0);

    lv_obj_t* title = lv_label_create(c->sheet);
    lv_label_set_text(title, "快速设置");
    lv_obj_set_style_text_font(title, theme->text_font()->font(), 0);
    lv_obj_set_style_text_color(title, theme->text_color(), 0);
    lv_obj_set_width(title, LV_PCT(100));
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t* hint = lv_label_create(c->sheet);
    lv_label_set_text(hint, "自顶部下滑跟手展开；下半屏松手全屏，上半屏松手收起");
    lv_obj_set_style_text_font(hint, theme->text_font()->font(), 0);
    lv_obj_set_style_text_color(hint, theme->text_color(), 0);
    lv_obj_set_style_text_opa(hint, LV_OPA_70, 0);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t* grid = lv_obj_create(c->sheet);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_height(grid, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 4, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(grid, 14, 0);
    lv_obj_set_style_pad_column(grid, 10, 0);

    static const char* kQuickLbls[StickerChatQuickSettingsCtx::kQuickBtnCount] = {
        "WiFi", "蓝牙", "麦克风", "扬声器", "设置", "QR",
    };
    for (int i = 0; i < StickerChatQuickSettingsCtx::kQuickBtnCount; ++i) {
        lv_obj_t* btn = lv_obj_create(grid);
        c->quick_btns[i] = btn;
        c->quick_on[i] = false;
        apply_round_toggle_visual(btn, false);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_pad_all(btn, 0, 0);
        c->quick_btn_ctx[i].qs = c;
        c->quick_btn_ctx[i].idx = i;
        lv_obj_add_event_cb(btn, on_quick_btn_click, LV_EVENT_CLICKED, &c->quick_btn_ctx[i]);
        lv_obj_t* lab = lv_label_create(btn);
        lv_label_set_text(lab, kQuickLbls[i]);
        lv_obj_set_style_text_font(lab, theme->text_font()->font(), 0);
        lv_obj_set_style_text_color(lab, lv_color_black(), 0);
        lv_obj_set_style_text_align(lab, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(lab);
    }

    lv_obj_t* link = lv_label_create(c->sheet);
    lv_label_set_text(link, "完整设置");
    lv_obj_set_style_text_font(link, theme->text_font()->font(), 0);
    lv_obj_set_style_text_color(link, theme->text_color(), 0);
    lv_obj_set_style_text_decor(link, LV_TEXT_DECOR_UNDERLINE, 0);
    lv_obj_set_width(link, LV_PCT(100));
    lv_obj_set_style_text_align(link, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(link, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(link, on_full_settings_click, LV_EVENT_CLICKED, nullptr);

    attach_drag_sources(parent, c);
    attach_drag_sources(c->scrim, c);
    attach_drag_sources(c->sheet, c);
    attach_drag_sources(title, c);
    attach_drag_sources(hint, c);
    attach_drag_sources(grid, c);

    return c;
}

void sticker_chat_quick_settings_destroy(void* ctx)
{
    auto* c = static_cast<StickerChatQuickSettingsCtx*>(ctx);
    if (c == nullptr) {
        return;
    }
    lv_anim_delete(c, qs_sheet_h_anim_exec);
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
