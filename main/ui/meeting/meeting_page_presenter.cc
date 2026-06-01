#include "meeting_page_presenter.h"

#include "display.h"
#include "meeting_page_view.h"
#include "ui_command_dispatcher.h"
#include "ui_page_ids.h"
#include "ui_page_router.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <sdkconfig.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

#include <functional>
#include <mutex>
#include <new>

#if CONFIG_USE_OYE_CLOUD_API
#include "application.h"
#include "assets/lang_config.h"
#include "audio_codec.h"
#include "board.h"
#include "device_state_machine.h"
#include "mcp_server.h"
#include "oye/oye_audio_util.h"
#include "oye/oye_cloud_api.h"
#include "oye/oye_config.h"
#include "oye/oye_meeting_stream.h"
#include "protocols/oye_metting_protocol.h"

#ifdef CONFIG_USE_OYE_BLE_PROVISIONING
#include "ble/oye_ble_service.h"
#endif

#include <cJSON.h>
#include <cstring>
#endif

namespace ui::meeting {

namespace {

constexpr char TAG[] = "MeetingPresenter";
constexpr uintptr_t kUserDataBack = 0x4241434Bu;
constexpr uintptr_t kUserDataRefresh = 0x52454652u;
constexpr uintptr_t kUserDataRecord = 0x52454344u;
constexpr uintptr_t kUserDataRowBase = 0x4D545230u;
constexpr int kListPageSize = 1;
constexpr uint32_t kMeetingUiTaskStackWords = 4096;
constexpr TickType_t kRecordStopPollTicks = pdMS_TO_TICKS(50);
constexpr TickType_t kMeetingUpstreamReadyWaitTicks = pdMS_TO_TICKS(45000);
constexpr TickType_t kMeetingReadyPollSliceTicks = pdMS_TO_TICKS(200);

/** xTaskCreateStatic 用的栈/TCB 只分配一次并复用；此前每次请求都 malloc 且永不 free，导致 internal 碎片化。 */
struct MeetingUiWorkerPool {
    StackType_t* stack = nullptr;
    StaticTask_t* tcb = nullptr;
    bool busy = false;

    void LogHeap(const char* label) const {
        ESP_LOGI(TAG, "%s: free_internal=%u largest_internal=%u pool_busy=%d",
                 label, static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)), busy ? 1 : 0);
    }

    bool EnsureAllocated() {
        if (stack != nullptr && tcb != nullptr) {
            return true;
        }
        stack = static_cast<StackType_t*>(
            heap_caps_malloc(kMeetingUiTaskStackWords * sizeof(StackType_t), MALLOC_CAP_SPIRAM));
        tcb = static_cast<StaticTask_t*>(heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL));
        if (stack == nullptr || tcb == nullptr) {
            ReleaseIfIdle();
            return false;
        }
        LogHeap("meeting worker pool allocated");
        return true;
    }

    void ReleaseIfIdle() {
        if (busy) {
            return;
        }
        if (stack != nullptr) {
            heap_caps_free(stack);
            stack = nullptr;
        }
        if (tcb != nullptr) {
            heap_caps_free(tcb);
            tcb = nullptr;
        }
    }
};

MeetingUiWorkerPool& MeetingUiWorkerPoolInstance() {
    static MeetingUiWorkerPool pool;
    return pool;
}

/** 会议 ASR / WebSocket 期间临时关闭 BLE 配网，析构或 Restore() 时按原状态恢复。 */
struct BlePauseForMeetingAsr {
    bool was_running = false;
    bool stopped = false;

    BlePauseForMeetingAsr() {
#ifdef CONFIG_USE_OYE_BLE_PROVISIONING
        auto& ble = OyeBleService::GetInstance();
        was_running = ble.IsRunning();
        if (!was_running) {
            return;
        }
        ESP_LOGI(TAG,
                 "pause BLE before meeting ASR (free_internal=%u largest_internal=%u)",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
        if (ble.Stop() == ESP_OK) {
            stopped = true;
            ESP_LOGI(TAG,
                     "BLE paused for meeting ASR (free_internal=%u largest_internal=%u)",
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                     static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
        } else {
            ESP_LOGE(TAG, "failed to pause BLE before meeting ASR");
        }
#endif
    }

    void Restore() {
#ifdef CONFIG_USE_OYE_BLE_PROVISIONING
        if (!was_running || !stopped) {
            return;
        }
        auto& ble = OyeBleService::GetInstance();
        if (ble.IsRunning()) {
            return;
        }
        ESP_LOGI(TAG, "restore BLE after meeting ASR");
        if (ble.Start() != ESP_OK) {
            ESP_LOGE(TAG, "failed to restore BLE after meeting ASR");
        }
        stopped = false;
#endif
    }

    ~BlePauseForMeetingAsr() { Restore(); }
};

void PostUiIfAlive(const std::shared_ptr<std::atomic<bool>>& alive, std::function<void()> fn) {
    if (alive == nullptr || !alive->load()) {
        return;
    }
    UiCommandDispatcher::Instance().Post([alive, fn = std::move(fn)]() mutable {
        if (!alive->load()) {
            return;
        }
        fn();
    });
}

static std::string StatusText(const std::string& status) {
    if (status == "pending") {
        return "等待";
    }
    if (status == "submitting") {
        return "提交中";
    }
    if (status == "transcribing") {
        return "转写中";
    }
    if (status == "summarizing") {
        return "生成纪要";
    }
    if (status == "done") {
        return "完成";
    }
    if (status == "failed") {
        return "失败";
    }
    return status;
}

#if CONFIG_USE_OYE_CLOUD_API
static std::string BuildDetailBody(const oye::MeetingInfo& info) {
    std::string body;
    if (!info.summary.empty()) {
        body += "摘要:\n";
        body += info.summary;
        body += "\n\n";
    }
    if (!info.transcript_text.empty()) {
        body += "转写:\n";
        body += info.transcript_text;
        body += "\n\n";
    }
    if (!info.error_message.empty()) {
        body += "错误: ";
        body += info.error_message;
    }
    if (body.empty()) {
        body = "暂无纪要内容";
    }
    return body;
}
#endif

}  // namespace

void ReleaseMeetingUiWorkerIfIdle() {
    auto& pool = MeetingUiWorkerPoolInstance();
    pool.ReleaseIfIdle();
    pool.LogHeap("meeting worker pool release");
}

MeetingPagePresenter::MeetingPagePresenter(IMeetingPageView* view, Display* display)
    : view_(view), display_(display) {}

void MeetingPagePresenter::BindPageLifetime(std::shared_ptr<std::atomic<bool>> alive) {
    page_alive_ = std::move(alive);
}

bool MeetingPagePresenter::IsPageAlive() const {
    return page_alive_ != nullptr && page_alive_->load();
}

void MeetingPagePresenter::PostUi(std::function<void()> fn) {
    if (!IsPageAlive()) {
        return;
    }
    std::shared_ptr<std::atomic<bool>> alive = page_alive_;
    UiCommandDispatcher::Instance().Post([alive, fn = std::move(fn)]() mutable {
        if (!alive->load()) {
            return;
        }
        fn();
    });
}

void MeetingPagePresenter::Show(const MeetingPageModel& model) {
    model_ = model;
    if (view_ == nullptr) {
        return;
    }
    if (display_ != nullptr) {
        DisplayLockGuard lock(display_);
        view_->Show(model_);
        return;
    }
    view_->Show(model_);
}

void MeetingPagePresenter::OnShow() {
#if CONFIG_USE_OYE_CLOUD_API
    if (!oye::HasAccessToken()) {
        ESP_LOGW(TAG, "OnShow: no access token, skip refresh");
        model_.status_line = "请先在 App 中绑定账号";
        model_.loading = false;
        Show(model_);
        return;
    }
    ESP_LOGI(TAG, "OnShow: has token, refresh list");
    RefreshList();
#else
    model_.status_line = "未启用 Oye 云服务";
    Show(model_);
#endif
}

void MeetingPagePresenter::OnClick(lv_event_t* e) {
    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const uintptr_t ud = reinterpret_cast<uintptr_t>(lv_obj_get_user_data(target));

    if (ud == kUserDataBack) {
        NavigateBack();
        return;
    }
    if (ud == kUserDataRefresh) {
        ESP_LOGI(TAG, "refresh button clicked");
        RefreshList();
        return;
    }
    if (ud == kUserDataRecord) {
        StartRecordAndUpload();
        return;
    }
    if (ud >= kUserDataRowBase && ud < kUserDataRowBase + 100) {
        const int meeting_id = static_cast<int>(ud - kUserDataRowBase);
        OpenDetail(meeting_id);
    }
}

void MeetingPagePresenter::NavigateBack() {
    UiPageRouter::Instance().NavigateBackFromInput();
}

void MeetingPagePresenter::RefreshList() {
#if !CONFIG_USE_OYE_CLOUD_API
    return;
#endif
    if (busy_) {
        ESP_LOGW(TAG, "RefreshList ignored: busy");
        return;
    }
    ESP_LOGI(TAG, "RefreshList start");
    busy_ = true;
    model_.loading = true;
    model_.show_detail = false;
    model_.status_line = "加载会议列表…";
    Show(model_);
    RunNetworkTask(RefreshListTask);
}

void MeetingPagePresenter::OpenDetail(int meeting_id) {
#if !CONFIG_USE_OYE_CLOUD_API
    return;
#endif
    if (busy_ || meeting_id <= 0) {
        ESP_LOGW(TAG, "OpenDetail ignored: busy=%d id=%d", busy_ ? 1 : 0, meeting_id);
        return;
    }
    ESP_LOGI(TAG, "OpenDetail id=%d", meeting_id);
    pending_detail_id_ = meeting_id;
    busy_ = true;
    model_.loading = true;
    model_.status_line = "加载详情…";
    Show(model_);
    RunNetworkTask(LoadDetailTask);
}

void MeetingPagePresenter::StartRecordAndUpload() {
#if !CONFIG_USE_OYE_CLOUD_API
    return;
#endif
    if (recording_) {
        RequestStopRecording();
        return;
    }
    if (busy_) {
        ESP_LOGW(TAG, "StartRecordAndUpload ignored: busy");
        return;
    }
    if (!oye::HasAccessToken()) {
        model_.status_line = "请先绑定账号";
        Show(model_);
        return;
    }
    if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
        model_.status_line = "请先结束当前语音对话";
        Show(model_);
        return;
    }
    ESP_LOGI(TAG, "StartRecordAndUpload: meeting ASR stream start");
    busy_ = true;
    recording_ = true;
    stop_recording_requested_ = false;
    record_stop_flag_ = std::make_shared<std::atomic<bool>>(false);
    model_.loading = true;
    model_.record_button_text = "结束录音";
    model_.status_line = "连接识别服务…";
    Show(model_);
    RunNetworkTask(UploadTask);
}

void MeetingPagePresenter::RequestStopRecording() {
#if !CONFIG_USE_OYE_CLOUD_API
    return;
#endif
    if (!recording_ || stop_recording_requested_) {
        return;
    }
    ESP_LOGI(TAG, "RequestStopRecording");
    stop_recording_requested_ = true;
    if (record_stop_flag_ != nullptr) {
        record_stop_flag_->store(true);
    }
    PostUi([this]() {
        model_.record_button_text = "结束中…";
        model_.status_line = "结束录音…";
        Show(model_);
    });
}

void MeetingPagePresenter::NotifyTaskFailed(const char* status_line) {
    const std::string status = status_line;
    PostUi([this, status]() {
        busy_ = false;
        ResetRecordingState();
        model_.loading = false;
        model_.status_line = status;
        Show(model_);
    });
}

void MeetingPagePresenter::ResetRecordingState() {
    recording_ = false;
    stop_recording_requested_ = false;
    record_stop_flag_.reset();
    model_.record_button_text = "录音纪要";
}

void MeetingPagePresenter::RunNetworkTask(void (*worker)(MeetingPagePresenter* self)) {
    auto& pool = MeetingUiWorkerPoolInstance();
    if (pool.busy) {
        ESP_LOGW(TAG, "RunNetworkTask: worker busy");
        NotifyTaskFailed("请稍候…");
        return;
    }

    struct TaskCtx {
        MeetingPagePresenter* self;
        void (*worker)(MeetingPagePresenter*);
    };
    auto* ctx = new (std::nothrow) TaskCtx{this, worker};
    if (ctx == nullptr) {
        ESP_LOGE(TAG, "RunNetworkTask: alloc ctx failed");
        NotifyTaskFailed("内存不足，操作失败");
        return;
    }

    auto task_entry = [](void* p) {
        auto* task_ctx = static_cast<TaskCtx*>(p);
        ESP_LOGI(TAG, "oye_meeting_ui task running");
        if (task_ctx != nullptr) {
            if (task_ctx->worker != nullptr && task_ctx->self != nullptr) {
                task_ctx->worker(task_ctx->self);
            }
            delete task_ctx;
        }
        MeetingUiWorkerPoolInstance().busy = false;
        vTaskDelete(nullptr);
    };

    pool.busy = true;
    TaskHandle_t handle = nullptr;
#if CONFIG_SPIRAM
    if (pool.EnsureAllocated()) {
        handle = xTaskCreateStatic(task_entry, "oye_meeting_ui", kMeetingUiTaskStackWords, ctx, 5, pool.stack,
                                   pool.tcb);
        if (handle != nullptr) {
            ESP_LOGI(TAG, "oye_meeting_ui started (reused PSRAM stack)");
        } else {
            ESP_LOGE(TAG, "oye_meeting_ui xTaskCreateStatic failed");
        }
    }
#endif
    if (handle == nullptr) {
        if (xTaskCreate(task_entry, "oye_meeting_ui", kMeetingUiTaskStackWords, ctx, 5, &handle) != pdPASS) {
            pool.busy = false;
            pool.LogHeap("oye_meeting_ui task create failed");
            delete ctx;
            NotifyTaskFailed("内存不足，操作失败");
            return;
        }
        ESP_LOGI(TAG, "oye_meeting_ui started (internal stack fallback)");
    }
}

void MeetingPagePresenter::RefreshListTask(MeetingPagePresenter* self) {
#if CONFIG_USE_OYE_CLOUD_API
    const auto alive = self->page_alive_;
    ESP_LOGI(TAG, "RefreshListTask: begin (self=%p)", static_cast<void*>(self));
    std::vector<oye::MeetingInfo> items;
    esp_err_t err = oye::ListMeetings(1, kListPageSize, items);
    ESP_LOGI(TAG, "RefreshListTask: ListMeetings finished err=%s count=%u",
             esp_err_to_name(err), static_cast<unsigned>(items.size()));

    if (alive == nullptr || !alive->load()) {
        ESP_LOGW(TAG, "RefreshListTask: page destroyed before UI update");
        return;
    }
    ESP_LOGI(TAG, "RefreshListTask: posting UI update to ui_cmd");
    PostUiIfAlive(alive, [self, err, items = std::move(items)]() mutable {
        ESP_LOGI(TAG, "RefreshListTask UI callback: err=%s items=%u",
                 esp_err_to_name(err), static_cast<unsigned>(items.size()));
        self->busy_ = false;
        self->model_.loading = false;
        self->model_.meetings.clear();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "RefreshListTask UI: show load failed");
            self->model_.status_line = "加载失败（网络超时或内存不足）";
            self->Show(self->model_);
            return;
        }
        for (const auto& m : items) {
            MeetingRowModel row;
            row.id = m.id;
            row.title = m.title.empty() ? ("会议 #" + std::to_string(m.id)) : m.title;
            row.status = StatusText(m.status);
            row.summary_preview = m.summary;
            if (row.summary_preview.size() > 48) {
                row.summary_preview.resize(48);
                row.summary_preview += "…";
            }
            self->model_.meetings.push_back(std::move(row));
        }
        self->model_.status_line = self->model_.meetings.empty() ? "暂无会议，可录音上传" : "共 " +
                                                                 std::to_string(self->model_.meetings.size()) +
                                                                 " 条";
        ESP_LOGI(TAG, "RefreshListTask UI: %u row(s)", static_cast<unsigned>(self->model_.meetings.size()));
        self->Show(self->model_);
    });
#else
    (void)self;
#endif
}

void MeetingPagePresenter::LoadDetailTask(MeetingPagePresenter* self) {
#if CONFIG_USE_OYE_CLOUD_API
    const auto alive = self->page_alive_;
    const int id = self->pending_detail_id_;
    ESP_LOGI(TAG, "LoadDetailTask: begin id=%d", id);
    oye::MeetingInfo info;
    esp_err_t err = oye::GetMeeting(id, info);
    ESP_LOGI(TAG, "LoadDetailTask: GetMeeting finished err=%s status=%s", esp_err_to_name(err),
             info.status.c_str());

    if (alive == nullptr || !alive->load()) {
        ESP_LOGW(TAG, "LoadDetailTask: page destroyed before UI update");
        return;
    }
    ESP_LOGI(TAG, "LoadDetailTask: posting UI update to ui_cmd");
    PostUiIfAlive(alive, [self, err, info = std::move(info)]() mutable {
        ESP_LOGI(TAG, "LoadDetailTask UI callback: err=%s", esp_err_to_name(err));
        self->busy_ = false;
        self->model_.loading = false;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "LoadDetailTask UI: show failed id=%d", self->pending_detail_id_);
            self->model_.status_line = "详情加载失败";
            self->Show(self->model_);
            return;
        }
        self->model_.show_detail = true;
        self->model_.selected_id = info.id;
        self->model_.detail_title = info.title.empty() ? ("会议 #" + std::to_string(info.id)) : info.title;
        self->model_.detail_status = StatusText(info.status);
        self->model_.detail_body = BuildDetailBody(info);
        self->model_.status_line = "会议 #" + std::to_string(info.id);
        self->Show(self->model_);
    });
#else
    (void)self;
#endif
}

void MeetingPagePresenter::UploadTask(MeetingPagePresenter* self) {
#if CONFIG_USE_OYE_CLOUD_API
    const auto alive = self->page_alive_;
    const auto stop_flag = self->record_stop_flag_;
    ESP_LOGI(TAG, "UploadTask: meeting ASR stream begin");
    auto stream = std::make_unique<oye::MeetingStream>();
    oye::MeetingStream* stream_ptr = stream.get();
    std::mutex result_mutex;
    std::string final_text;
    int final_meeting_id = 0;
    Application::GetInstance().SetOtaCheckSuspended(true);
    auto restore_ota = []() { Application::GetInstance().SetOtaCheckSuspended(false); };
    BlePauseForMeetingAsr ble_pause;

    const bool stream_ok = stream_ptr->Start(
        "设备端会议",
        true,
        [alive, self, &result_mutex, &final_text](const std::string& text) {
            {
                std::lock_guard<std::mutex> lock(result_mutex);
                final_text = text;
            }
            PostUiIfAlive(alive, [self, text]() {
                self->model_.show_detail = true;
                self->model_.detail_title = "实时转写";
                self->model_.detail_status = "识别中";
                self->model_.detail_body = text.empty() ? "正在接收语音…" : text;
                self->model_.status_line = "实时转写中…";
                self->Show(self->model_);
            });
        },
        [alive, self, &result_mutex, &final_text, &final_meeting_id](const std::string& text, int meeting_id) {
            {
                std::lock_guard<std::mutex> lock(result_mutex);
                final_text = text;
                final_meeting_id = meeting_id;
            }
            PostUiIfAlive(alive, [self, text]() {
                self->model_.show_detail = true;
                self->model_.detail_title = "会议转写";
                self->model_.detail_status = "已提交";
                self->model_.detail_body = text.empty() ? "转写完成" : text;
                self->model_.status_line = "转写完成，正在生成纪要…";
                self->Show(self->model_);
            });
        });
    if (!stream_ok) {
        ESP_LOGE(TAG, "UploadTask: MeetingStream::Start failed");
        PostUiIfAlive(alive, [self]() {
            self->busy_ = false;
            self->ResetRecordingState();
            self->model_.loading = false;
            self->model_.status_line = "连接识别服务失败";
            self->Show(self->model_);
        });
        restore_ota();
        return;
    }

    PostUiIfAlive(alive, [self]() {
        self->model_.show_detail = true;
        self->model_.detail_title = "会议录音";
        self->model_.detail_status = "连接中";
        self->model_.detail_body = "正在等待识别服务就绪…";
        self->model_.status_line = "等待识别就绪…";
        self->Show(self->model_);
    });

    const TickType_t ready_deadline = xTaskGetTickCount() + kMeetingUpstreamReadyWaitTicks;
    bool upstream_ready = stream_ptr->IsUpstreamReady();
    while (!upstream_ready && stream_ptr->IsRunning()) {
        if (stop_flag != nullptr && stop_flag->load()) {
            ESP_LOGI(TAG, "UploadTask: stop requested while waiting for ready");
            break;
        }
        if (alive == nullptr || !alive->load()) {
            break;
        }
        const TickType_t now = xTaskGetTickCount();
        if (now >= ready_deadline) {
            ESP_LOGW(TAG, "UploadTask: wait for WS ready timeout");
            break;
        }
        TickType_t slice = ready_deadline - now;
        if (slice > kMeetingReadyPollSliceTicks) {
            slice = kMeetingReadyPollSliceTicks;
        }
        upstream_ready = stream_ptr->WaitForUpstreamReady(slice);
    }

    if (!upstream_ready) {
        ESP_LOGE(TAG, "UploadTask: upstream not ready (running=%d interrupted=%d)",
                 stream_ptr->IsRunning() ? 1 : 0, stream_ptr->WasInterrupted() ? 1 : 0);
        stream_ptr->Stop(false, false);
        stream.reset();
        ble_pause.Restore();
        restore_ota();
        const bool user_cancel = stop_flag != nullptr && stop_flag->load();
        PostUiIfAlive(alive, [self, user_cancel]() {
            self->busy_ = false;
            self->ResetRecordingState();
            self->model_.loading = false;
            self->model_.status_line = user_cancel ? "已取消" : "识别服务未就绪";
            self->Show(self->model_);
        });
        return;
    }

    ESP_LOGI(TAG, "UploadTask: upstream ready, start microphone and PCM uplink");

    auto& audio = Application::GetInstance().GetAudioService();
    const bool was_processing = audio.IsAudioProcessorRunning();
    const bool was_wake = audio.IsWakeWordRunning();
    bool audio_streaming_started = false;
    auto restore_audio = [&]() {
        if (!audio_streaming_started) {
            return;
        }
        audio.ClearPcmTap();
        if (!was_processing) {
            audio.EnableVoiceProcessing(false);
        }
        if (was_wake) {
            audio.EnableWakeWordDetection(true);
        }
        audio_streaming_started = false;
    };

    audio.ClearPcmTap();
    if (was_wake) {
        audio.EnableWakeWordDetection(false);
    }
    if (!was_processing) {
        audio.EnableVoiceProcessing(true);
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    auto first_tap_logged = std::make_shared<std::atomic<bool>>(false);
    audio.SetPcmTap([stream_ptr, first_tap_logged](const std::vector<int16_t>& pcm) {
        if (stream_ptr == nullptr || !stream_ptr->IsRunning() || !stream_ptr->IsUpstreamReady() ||
            pcm.empty()) {
            return;
        }
        if (!first_tap_logged->exchange(true)) {
            ESP_LOGI(TAG, "UploadTask: first PCM tap feed samples=%u", static_cast<unsigned>(pcm.size()));
        }
        stream_ptr->FeedPcm(pcm.data(), pcm.size());
    });
    audio_streaming_started = true;

    PostUiIfAlive(alive, [self]() {
        self->model_.show_detail = true;
        self->model_.detail_title = "实时转写";
        self->model_.detail_status = "录音中";
        self->model_.detail_body = "请开始讲话，再次点击按钮结束录音";
        self->model_.status_line = "录音与识别中…";
        self->Show(self->model_);
    });
    while (stream_ptr->IsRunning()) {
        if (stop_flag != nullptr && stop_flag->load()) {
            break;
        }
        if (alive == nullptr || !alive->load()) {
            break;
        }
        vTaskDelay(kRecordStopPollTicks);
    }
    restore_audio();

    const bool user_requested_stop = stop_flag != nullptr && stop_flag->load();
    if (!user_requested_stop && stream_ptr->WasInterrupted()) {
        const std::string partial_text = stream_ptr->LatestText();
        ESP_LOGW(TAG, "UploadTask: meeting ASR interrupted before user stop");
        stream_ptr->Stop(false, false);
        stream.reset();
        ble_pause.Restore();
        restore_ota();
        PostUiIfAlive(alive, [self, partial_text]() {
            self->busy_ = false;
            self->ResetRecordingState();
            self->model_.loading = false;
            self->model_.show_detail = true;
            self->model_.detail_title = "实时转写";
            self->model_.detail_status = "连接中断";
            self->model_.detail_body = partial_text.empty() ? "识别连接中断，请重试" : partial_text;
            self->model_.status_line = "录音被意外中断";
            self->Show(self->model_);
        });
        return;
    }

    PostUiIfAlive(alive, [self]() {
        self->model_.detail_status = "收尾中";
        self->model_.status_line = "结束录音，等待识别完成…";
        self->Show(self->model_);
    });

    stream_ptr->Stop(true);

    std::string transcript = stream_ptr->LatestText();
    int meeting_id = 0;
    {
        std::lock_guard<std::mutex> lock(result_mutex);
        if (!final_text.empty()) {
            transcript = final_text;
        }
        meeting_id = final_meeting_id;
    }
    ESP_LOGI(TAG, "UploadTask: stream finished meeting_id=%d transcript_chars=%u", meeting_id,
             static_cast<unsigned>(transcript.size()));
    stream.reset();
    restore_ota();

    if (meeting_id <= 0) {
        PostUiIfAlive(alive, [self, transcript]() {
            self->busy_ = false;
            self->ResetRecordingState();
            self->model_.loading = false;
            self->model_.show_detail = true;
            self->model_.detail_title = "会议转写";
            self->model_.detail_status = "失败";
            self->model_.detail_body = transcript.empty() ? "未识别到有效语音内容" : transcript;
            self->model_.status_line = "纪要提交失败";
            self->Show(self->model_);
        });
        return;
    }

    PostUiIfAlive(alive, [self, transcript]() {
        self->model_.show_detail = true;
        self->model_.detail_title = "会议转写";
        self->model_.detail_status = "生成中";
        self->model_.detail_body = transcript.empty() ? "转写完成，等待纪要生成…" : transcript;
        self->model_.status_line = "纪要生成中…";
        self->Show(self->model_);
    });

    ESP_LOGI(TAG, "UploadTask: poll meeting_id=%d", meeting_id);
    oye::MeetingInfo info;
    esp_err_t poll = oye::PollMeetingUntilDone(meeting_id, info, 300);
    ESP_LOGI(TAG, "UploadTask: poll err=%s status=%s", esp_err_to_name(poll), info.status.c_str());

    if (alive == nullptr || !alive->load()) {
        ESP_LOGW(TAG, "UploadTask: page destroyed before UI update");
        return;
    }
    PostUiIfAlive(alive, [self, poll, info = std::move(info)]() mutable {
        self->busy_ = false;
        self->ResetRecordingState();
        self->model_.loading = false;
        if (poll != ESP_OK) {
            ESP_LOGW(TAG, "UploadTask UI: poll failed");
            self->model_.status_line = "处理超时或失败";
            self->Show(self->model_);
            return;
        }
        self->model_.show_detail = true;
        self->model_.selected_id = info.id;
        self->model_.detail_title = info.title.empty() ? "新会议" : info.title;
        self->model_.detail_status = StatusText(info.status);
        self->model_.detail_body = BuildDetailBody(info);
        self->model_.status_line = "纪要生成完成";
        self->Show(self->model_);
    });
#else
    (void)self;
#endif
}

void MeetingPagePresenter::MeetingTask(MeetingPagePresenter* self) {
#if CONFIG_USE_OYE_CLOUD_API
    (void)self;
    ESP_LOGI(TAG, "MeetingTask: begin");
    BlePauseForMeetingAsr ble_pause;

    // 创建对话流实例
    ESP_LOGI(TAG, "MeetingTask: create stream");
    auto stream = std::make_unique<oye::MeetingStream>();
    if (stream == nullptr) {
        ESP_LOGW(TAG, "MeetingTask: stream is not initialized");
        return;
    }

    // 启动对话流
    ESP_LOGI(TAG, "MeetingTask: start stream");
    stream->Start("会议", true, [](const std::string& text) {
        ESP_LOGI(TAG, "MeetingTask: text=%s", text.c_str());
    }, [](const std::string& text, int meeting_id) {
        ESP_LOGI(TAG, "MeetingTask: text=%s meeting_id=%d", text.c_str(), meeting_id);
    });

    // 等待对话流结束
    ESP_LOGI(TAG, "MeetingTask: wait stream end");
    while (stream->IsRunning()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    // 获取对话流文本
    ESP_LOGI(TAG, "MeetingTask: get stream text");
    const std::string text = stream->LatestText();
    ESP_LOGI(TAG, "MeetingTask: text=%s", text.c_str());

    // 停止对话流
    stream->Stop();

    // 释放对话流实例
    stream.reset();

    ESP_LOGI(TAG, "MeetingTask: end");
#else
    (void)self;
#endif
}

/**
 * 创建会议语音协议实例并注册回调（安装到 Application::protocol_）。
 * Oye 固件：OyeMettingProtocol（/mcu/voice-chat/ws）。
 */
void MeetingPagePresenter::InitializeProtocol() {
#if !CONFIG_USE_OYE_CLOUD_API
    ESP_LOGW(TAG, "InitializeProtocol: Oye cloud API disabled");
    return;
#else
    Application& app = Application::GetInstance();
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    UiCommandDispatcher::Instance().Post([display]() {
        display->SetStatus(Lang::Strings::LOADING_PROTOCOL);
    });

    {
        auto oye_proto = std::make_unique<OyeMettingProtocol>();
        oye_proto->SetMainScheduler([&app](std::function<void()> fn) { app.Schedule(std::move(fn)); });
        app.protocol_ = std::move(oye_proto);
        ESP_LOGI(TAG, "Voice protocol: OyeMettingProtocol (meeting voice-chat)");
        if (!oye::HasAccessToken()) {
            ESP_LOGW(TAG, "No access_token yet; bind via BLE SET_USER_TOKEN before meeting voice");
        }
    }

    app.protocol_->OnConnected([&app]() {
        UiCommandDispatcher::Instance().Post([&app]() {
            app.DismissAlert();
        });
    });

    app.protocol_->OnNetworkError([&app](const std::string& message) {
        app.last_error_message_ = message;
        xEventGroupSetBits(app.event_group_, MAIN_EVENT_ERROR);
    });

    app.protocol_->OnIncomingAudio([&app](std::unique_ptr<AudioStreamPacket> packet) {
        if (app.GetDeviceState() == kDeviceStateSpeaking) {
            app.audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });

    app.protocol_->OnAudioChannelOpened([&app, codec, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        if (app.protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG,
                     "Server sample rate %d does not match device output sample rate %d, resampling may cause "
                     "distortion",
                     app.protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });

    app.protocol_->OnAudioChannelClosed([&app, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        UiCommandDispatcher::Instance().Post([&app]() {
            auto meeting_display = Board::GetInstance().GetDisplay();
            meeting_display->SetChatMessage("system", "");
            app.SetDeviceState(kDeviceStateIdle);
        });
    });

    app.protocol_->OnIncomingJson([&app, display](const cJSON* root) {
        auto type = cJSON_GetObjectItem(root, "type");
        if (type == nullptr || !cJSON_IsString(type)) {
            ESP_LOGW(TAG, "Incoming JSON missing type");
            return;
        }
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (state == nullptr || !cJSON_IsString(state)) {
                return;
            }
            if (strcmp(state->valuestring, "start") == 0) {
                app.Schedule([&app]() {
                    app.aborted_ = false;
                    app.deferred_tts_stop_state_ = kDeviceStateUnknown;
                    app.SetDeviceState(kDeviceStateSpeaking);
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                app.Schedule([&app]() {
                    if (app.GetDeviceState() == kDeviceStateSpeaking) {
                        const DeviceState target_state = app.listening_mode_ == kListeningModeManualStop
                                                             ? kDeviceStateIdle
                                                             : kDeviceStateListening;
                        if (app.IsAwaitingVoiceResult()) {
                            app.deferred_tts_stop_state_ = target_state;
                            ESP_LOGI(TAG, "TTS stop: defer state transition to %s until playback drains",
                                     DeviceStateMachine::GetStateName(target_state));
                            return;
                        }
                        app.SetDeviceState(target_state);
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    UiCommandDispatcher::Instance().Post([&app, display, message = std::string(text->valuestring)]() {
                        display->SetChatMessage("assistant", message.c_str());
                        app.NotifyAssistantTextListeners(message);
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                UiCommandDispatcher::Instance().Post([&app, display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("user", message.c_str());
                    app.NotifyRecognitionTextListeners(message);
                });
            }
        } else if (strcmp(type->valuestring, "chat_stage") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (cJSON_IsString(state) && strcmp(state->valuestring, "thinking") == 0) {
                UiCommandDispatcher::Instance().Post([&app]() {
                    app.NotifyChatStatusListeners("ASR 已结束，正在思考…");
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                UiCommandDispatcher::Instance().Post(
                    [display, emotion_str = std::string(emotion->valuestring)]() {
                        display->SetEmotion(emotion_str.c_str());
                    });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    app.Schedule([&app]() {
                        app.Reboot();
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                std::string s_status(status->valuestring);
                std::string s_message(message->valuestring);
                std::string s_emotion(emotion->valuestring);
                UiCommandDispatcher::Instance().Post([&app, s_status = std::move(s_status), s_message = std::move(s_message),
                                                      s_emotion = std::move(s_emotion)]() {
                    app.Alert(s_status.c_str(), s_message.c_str(), s_emotion.c_str(), Lang::Sounds::OGG_VIBRATION);
                });
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
            if (cJSON_IsObject(payload)) {
                UiCommandDispatcher::Instance().Post(
                    [display, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
                        display->SetChatMessage("system", payload_str.c_str());
                    });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });

    app.protocol_->Start();
#endif
}

}  // namespace ui::meeting
