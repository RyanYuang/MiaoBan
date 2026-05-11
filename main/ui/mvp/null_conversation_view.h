#pragma once

#include "conversation_view.h"

namespace ui::mvp {

// No-op view for tests or wiring before an LVGL-backed adapter exists.
class NullConversationView final : public IConversationView {
public:
    void ShowSystemLine(std::string text) override;
    void ShowUserLine(std::string text) override;
    void ShowAssistantLine(std::string text) override;
    void Clear() override;
};

}  // namespace ui::mvp
