#pragma once

#include <string>

namespace ui::mvp {

// View contract for the conversation (subtitle) area. Intentionally mirrors
// the role + content shape of Display::SetChatMessage for future LcdDisplay adapters.
//
// Do not include lvgl.h here. Implementations may wrap LcdDisplay with DisplayLockGuard.
class IConversationView {
public:
    virtual ~IConversationView() = default;

    virtual void ShowSystemLine(std::string text) = 0;
    virtual void ShowUserLine(std::string text) = 0;
    virtual void ShowAssistantLine(std::string text) = 0;
    virtual void Clear() = 0;
};

}  // namespace ui::mvp
