#include "null_conversation_view.h"

namespace ui::mvp {

void NullConversationView::ShowSystemLine(std::string /*text*/) {}

void NullConversationView::ShowUserLine(std::string /*text*/) {}

void NullConversationView::ShowAssistantLine(std::string /*text*/) {}

void NullConversationView::Clear() {}

}  // namespace ui::mvp
