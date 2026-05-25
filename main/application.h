#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <string>
#include <mutex>
#include <deque>
#include <memory>
#include <atomic>
#include <cstdint>
#include <vector>

#include "protocol.h"
#include "ota.h"
#include "audio_service.h"
#include "device_state.h"
#include "device_state_machine.h"

// Main event bits
#define MAIN_EVENT_SCHEDULE             (1 << 0)
#define MAIN_EVENT_SEND_AUDIO           (1 << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED   (1 << 2)
#define MAIN_EVENT_VAD_CHANGE           (1 << 3)
#define MAIN_EVENT_ERROR                (1 << 4)
#define MAIN_EVENT_ACTIVATION_DONE      (1 << 5)
#define MAIN_EVENT_CLOCK_TICK           (1 << 6)
#define MAIN_EVENT_NETWORK_CONNECTED    (1 << 7)
#define MAIN_EVENT_NETWORK_DISCONNECTED (1 << 8)
#define MAIN_EVENT_TOGGLE_CHAT          (1 << 9)
#define MAIN_EVENT_START_LISTENING      (1 << 10)
#define MAIN_EVENT_STOP_LISTENING       (1 << 11)
#define MAIN_EVENT_STATE_CHANGED        (1 << 12)
#define MAIN_EVENT_SPEECH_END_DETECTED  (1 << 13)
#define MAIN_EVENT_MAX_LISTEN_TIMEOUT   (1 << 14)


enum AecMode {
    kAecOff,
    kAecOnDeviceSide,
    kAecOnServerSide,
};

enum class SpeechEndTimerReason : uint8_t {
    kNone,
    kSilence,
    kNoSpeech,
};

enum class SpeechActivitySource : uint8_t {
    kAfeVad,
    kPcmLevel,
};

class Application {
public:
    using RecognitionTextCallback = std::function<void(const std::string& text)>;
    using AssistantTextCallback = std::function<void(const std::string& text)>;
    using ChatStatusCallback = std::function<void(const std::string& text)>;

    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    // Delete copy constructor and assignment operator
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /**
     * Initialize the application
     * This sets up display, audio, network callbacks, etc.
     * Network connection starts asynchronously.
     */
    void Initialize();

    /**
     * Run the main event loop
     * This function runs in the main task and never returns.
     * It handles all events including network, state changes, and user interactions.
     */
    void Run();

    DeviceState GetDeviceState() const { return state_machine_.GetState(); }
    bool IsVoiceDetected() const { return audio_service_.IsVoiceDetected(); }

    /** 订阅设备状态迁移（回调在 `TransitionTo` 调用方上下文中同步触发；改 UI 请投递到 ui_cmd）。 */
    int AddDeviceStateChangeListener(DeviceStateMachine::StateCallback callback);
    void RemoveDeviceStateChangeListener(int listener_id);
    int AddRecognitionTextListener(RecognitionTextCallback callback);
    void RemoveRecognitionTextListener(int listener_id);
    int AddAssistantTextListener(AssistantTextCallback callback);
    void RemoveAssistantTextListener(int listener_id);
    int AddChatStatusListener(ChatStatusCallback callback);
    void RemoveChatStatusListener(int listener_id);

    /**
     * Request state transition
     * Returns true if transition was successful
     */
    bool SetDeviceState(DeviceState state);

    /**
     * Schedule a callback to be executed in the main task
     */
    void Schedule(std::function<void()>&& callback);

    /**
     * Alert with status, message, emotion and optional sound
     */
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");
    void DismissAlert();

    void AbortSpeaking(AbortReason reason);
#if CONFIG_OYE_SPEECH_END_DETECTION
    void ObserveUplinkPcmForSpeechEnd(const int16_t* samples, size_t count);
#endif

    /**
     * Toggle chat state (event-based, thread-safe)
     * Sends MAIN_EVENT_TOGGLE_CHAT to be handled in Run()
     */
    void ToggleChatState();

    /**
     * Start listening (event-based, thread-safe)
     * Sends MAIN_EVENT_START_LISTENING to be handled in Run()
     */
    void StartListening();

    /**
     * Stop listening (event-based, thread-safe)
     * Sends MAIN_EVENT_STOP_LISTENING to be handled in Run()
     */
    void StopListening();

    void Reboot();
    void WakeWordInvoke(const std::string& wake_word);
    bool UpgradeFirmware(const std::string& url, const std::string& version = "");
    bool CanEnterSleepMode();
    void SendMcpMessage(const std::string& payload);
    void SetAecMode(AecMode mode);
    AecMode GetAecMode() const { return aec_mode_; }
    void PlaySound(const std::string_view& sound);
    AudioService& GetAudioService() { return audio_service_; }
    
    /**
     * Reset protocol resources (thread-safe)
     * Can be called from any task to release resources allocated after network connected
     * This includes closing audio channel, resetting protocol and ota objects
     */
    void ResetProtocol();

private:
    Application();
    ~Application();

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    std::unique_ptr<Protocol> protocol_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    esp_timer_handle_t speech_end_timer_handle_ = nullptr;
    esp_timer_handle_t max_listen_timer_handle_ = nullptr;
    DeviceStateMachine state_machine_;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    AecMode aec_mode_ = kAecOff;
    std::string last_error_message_;
    AudioService audio_service_;
    std::unique_ptr<Ota> ota_;

    bool has_server_time_ = false;
    bool aborted_ = false;
    bool assets_version_checked_ = false;
    bool play_popup_on_listening_ = false;  // Flag to play popup sound after state changes to listening
    int clock_ticks_ = 0;
    TaskHandle_t activation_task_handle_ = nullptr;
    std::mutex recognition_text_listeners_mutex_;
    std::vector<std::pair<int, RecognitionTextCallback>> recognition_text_listeners_;
    int next_recognition_text_listener_id_ = 0;
    std::mutex assistant_text_listeners_mutex_;
    std::vector<std::pair<int, AssistantTextCallback>> assistant_text_listeners_;
    int next_assistant_text_listener_id_ = 0;
    std::mutex chat_status_listeners_mutex_;
    std::vector<std::pair<int, ChatStatusCallback>> chat_status_listeners_;
    int next_chat_status_listener_id_ = 0;
    std::atomic<bool> afe_vad_speaking_{false};
    std::atomic<bool> pcm_level_speaking_{false};
    std::atomic<bool> speech_activity_speaking_{false};
    std::atomic<bool> speech_activity_seen_{false};
    bool speech_started_ = false;
    bool speech_end_waiting_ = false;
    SpeechEndTimerReason speech_end_timer_reason_ = SpeechEndTimerReason::kNone;


    // Event handlers
    /**
     * 处理 MAIN_EVENT_STATE_CHANGED：根据新设备状态同步界面、LED 与音频链路。
     * 在状态机完成迁移后，于主任务中调用。
     */
    void HandleStateChangedEvent();
    void HandleToggleChatEvent();
    void HandleStartListeningEvent();
    void HandleStopListeningEvent();
    void HandleNetworkConnectedEvent();
    void HandleNetworkDisconnectedEvent();
    void HandleActivationDoneEvent();
    void HandleWakeWordDetectedEvent();
    void HandleVadChangeEvent();
    void HandleSpeechEndDetectedEvent();
    void HandleMaxListenTimeoutEvent();
    void ContinueOpenAudioChannel(ListeningMode mode);
    void ContinueWakeWordInvoke(const std::string& wake_word);

    // Activation task (runs in background)
    void ActivationTask();

    // Helper methods
    void CheckAssetsVersion();
    void CheckNewVersion();
    void InitializeProtocol();
    void NotifyRecognitionTextListeners(const std::string& text);
    void NotifyAssistantTextListeners(const std::string& text);
    void NotifyChatStatusListeners(const std::string& text);
    void ShowActivationCode(const std::string& code, const std::string& message);
    void SetListeningMode(ListeningMode mode);
    ListeningMode GetDefaultListeningMode() const;
    void BeginSpeechEndDetection();
    void ResetSpeechEndDetection();
    void ArmSpeechEndTimer(SpeechEndTimerReason reason, uint32_t timeout_ms);
    void ArmMaxListenTimer(uint32_t timeout_ms);
    void UpdateSpeechActivity(SpeechActivitySource source, bool speaking);
    
    // State change handler called by state machine
    void OnStateChanged(DeviceState old_state, DeviceState new_state);
};


class TaskPriorityReset {
public:
    TaskPriorityReset(BaseType_t priority) {
        original_priority_ = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, priority);
    }
    ~TaskPriorityReset() {
        vTaskPrioritySet(NULL, original_priority_);
    }

private:
    BaseType_t original_priority_;
};

#endif // _APPLICATION_H_
