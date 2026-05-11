#pragma once

#include "presenter_base.h"

namespace ui::mvp {

class IConversationView;
struct ConversationModel;

// Example presenter: maps ConversationModel to IConversationView calls only.
// Not registered from Application yet; safe to construct from tests or future wiring.
class ExampleConversationPresenter final : public PresenterBase {
public:
    explicit ExampleConversationPresenter(IConversationView* view);

    void ApplySnapshot(const ConversationModel& model);

private:
    IConversationView* view_;
};

}  // namespace ui::mvp
