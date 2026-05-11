#pragma once

namespace ui::mvp {

// Base for UI presenters: no LVGL, no widget types.
//
// Threading: presenters do not assume a particular task. Any IView implementation
// that touches LVGL must marshal updates to the LVGL task and use DisplayLockGuard
// (or equivalent) inside the view, not in generic presenter logic.
class PresenterBase {
public:
    PresenterBase() = default;
    virtual ~PresenterBase() = default;

    PresenterBase(const PresenterBase&) = delete;
    PresenterBase& operator=(const PresenterBase&) = delete;
    PresenterBase(PresenterBase&&) = delete;
    PresenterBase& operator=(PresenterBase&&) = delete;
};

}  // namespace ui::mvp
