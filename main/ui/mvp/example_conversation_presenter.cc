#include "example_conversation_presenter.h"

#include "conversation_model.h"
#include "conversation_view.h"

namespace ui::mvp {

ExampleConversationPresenter::ExampleConversationPresenter(IConversationView* view)
    : view_(view) {}

void ExampleConversationPresenter::ApplySnapshot(const ConversationModel& model) {
    if (view_ == nullptr) {
        return;
    }
    view_->Clear();
    for (const auto& line : model.lines) {
        switch (line.role) {
            case ChatRole::kSystem:
                view_->ShowSystemLine(line.text);
                break;
            case ChatRole::kUser:
                view_->ShowUserLine(line.text);
                break;
            case ChatRole::kAssistant:
                view_->ShowAssistantLine(line.text);
                break;
        }
    }
}

}  // namespace ui::mvp
