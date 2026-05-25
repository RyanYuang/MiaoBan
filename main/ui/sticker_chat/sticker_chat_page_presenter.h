#pragma once

#include "device_state.h"
#include "lvgl_page_touch_presenter.h"

#include <string>

class Display;

namespace ui::sticker_chat {

/**
 * 贴图对话页：返回仅出栈；点击 Chat_btn 只 ToggleChatState（不关闭叠页）。
 * 底部提示文案随状态机更新；结束收音后先显示「等待识别结果…」，
 * 只有收到后端 llm_start 时才切到「正在思考…」。
 */
class StickerChatPagePresenter final : public ui::mvp::LvglPageTouchPresenter {
public:
    StickerChatPagePresenter() = default;
    ~StickerChatPagePresenter() override;

    void OnClick(lv_event_t* e) override;

    /** 在 BuildLayout 末尾调用：`page_alive` 在根 DELETE 前置 false，避免异步改已删控件。 */
    void AttachStateUi(Display* display, bool* page_alive, void* hint_label, void* transcript_label);

private:
    void OnDeviceState(DeviceState old_state, DeviceState new_state);
    void OnRecognitionText(const std::string& text);
    void OnAssistantText(const std::string& text);
    void OnChatStatus(const std::string& text);

    Display* display_ = nullptr;
    bool* page_alive_ = nullptr;
    void* hint_label_ = nullptr;
    void* transcript_label_ = nullptr;
    int listener_id_ = -1;
    int recognition_listener_id_ = -1;
    int assistant_listener_id_ = -1;
    int chat_status_listener_id_ = -1;
};

}  // namespace ui::sticker_chat
