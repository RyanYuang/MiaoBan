#pragma once

#include <string>
#include <vector>

namespace ui::mvp {

// Pure UI snapshot types; keep independent from protocol JSON or Display.

enum class ChatRole {
    kSystem,
    kUser,
    kAssistant,
};

struct ChatLine {
    ChatRole role;
    std::string text;
};

// Ordered lines to render in the conversation / subtitle region.
struct ConversationModel {
    std::vector<ChatLine> lines;
};

}  // namespace ui::mvp
