#include "lvgl_meeting_page_view.h"

#include "display.h"
#include "lvgl_page_touch_presenter.h"
#include "lvgl_theme.h"
#include "meeting_page_presenter.h"
#include "ui_page_router.h"

#include <atomic>
#include <cstdint>
#include <memory>

namespace ui::meeting {

namespace {

constexpr uintptr_t kUserDataBack = 0x4241434Bu;
constexpr uintptr_t kUserDataRefresh = 0x52454652u;
constexpr uintptr_t kUserDataRecord = 0x52454344u;
constexpr uintptr_t kUserDataRowBase = 0x4D545230u;

struct MeetingMvpBundle {
    MeetingPagePresenter* presenter = nullptr;
    LvglMeetingPageView* view = nullptr;
    std::shared_ptr<std::atomic<bool>> alive;
    bool swipe_active = false;
    lv_coord_t swipe_x0 = 0;
    lv_coord_t swipe_y0 = 0;
};

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

/** 从左向右水平滑动松手后返回上一页（与 WiFi / QR 页一致）。 */
static void on_swipe_back_gesture(lv_event_t* e)
{
    auto* bundle = static_cast<MeetingMvpBundle*>(lv_event_get_user_data(e));
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
                UiPageRouter::Instance().NavigateBackFromInput();
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

static void attach_swipe_back(lv_obj_t* obj, MeetingMvpBundle* bundle)
{
    if (obj == nullptr || bundle == nullptr) {
        return;
    }
    lv_obj_add_event_cb(obj, on_swipe_back_gesture, LV_EVENT_PRESSED, bundle);
    lv_obj_add_event_cb(obj, on_swipe_back_gesture, LV_EVENT_RELEASED, bundle);
    lv_obj_add_event_cb(obj, on_swipe_back_gesture, LV_EVENT_PRESS_LOST, bundle);
}

static MeetingMvpBundle* BundleFromRoot(lv_obj_t* root)
{
    if (root == nullptr) {
        return nullptr;
    }
    return static_cast<MeetingMvpBundle*>(lv_obj_get_user_data(root));
}

static void register_swipe_back_subtree(lv_obj_t* obj, MeetingMvpBundle* bundle)
{
    if (obj == nullptr || bundle == nullptr) {
        return;
    }
    attach_swipe_back(obj, bundle);
    const uint32_t n = lv_obj_get_child_cnt(obj);
    for (uint32_t i = 0; i < n; ++i) {
        register_swipe_back_subtree(lv_obj_get_child(obj, i), bundle);
    }
}

static void attach_swipe_back_list_children(lv_obj_t* list_panel, lv_obj_t* root)
{
    auto* bundle = BundleFromRoot(root);
    if (list_panel == nullptr || bundle == nullptr) {
        return;
    }
    const uint32_t n = lv_obj_get_child_cnt(list_panel);
    for (uint32_t i = 0; i < n; ++i) {
        attach_swipe_back(lv_obj_get_child(list_panel, i), bundle);
    }
}

static void MeetingRootOnDelete(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    lv_obj_t* root = static_cast<lv_obj_t*>(lv_event_get_target(e));
    auto* bundle = static_cast<MeetingMvpBundle*>(lv_obj_get_user_data(root));
    if (bundle == nullptr) {
        return;
    }
    if (bundle->alive != nullptr) {
        bundle->alive->store(false);
    }
    ReleaseMeetingUiWorkerIfIdle();
    delete bundle->presenter;
    delete bundle->view;
    delete bundle;
    lv_obj_set_user_data(root, nullptr);
}

}  // namespace

void* LvglMeetingPageView::CreateRouterPageRoot(Display* display, LvglTheme* theme)
{
    if (display == nullptr || theme == nullptr) {
        return nullptr;
    }
    auto alive = std::make_shared<std::atomic<bool>>(true);
    auto* view = new LvglMeetingPageView(display, theme);
    auto* presenter = new MeetingPagePresenter(view, display);
    presenter->BindPageLifetime(alive);
    view->BindTouchPresenter(presenter);

    MeetingPageModel model;
    model.title = "会议纪要";
    presenter->Show(model);
    presenter->OnShow();

    lv_obj_t* root = static_cast<lv_obj_t*>(view->RootHandle());
    if (root == nullptr) {
        delete presenter;
        delete view;
        return nullptr;
    }

    auto* bundle = new MeetingMvpBundle{presenter, view, alive};
    lv_obj_set_user_data(root, bundle);
    lv_obj_add_event_cb(root, MeetingRootOnDelete, LV_EVENT_DELETE, nullptr);
    register_swipe_back_subtree(root, bundle);
    return root;
}

LvglMeetingPageView::LvglMeetingPageView(Display* display, LvglTheme* theme)
    : display_(display), theme_(theme) {}

LvglMeetingPageView::~LvglMeetingPageView() {
    root_ = nullptr;
}

void LvglMeetingPageView::Show(const MeetingPageModel& model) {
    if (root_ == nullptr) {
        BuildLayout(model);
    } else {
        RebuildList(model);
        UpdateDetailPanel(model);
        if (record_button_label_ != nullptr) {
            lv_label_set_text(record_button_label_, model.record_button_text.c_str());
        }
        if (status_label_ != nullptr) {
            lv_label_set_text(status_label_, model.status_line.c_str());
        }
    }
}

void LvglMeetingPageView::SetStatusLine(const std::string& text) {
    if (status_label_ != nullptr) {
        lv_label_set_text(status_label_, text.c_str());
    }
}

void LvglMeetingPageView::BuildLayout(const MeetingPageModel& model)
{
    if (display_ == nullptr || theme_ == nullptr || root_ != nullptr) {
        return;
    }

    lv_obj_t* screen = lv_screen_active();
    lv_obj_t* panel = lv_obj_create(screen);
    lv_obj_set_size(panel, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(panel, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, theme_->spacing(3), 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(panel, theme_->background_color(), 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(panel, theme_->spacing(2), 0);

    lv_obj_t* header = lv_obj_create(panel);
    lv_obj_set_width(header, LV_PCT(100));
    lv_obj_set_height(header, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* back = lv_label_create(header);
    lv_label_set_text(back, LV_SYMBOL_LEFT " 返回");
    lv_obj_set_style_text_font(back, theme_->text_font()->font(), 0);
    lv_obj_set_style_text_color(back, theme_->text_color(), 0);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(back, reinterpret_cast<void*>(kUserDataBack));

    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, model.title.c_str());
    lv_obj_set_style_text_font(title, theme_->text_font()->font(), 0);
    lv_obj_set_style_text_color(title, theme_->text_color(), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    list_panel_ = lv_obj_create(panel);
    lv_obj_set_width(list_panel_, LV_PCT(100));
    lv_obj_set_flex_grow(list_panel_, 1);
    lv_obj_set_style_bg_opa(list_panel_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_panel_, 0, 0);
    lv_obj_set_style_pad_all(list_panel_, 0, 0);
    lv_obj_remove_flag(list_panel_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(list_panel_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list_panel_, theme_->spacing(2), 0);

    detail_panel_ = lv_obj_create(panel);
    lv_obj_set_width(detail_panel_, LV_PCT(100));
    lv_obj_set_height(detail_panel_, LV_VER_RES / 3);
    lv_obj_set_style_bg_color(detail_panel_, theme_->chat_background_color(), 0);
    lv_obj_set_style_bg_opa(detail_panel_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(detail_panel_, 0, 0);
    lv_obj_set_style_pad_all(detail_panel_, theme_->spacing(2), 0);
    lv_obj_add_flag(detail_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(detail_panel_, LV_OBJ_FLAG_SCROLLABLE);

    detail_body_ = lv_label_create(detail_panel_);
    lv_label_set_long_mode(detail_body_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(detail_body_, LV_PCT(100));
    lv_obj_set_style_text_font(detail_body_, theme_->text_font()->font(), 0);
    lv_obj_set_style_text_color(detail_body_, theme_->text_color(), 0);

    lv_obj_t* actions = lv_obj_create(panel);
    lv_obj_set_width(actions, LV_PCT(100));
    lv_obj_set_height(actions, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(actions, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(actions, 0, 0);
    lv_obj_set_style_pad_column(actions, theme_->spacing(3), 0);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* refresh_btn = lv_button_create(actions);
    lv_obj_set_size(refresh_btn, 120, 40);
    lv_obj_t* refresh_lab = lv_label_create(refresh_btn);
    lv_label_set_text(refresh_lab, "刷新");
    lv_obj_center(refresh_lab);
    lv_obj_set_user_data(refresh_btn, reinterpret_cast<void*>(kUserDataRefresh));

    record_button_ = lv_button_create(actions);
    lv_obj_set_size(record_button_, 120, 40);
    record_button_label_ = lv_label_create(record_button_);
    lv_label_set_text(record_button_label_, model.record_button_text.c_str());
    lv_obj_center(record_button_label_);
    lv_obj_set_user_data(record_button_, reinterpret_cast<void*>(kUserDataRecord));

    status_label_ = lv_label_create(panel);
    lv_obj_set_width(status_label_, LV_PCT(100));
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(status_label_, theme_->text_font()->font(), 0);
    lv_obj_set_style_text_color(status_label_, theme_->text_color(), 0);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(status_label_, model.status_line.c_str());

    if (touch_presenter_ != nullptr) {
        ui::mvp::LvglPageAttachTouchHandlers(panel, touch_presenter_);
        ui::mvp::LvglPageAttachTouchHandlers(back, touch_presenter_);
        ui::mvp::LvglPageAttachTouchHandlers(refresh_btn, touch_presenter_);
        ui::mvp::LvglPageAttachTouchHandlers(record_button_, touch_presenter_);
    }

    root_ = panel;
    RebuildList(model);
    UpdateDetailPanel(model);
}

void LvglMeetingPageView::RebuildList(const MeetingPageModel& model)
{
    if (list_panel_ == nullptr) {
        return;
    }
    lv_obj_clean(list_panel_);

    if (model.loading && model.meetings.empty()) {
        lv_obj_t* lab = lv_label_create(list_panel_);
        lv_obj_align(lab, LV_ALIGN_CENTER, 0, 0);
        lv_label_set_text(lab, "加载中…");
        lv_obj_set_style_text_font(lab, theme_->text_font()->font(), 0);
        lv_obj_set_style_text_color(lab, theme_->text_color(), 0);
        attach_swipe_back_list_children(list_panel_, static_cast<lv_obj_t*>(root_));
        return;
    }

    if (model.meetings.empty()) {
        lv_obj_t* lab = lv_label_create(list_panel_);
        lv_label_set_text(lab, "暂无会议记录");
        lv_obj_set_style_text_font(lab, theme_->text_font()->font(), 0);
        lv_obj_set_style_text_color(lab, theme_->text_color(), 0);
        attach_swipe_back_list_children(list_panel_, static_cast<lv_obj_t*>(root_));
        return;
    }

    for (const auto& row : model.meetings) {
        lv_obj_t* btn = lv_button_create(list_panel_);
        lv_obj_set_width(btn, LV_PCT(100));
        lv_obj_set_height(btn, LV_SIZE_CONTENT);
        lv_obj_set_user_data(btn, reinterpret_cast<void*>(kUserDataRowBase + static_cast<uintptr_t>(row.id)));

        lv_obj_t* col = lv_obj_create(btn);
        lv_obj_set_width(col, LV_PCT(100));
        lv_obj_set_height(col, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(col, 0, 0);
        lv_obj_set_style_pad_all(col, 4, 0);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);

        lv_obj_t* t = lv_label_create(col);
        lv_label_set_text(t, row.title.c_str());
        lv_obj_set_style_text_font(t, theme_->text_font()->font(), 0);
        lv_obj_set_style_text_color(t, theme_->text_color(), 0);
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_obj_set_width(t, LV_PCT(100));

        std::string sub = row.status;
        if (!row.summary_preview.empty()) {
            sub += " · ";
            sub += row.summary_preview;
        }
        lv_obj_t* s = lv_label_create(col);
        lv_label_set_text(s, sub.c_str());
        lv_obj_set_style_text_font(s, theme_->text_font()->font(), 0);
        lv_obj_set_style_text_opa(s, LV_OPA_70, 0);
        lv_obj_set_style_text_color(s, theme_->text_color(), 0);
        lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
        lv_obj_set_width(s, LV_PCT(100));

        if (touch_presenter_ != nullptr) {
            ui::mvp::LvglPageAttachTouchHandlers(btn, touch_presenter_);
        }
    }
    attach_swipe_back_list_children(list_panel_, static_cast<lv_obj_t*>(root_));
}

void LvglMeetingPageView::UpdateDetailPanel(const MeetingPageModel& model)
{
    if (detail_panel_ == nullptr || detail_body_ == nullptr) {
        return;
    }
    if (!model.show_detail) {
        lv_obj_add_flag(detail_panel_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(detail_panel_, LV_OBJ_FLAG_HIDDEN);
    std::string body = model.detail_title + "\n[" + model.detail_status + "]\n\n" + model.detail_body;
    lv_label_set_text(detail_body_, body.c_str());
}

}  // namespace ui::meeting
