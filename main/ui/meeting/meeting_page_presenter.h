#pragma once

#include "lvgl_page_touch_presenter.h"
#include "meeting_page_model.h"

#include <atomic>
#include <functional>
#include <memory>

class Display;

namespace ui::meeting {

/** 会议页销毁且后台任务空闲时释放复用的 PSRAM 栈 / TCB（见 meeting_page_presenter.cc）。 */
void ReleaseMeetingUiWorkerIfIdle();

class IMeetingPageView;

class MeetingPagePresenter final : public ui::mvp::LvglPageTouchPresenter {
public:
    explicit MeetingPagePresenter(IMeetingPageView* view, Display* display);

    void BindPageLifetime(std::shared_ptr<std::atomic<bool>> alive);

    void Show(const MeetingPageModel& model);
    void OnShow();

    void OnClick(lv_event_t* e) override;

private:
    void NavigateBack();
    void RefreshList();
    void OpenDetail(int meeting_id);
    void StartRecordAndUpload();
    void RunNetworkTask(void (*worker)(MeetingPagePresenter* self));
    void NotifyTaskFailed(const char* status_line);
    void PostUi(std::function<void()> fn);
    bool IsPageAlive() const;

    static void RefreshListTask(MeetingPagePresenter* self);
    static void LoadDetailTask(MeetingPagePresenter* self);
    static void UploadTask(MeetingPagePresenter* self);
    static void MeetingTask(MeetingPagePresenter* self);
    static void InitializeProtocol();

    IMeetingPageView* view_;
    Display* display_ = nullptr;
    std::shared_ptr<std::atomic<bool>> page_alive_;
    MeetingPageModel model_;
    bool busy_ = false;
    int pending_detail_id_ = 0;
};

}  // namespace ui::meeting
