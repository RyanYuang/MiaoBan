#include "lvgl_about_page_view.h"

#include "about_page_model.h"
#include "about_page_presenter.h"
#include "display.h"
#include "lvgl_page_touch_presenter.h"
#include "lvgl_theme.h"
#include "system_info.h"

#include <lvgl.h>

namespace ui::about {

namespace {

struct AboutMvpBundle {
    AboutPagePresenter* presenter = nullptr;
    LvglAboutPageView* view = nullptr;
};

static void AboutRootOnDelete(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    lv_obj_t* root = static_cast<lv_obj_t*>(lv_event_get_target(e));
    auto* bundle = static_cast<AboutMvpBundle*>(lv_obj_get_user_data(root));
    if (bundle == nullptr) {
        return;
    }
    delete bundle->presenter;
    delete bundle->view;
    delete bundle;
    lv_obj_set_user_data(root, nullptr);
}

}  // namespace

void* LvglAboutPageView::CreateRouterPageRoot(Display* display, LvglTheme* theme)
{
    if (display == nullptr || theme == nullptr) {
        return nullptr;
    }

    auto* view = new LvglAboutPageView(display, theme);
    auto* presenter = new AboutPagePresenter(view);
    view->BindTouchPresenter(presenter);

    AboutPageModel model;
    model.title = "About";
    model.body = SystemInfo::GetChipModelName();
    model.body += "\n";
    model.body += SystemInfo::GetMacAddress();
    model.body += "\n\n";
    model.body += SystemInfo::GetUserAgent();

    presenter->Show(model);

    lv_obj_t* root = static_cast<lv_obj_t*>(view->RootHandle());
    if (root == nullptr) {
        delete presenter;
        delete view;
        return nullptr;
    }

    auto* bundle = new AboutMvpBundle{presenter, view};
    lv_obj_set_user_data(root, bundle);
    lv_obj_add_event_cb(root, AboutRootOnDelete, LV_EVENT_DELETE, nullptr);
    return root;
}

LvglAboutPageView::LvglAboutPageView(Display* display, LvglTheme* theme)
    : display_(display), theme_(theme) {}

LvglAboutPageView::~LvglAboutPageView() {
    root_ = nullptr;
}

void LvglAboutPageView::BuildLayout(const AboutPageModel& model)
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
    lv_obj_add_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(panel, theme_->spacing(4), 0);
    lv_obj_set_style_pad_row(panel, theme_->spacing(3), 0);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, model.title.empty() ? "About" : model.title.c_str());
    lv_obj_set_style_text_font(title, theme_->text_font()->font(), 0);
    lv_obj_set_style_text_color(title, theme_->text_color(), 0);

    lv_obj_t* details = lv_label_create(panel);
    lv_label_set_text(details, model.body.c_str());
    lv_obj_set_style_text_font(details, theme_->text_font()->font(), 0);
    lv_obj_set_style_text_color(details, theme_->text_color(), 0);
    const lv_coord_t side = theme_->spacing(4);
    lv_obj_set_width(details, LV_HOR_RES - 2 * side);
    lv_label_set_long_mode(details, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(details, LV_TEXT_ALIGN_LEFT, 0);

    if (presenter_for_touch_ != nullptr) {
        ui::mvp::LvglPageAttachTouchHandlers(panel, presenter_for_touch_);
    }

    root_ = panel;
}

void LvglAboutPageView::Show(const AboutPageModel& model) {
    BuildLayout(model);
}

}  // namespace ui::about
