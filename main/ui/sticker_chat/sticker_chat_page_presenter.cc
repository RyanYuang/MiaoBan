#include "sticker_chat_page_presenter.h"

#include "application.h"
#include "ui_command_dispatcher.h"
#include "ui_page_router.h"

#include <cstdint>
#include <lvgl.h>

namespace ui::sticker_chat {

namespace {

/** lv_obj user_data：区分点击目标（避免依赖控件指针比较）。 */
constexpr uintptr_t kUserDataBack = 0x4241434Bu;      // 'BACK'
constexpr uintptr_t kUserDataSticker = 0x53544B52u;   // 'STKR' sticker / music btn

}  // namespace

void StickerChatPagePresenter::OnClick(lv_event_t* e)
{
    if (e == nullptr) {
        return;
    }
    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const uintptr_t ud = reinterpret_cast<uintptr_t>(lv_obj_get_user_data(target));

    if (ud == kUserDataBack) {
        UiPageRouter::Instance().PostNavigateBack();
        return;
    }
    if (ud == kUserDataSticker) {
        UiPageRouter::Instance().PostNavigateBack();
        UiCommandDispatcher::Instance().Post([]() { Application::GetInstance().ToggleChatState(); });
        return;
    }
}

}  // namespace ui::sticker_chat
