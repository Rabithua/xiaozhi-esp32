#include "box_face_display.h"
#include "application.h"
#include "board.h"

#include <esp_log.h>
#include <font_awesome.h>
#include <algorithm>
#include <cstring>
#include <exception>

namespace {
constexpr const char* TAG = "BoxFaceDisplay";

bool Important(DisplayUiState state) {
    return state == DisplayUiState::WifiConfiguring ||
           state == DisplayUiState::WaitingActivation || state == DisplayUiState::Offline;
}

face_ui::FaceState FaceStateFor(DisplayUiState state) {
    switch (state) {
        case DisplayUiState::Idle: return face_ui::FaceState::Idle;
        case DisplayUiState::Listening: return face_ui::FaceState::Listening;
        case DisplayUiState::Thinking: return face_ui::FaceState::Thinking;
        case DisplayUiState::Speaking: return face_ui::FaceState::Speaking;
        case DisplayUiState::WifiConfiguring: return face_ui::FaceState::WifiConfiguring;
        case DisplayUiState::Connecting: return face_ui::FaceState::Connecting;
        case DisplayUiState::Offline: return face_ui::FaceState::Offline;
        case DisplayUiState::Upgrading: return face_ui::FaceState::Upgrading;
        case DisplayUiState::WaitingActivation: return face_ui::FaceState::WaitingActivation;
    }
    return face_ui::FaceState::Idle;
}

DisplayUiState CurrentActivity() {
    switch (Application::GetInstance().GetDeviceState()) {
        case kDeviceStateIdle: return DisplayUiState::Idle;
        case kDeviceStateListening: return DisplayUiState::Listening;
        case kDeviceStateSpeaking: return DisplayUiState::Speaking;
        case kDeviceStateWifiConfiguring: return DisplayUiState::WifiConfiguring;
        case kDeviceStateUpgrading: return DisplayUiState::Upgrading;
        case kDeviceStateAudioTesting: return DisplayUiState::Listening;
        case kDeviceStateFatalError: return DisplayUiState::Offline;
        default: return DisplayUiState::Connecting;
    }
}
} // namespace

BoxFaceDisplay::BoxFaceDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                             int width, int height, int offset_x, int offset_y,
                             bool mirror_x, bool mirror_y, bool swap_xy, DisplayFonts fonts)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y,
                    mirror_x, mirror_y, swap_xy, fonts) {
    if (!display_ || width != 320 || height != 240) return;
    DisplayLockGuard lock(this);
    try {
        // Keep the established LCD driver and DMA buffers; only add presentation.
        // The renderer's two large buffers are allocated exclusively in PSRAM.
        face_screen_ = std::make_unique<face_ui::FaceScreen>(
            lv_display_get_screen_active(display_), fonts.text_font, 50);
        face_screen_->SetState(face_ui::FaceState::Connecting);
        ESP_LOGI(TAG, "Black/white face UI ready; PSRAM canvas; 50ms animation budget");
    } catch (const std::exception&) {
        // Allocation failure must leave the original readable UI available.
        ESP_LOGE(TAG, "Face UI unavailable; keeping the original LCD interface");
    }
}

BoxFaceDisplay::~BoxFaceDisplay() {
    if (notification_timer_) esp_timer_stop(notification_timer_);
    DisplayLockGuard lock(this);
    face_screen_.reset();
    if (display_) lv_obj_clean(lv_display_get_screen_active(display_));
    // The legacy derived/base destructors have overlapping borrowed UI pointers.
    content_ = status_bar_ = side_bar_ = container_ = preview_image_ = nullptr;
    emotion_label_ = network_label_ = status_label_ = notification_label_ = nullptr;
    mute_label_ = battery_label_ = chat_message_label_ = nullptr;
    low_battery_popup_ = low_battery_label_ = nullptr;
}

void BoxFaceDisplay::ClearMessageLocked() {
    message_.clear();
    system_message_ = false;
    face_screen_->ClearMessage();
}

void BoxFaceDisplay::ApplyMessageLocked() {
    face_screen_->SetMessage(message_, system_message_ && Important(ui_state_));
}

void BoxFaceDisplay::ApplyStateLocked(DisplayUiState state) {
    const bool changed = state != ui_state_;
    if (changed) {
        face_screen_->ShowNotification("", 0);
        // Keep the just-received STT/assistant text across its state transition.
        // Critical system information only survives while its state is relevant.
        if ((system_message_ && !Important(state)) ||
            (!system_message_ && (Important(state) || state == DisplayUiState::Upgrading))) {
            ClearMessageLocked();
        }
    }
    ui_state_ = state;
    face_screen_->SetState(FaceStateFor(state));
    if (changed && state == DisplayUiState::Upgrading) face_screen_->SetUpgradeProgress(0);
    ApplyMessageLocked();
}

void BoxFaceDisplay::SetUiState(DisplayUiState state) {
    DisplayLockGuard lock(this);
    if (!face_screen_) {
        ui_state_ = state;
        return;
    }
    ApplyStateLocked(state);
}

void BoxFaceDisplay::SetUpgradeProgress(int progress) {
    DisplayLockGuard lock(this);
    if (!face_screen_) {
        if (ui_state_ != DisplayUiState::Upgrading) return;
        const std::string text = std::to_string(std::clamp(progress, 0, 100)) + "%";
        SpiLcdDisplay::SetChatMessage("system", text.c_str());
        return;
    }
    // A queued progress callback must not resurrect an upgrade after failure.
    if (ui_state_ == DisplayUiState::Upgrading) face_screen_->SetUpgradeProgress(progress);
}

void BoxFaceDisplay::SetStatus(const char* status) {
    if (!face_screen_) { SpiLcdDisplay::SetStatus(status); return; }
    DisplayLockGuard lock(this);
    const std::string next = status ? status : "";
    if (next == status_) return;
    status_ = next;
    // A clock update must not overwrite an explicit offline/activation state.
    if (ui_state_ == DisplayUiState::Connecting && !next.empty())
        face_screen_->ShowNotification(next, 1500);
}

void BoxFaceDisplay::SetChatMessage(const char* role, const char* content) {
    if (!face_screen_) { SpiLcdDisplay::SetChatMessage(role, content); return; }
    DisplayLockGuard lock(this);
    const bool system = role && std::strcmp(role, "system") == 0;
    const std::string text = content ? content : "";
    if (ui_state_ == DisplayUiState::Upgrading) return; // Real progress has its own channel.
    if (text.empty()) {
        // Audio-channel teardown clears ordinary subtitles, not recovery details.
        if (!Important(ui_state_)) ClearMessageLocked();
        return;
    }
    if (!system && Important(ui_state_)) return;
    message_ = text;
    system_message_ = system;
    ApplyMessageLocked();
}

void BoxFaceDisplay::SetEmotion(const char* emotion) {
    if (!face_screen_) SpiLcdDisplay::SetEmotion(emotion);
    // LLM moods and "neutral" do not replace an event-driven UI state.
}

void BoxFaceDisplay::SetIcon(const char* icon) {
    if (!face_screen_) SpiLcdDisplay::SetIcon(icon);
}

void BoxFaceDisplay::SetPreviewImage(const lv_img_dsc_t* image) {
    if (!face_screen_) { SpiLcdDisplay::SetPreviewImage(image); return; }
    // Image previews retain the existing renderer instead of being hidden by a face.
    {
        DisplayLockGuard lock(this);
        face_screen_->SetVisible(image == nullptr);
    }
    SpiLcdDisplay::SetPreviewImage(image);
}

void BoxFaceDisplay::ShowNotification(const char* notification, int duration_ms) {
    if (!face_screen_) { SpiLcdDisplay::ShowNotification(notification, duration_ms); return; }
    DisplayLockGuard lock(this);
    if (Important(ui_state_) && system_message_ && !message_.empty()) return;
    face_screen_->ShowNotification(notification ? notification : "", std::max(0, duration_ms));
}

void BoxFaceDisplay::ShowNotification(const std::string& notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

void BoxFaceDisplay::SetTheme(const std::string& theme_name) {
    if (!face_screen_) SpiLcdDisplay::SetTheme(theme_name);
}

std::string BoxFaceDisplay::GetTheme() {
    return face_screen_ ? "dark" : SpiLcdDisplay::GetTheme();
}

void BoxFaceDisplay::SetPowerSaveMode(bool on) {
    if (!face_screen_) { SpiLcdDisplay::SetPowerSaveMode(on); return; }
    DisplayLockGuard lock(this);
    face_screen_->Face().SetPlaying(!on);
}

void BoxFaceDisplay::UpdateStatusBar(bool update_all) {
    if (!face_screen_) { SpiLcdDisplay::UpdateStatusBar(update_all); return; }
    const char* icon = Board::GetInstance().GetNetworkStateIcon();
    if (!icon) return;
    const bool connected = std::strcmp(icon, FONT_AWESOME_WIFI_SLASH) != 0;
    DisplayLockGuard lock(this);
    if (connected) {
        saw_wifi_connection_ = true;
        if (wifi_lost_) {
            wifi_lost_ = false;
            if (ui_state_ == DisplayUiState::Offline || ui_state_ == DisplayUiState::Connecting)
                ApplyStateLocked(CurrentActivity());
        }
    } else if (saw_wifi_connection_ && !wifi_lost_) {
        wifi_lost_ = true;
        if (ui_state_ != DisplayUiState::WifiConfiguring && ui_state_ != DisplayUiState::WaitingActivation &&
            ui_state_ != DisplayUiState::Upgrading) {
            ApplyStateLocked(DisplayUiState::Offline);
            message_ = "网络已断开，正在重新连接";
            system_message_ = true;
            ApplyMessageLocked();
        }
    }
}
