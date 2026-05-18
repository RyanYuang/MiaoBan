#pragma once

#include <cstdint>
#include <string>

class Display;

namespace ui::meeting {

/** Emote 屏会议纪要：无 LVGL 整页，用 Display 文本 + 触摸交互。 */
class MeetingEmoteUi {
public:
    static MeetingEmoteUi& Instance();

    void Enter(Display* display);
    void Exit();
    bool IsActive() const { return active_; }

    /** 短按：刷新；连续短按两次退出。 */
    void OnShortTap();
    /** 长按：录音并上传。 */
    void StartRecordUpload();
    void RefreshListAsync();

    void SetBusy(bool busy) { busy_ = busy; }

private:
    MeetingEmoteUi() = default;

    void ShowMessage(const char* role, const std::string& text);

    Display* display_ = nullptr;
    bool active_ = false;
    bool busy_ = false;
    uint32_t last_tap_ms_ = 0;
};

}  // namespace ui::meeting
