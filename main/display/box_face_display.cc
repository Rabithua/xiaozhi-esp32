#include "box_face_display.h"
#include "application.h"
#include "board.h"
#include "boards/atk-dnesp32s3-box/box_lcd_scan.h"

#include <esp_cache.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_lcd_io_i80.h>
#include <esp_lcd_panel_commands.h>
#include <esp_lvgl_port.h>
#include <freertos/FreeRTOS.h>
#include <font_awesome.h>
#include <algorithm>
#include <cstring>
#include <exception>
#include <new>

namespace {
constexpr const char* TAG = "BoxFaceDisplay";
constexpr uint32_t kFrameWidth = 320;
constexpr uint32_t kFrameHeight = 240;
constexpr uint32_t kFramePixels = kFrameWidth * kFrameHeight;
constexpr uint32_t kFrameBytes = kFramePixels * sizeof(uint16_t);
constexpr uint32_t kNativeWidth = 240;
constexpr uint32_t kNativeHeight = 320;
constexpr uint32_t kFirstScanLine = 96;
constexpr uint32_t kLastScanLine = 112;
constexpr uint32_t kScanTimeoutUs = 40000;
constexpr uint32_t kMaxPhaseDelayUs = 250;

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

// Both the ISR's state and its spinlock live in internal RAM, independently of
// the UI object and the pixel buffers, which may be allocated in PSRAM.
struct BoxFaceDisplay::RefreshContext {
    portMUX_TYPE mutex = portMUX_INITIALIZER_UNLOCKED;
    lv_display_t* display = nullptr;
    DisplayRefreshStats stats;
    uint32_t started_us = 0;
    bool in_flight = false;
};

BoxFaceDisplay::BoxFaceDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                             int width, int height, int offset_x, int offset_y,
                             bool mirror_x, bool mirror_y, bool swap_xy, DisplayFonts fonts)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y,
                    mirror_x, mirror_y, swap_xy, fonts) {
    if (!display_ || width != 320 || height != 240) return;
    DisplayLockGuard lock(this);
    InitializeFrameRefreshLocked();
    InitializeScanRefreshLocked();
    try {
        // Keep the established LCD driver, with a separate completed-frame upload.
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
    if (display_) {
        // I80 parameter writes synchronously drain pending color transactions.
        // Keep the display, ISR context and frame buffer alive until that point.
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(panel_io_, LCD_CMD_NOP, nullptr, 0));
        const esp_lcd_panel_io_callbacks_t callbacks = {};
        ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(panel_io_, &callbacks, nullptr));
    }
    face_screen_.reset();
    if (display_) {
        lv_display_set_flush_cb(display_, nullptr);
        lv_display_set_user_data(display_, nullptr);
        // This also releases the port-owned fallback buffers and driver data.
        ESP_ERROR_CHECK(lvgl_port_remove_disp(display_));
        display_ = nullptr;
    }
    heap_caps_free(full_frame_buffer_);
    full_frame_buffer_ = nullptr;
    heap_caps_free(native_frame_buffer_);
    native_frame_buffer_ = nullptr;
    if (refresh_context_) {
        refresh_context_->~RefreshContext();
        heap_caps_free(refresh_context_);
        refresh_context_ = nullptr;
    }
    // The legacy derived/base destructors have overlapping borrowed UI pointers.
    content_ = status_bar_ = side_bar_ = container_ = preview_image_ = nullptr;
    emotion_label_ = network_label_ = status_label_ = notification_label_ = nullptr;
    mute_label_ = battery_label_ = chat_message_label_ = nullptr;
    low_battery_popup_ = low_battery_label_ = nullptr;
}

void BoxFaceDisplay::InitializeFrameRefreshLocked() {
    // The I80 allocator supplies the DMA/cache alignment required by this bus;
    // a generic LVGL allocation with its default 4-byte alignment is insufficient.
    auto* buffer = static_cast<uint8_t*>(
        esp_lcd_i80_alloc_draw_buffer(panel_io_, kFrameBytes, MALLOC_CAP_SPIRAM));
    auto* context_memory = heap_caps_malloc(sizeof(RefreshContext), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buffer || !context_memory) {
        heap_caps_free(buffer);
        heap_caps_free(context_memory);
        ESP_LOGW(TAG, "Full-frame allocation unavailable; retaining partial LCD refresh");
        return;
    }
    auto* context = new (context_memory) RefreshContext;
    context->display = display_;
    context->stats.available = true;
    context->stats.full_frame = true;
    context->stats.buffer_bytes = kFrameBytes;

    // The base class may already have submitted an initial partial frame. Drain
    // it while the port's original completion callback and buffers are intact.
    esp_err_t error = esp_lcd_panel_io_tx_param(panel_io_, LCD_CMD_NOP, nullptr, 0);
    if (error == ESP_OK) {
        const esp_lcd_panel_io_callbacks_t callbacks = {
            .on_color_trans_done = ColorTransferDone,
        };
        error = esp_lcd_panel_io_register_event_callbacks(panel_io_, &callbacks, context);
    }
    if (error != ESP_OK) {
        context->~RefreshContext();
        heap_caps_free(context);
        heap_caps_free(buffer);
        ESP_LOGW(TAG, "Full-frame setup failed (%s); retaining partial LCD refresh", esp_err_to_name(error));
        return;
    }

    full_frame_buffer_ = buffer;
    refresh_context_ = context;
    // The port owns driver_data; user_data belongs to this BOX-only flush path.
    lv_display_set_user_data(display_, this);
    lv_display_set_flush_cb(display_, FlushFrame);
    lv_display_set_buffers(display_, buffer, nullptr, kFrameBytes, LV_DISPLAY_RENDER_MODE_FULL);
    lv_obj_invalidate(lv_display_get_screen_active(display_));
    ESP_LOGI(TAG, "Full-frame I80 refresh: %lu PSRAM bytes, one DMA transfer per frame; no TE sync",
             static_cast<unsigned long>(kFrameBytes));
}

esp_err_t BoxFaceDisplay::SetNativeOrientationLocked() {
    // Update the ST7789 driver's cached MADCTL as well as the hardware. The
    // port retains its original landscape configuration; reapply native order
    // before each upload in case a port size/rotation event reapplied it.
    esp_err_t error = esp_lcd_panel_swap_xy(panel_, false);
    if (error == ESP_OK) error = esp_lcd_panel_mirror(panel_, false, false);
    return error;
}

void BoxFaceDisplay::InitializeScanRefreshLocked() {
    if (!refresh_context_) return;
    const auto probe = box_lcd::GetScanProbeResult();
    // Restrict this experimental gate to the native scan that was measured on
    // this BOX. A different panel/porch/timing retains the previous refresh path.
    if (!probe.registers_match || !probe.scan_valid || !probe.pins_restored ||
        probe.frame_period_us < 16000 || probe.frame_period_us > 20000 ||
        probe.max_line < 330 || probe.max_line > 400) return;
    auto* buffer = static_cast<uint8_t*>(
        esp_lcd_i80_alloc_draw_buffer(panel_io_, kFrameBytes, MALLOC_CAP_SPIRAM));
    if (!buffer) {
        ESP_LOGW(TAG, "Native scan buffer unavailable; retaining landscape refresh");
        return;
    }
    esp_err_t error = esp_lcd_panel_io_tx_param(panel_io_, LCD_CMD_NOP, nullptr, 0);
    if (error == ESP_OK) error = SetNativeOrientationLocked();
    if (error != ESP_OK) {
        heap_caps_free(buffer);
        // Restore both the cached and hardware landscape direction before the
        // ordinary full-frame callback can send another frame.
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_, true));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_, true, false));
        ESP_LOGW(TAG, "Native scan setup failed (%s); retaining landscape refresh", esp_err_to_name(error));
        return;
    }
    native_frame_buffer_ = buffer;
    portENTER_CRITICAL(&refresh_context_->mutex);
    refresh_context_->stats.scan_synchronized = true;
    refresh_context_->stats.native_order = true;
    portEXIT_CRITICAL(&refresh_context_->mutex);
    ESP_LOGI(TAG, "Native 240x320 scan gate enabled at lines %lu..%lu; boot period %luus; bus remains 10MHz",
             static_cast<unsigned long>(kFirstScanLine), static_cast<unsigned long>(kLastScanLine),
             static_cast<unsigned long>(probe.frame_period_us));
}

void BoxFaceDisplay::FlushFrame(lv_display_t* display, const lv_area_t* area, uint8_t* pixels) {
    auto* self = static_cast<BoxFaceDisplay*>(lv_display_get_user_data(display));
    if (!self || !self->refresh_context_) {
        lv_display_flush_ready(display);
        return;
    }
    auto* context = self->refresh_context_;
    if (!area || area->x1 != 0 || area->y1 != 0 ||
        area->x2 != static_cast<int32_t>(kFrameWidth - 1) ||
        area->y2 != static_cast<int32_t>(kFrameHeight - 1) || pixels != self->full_frame_buffer_) {
        portENTER_CRITICAL(&context->mutex);
        ++context->stats.transfer_errors;
        portEXIT_CRITICAL(&context->mutex);
        ESP_LOGE(TAG, "Rejected non-full-frame LCD transfer");
        lv_display_flush_ready(display);
        return;
    }

    esp_err_t error = ESP_OK;
    if (self->native_frame_buffer_) {
        // Keep the logical 320x240 FULL buffer untouched. Rotate its completed
        // landscape snapshot into the panel's native scan order, preserving the
        // old MADCTL=0x60 appearance while writing with MADCTL=0x00.
        const auto* source = reinterpret_cast<const uint16_t*>(pixels);
        auto* destination = reinterpret_cast<uint16_t*>(self->native_frame_buffer_);
        for (uint32_t v = 0; v < kNativeHeight; ++v) {
            for (uint32_t u = 0; u < kNativeWidth; ++u) {
                const uint16_t value = source[(kNativeWidth - 1 - u) * kFrameWidth + v];
#if defined(LV_COLOR_16_SWAP) && LV_COLOR_16_SWAP
                destination[v * kNativeWidth + u] = value;
#else
                destination[v * kNativeWidth + u] = __builtin_bswap16(value);
#endif
            }
        }
        // Finish expensive preparation before measuring the scan phase. IDF's
        // tx_color also checks/flushes cache, but this makes its data clean first.
        error = esp_cache_msync(self->native_frame_buffer_, kFrameBytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        if (error == ESP_OK) error = self->SetNativeOrientationLocked();
        const uint8_t columns[] = {0, 0, 0, static_cast<uint8_t>(kNativeWidth - 1)};
        const uint8_t rows[] = {0, 0, static_cast<uint8_t>((kNativeHeight - 1) >> 8),
                              static_cast<uint8_t>(kNativeHeight - 1)};
        if (error == ESP_OK)
            error = esp_lcd_panel_io_tx_param(self->panel_io_, LCD_CMD_CASET, columns, sizeof(columns));
        if (error == ESP_OK)
            error = esp_lcd_panel_io_tx_param(self->panel_io_, LCD_CMD_RASET, rows, sizeof(rows));
        if (error == ESP_OK) {
            // Reading GSCAN does not change CASET/RASET. A missed or stale gate
            // drops this frame; it must never fall through to an unsynced write.
            const auto gate = box_lcd::WaitForScanWindow(
                self->panel_io_, kFirstScanLine, kLastScanLine, kScanTimeoutUs);
            portENTER_CRITICAL(&context->mutex);
            context->stats.last_scan_line = gate.line;
            context->stats.last_wait_us = gate.wait_us;
            context->stats.last_phase_delay_us = 0;
            if (!gate.ready || !gate.pins_restored) ++context->stats.sync_timeouts;
            if (!gate.pins_restored) ++context->stats.transfer_errors;
            portEXIT_CRITICAL(&context->mutex);
            if (!gate.ready || !gate.pins_restored) {
                lv_display_flush_ready(display);
                return;
            }
            // Keep the freshness check next to submission. An ISR can still
            // preempt after this check; this bound is a practical guard, not a
            // claim of hard real-time exclusion of every possible interruption.
            const uint32_t submit_us = static_cast<uint32_t>(esp_timer_get_time());
            const uint32_t phase_delay_us = submit_us - gate.sampled_us;
            portENTER_CRITICAL(&context->mutex);
            context->stats.last_phase_delay_us = phase_delay_us;
            if (phase_delay_us > kMaxPhaseDelayUs) {
                ++context->stats.late_gate_skips;
            } else {
                context->started_us = submit_us;
                context->in_flight = true;
            }
            portEXIT_CRITICAL(&context->mutex);
            if (phase_delay_us > kMaxPhaseDelayUs) {
                lv_display_flush_ready(display);
                return;
            }
            error = esp_lcd_panel_io_tx_color(self->panel_io_, LCD_CMD_RAMWR,
                                             self->native_frame_buffer_, kFrameBytes);
        }
    } else {
        // Fallback retains the old landscape transfer and exactly one byte swap.
#if !defined(LV_COLOR_16_SWAP) || !LV_COLOR_16_SWAP
        lv_draw_sw_rgb565_swap(pixels, kFramePixels);
#endif
        portENTER_CRITICAL(&context->mutex);
        context->started_us = static_cast<uint32_t>(esp_timer_get_time());
        context->in_flight = true;
        portEXIT_CRITICAL(&context->mutex);
        error = esp_lcd_panel_draw_bitmap(self->panel_, 0, 0, kFrameWidth, kFrameHeight, pixels);
    }
    if (error != ESP_OK) {
        portENTER_CRITICAL(&context->mutex);
        context->in_flight = false;
        ++context->stats.transfer_errors;
        portEXIT_CRITICAL(&context->mutex);
        ESP_LOGE(TAG, "Full-frame LCD transfer failed: %s", esp_err_to_name(error));
        // No completion ISR arrives when submission fails. Release the buffer so
        // LVGL can redraw and retry on the next animation tick instead of hanging.
        lv_display_flush_ready(display);
    }
}

bool BoxFaceDisplay::ColorTransferDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void* user_context) {
    auto* context = static_cast<RefreshContext*>(user_context);
    const uint32_t now_us = static_cast<uint32_t>(esp_timer_get_time());
    portENTER_CRITICAL_ISR(&context->mutex);
    if (context->in_flight) {
        // Unsigned subtraction remains correct across the 32-bit microsecond
        // clock wrap. Measure submission through the hardware completion ISR;
        // native packing, initial cache writeback and gate waiting happen first.
        const uint32_t elapsed_us = now_us - context->started_us;
        context->stats.last_dma_us = elapsed_us;
        context->stats.max_dma_us = std::max(context->stats.max_dma_us, elapsed_us);
        ++context->stats.completed_frames;
        context->in_flight = false;
    }
    portEXIT_CRITICAL_ISR(&context->mutex);
    lv_display_flush_ready(context->display);
    return false;
}

DisplayRefreshStats BoxFaceDisplay::GetRefreshStats() {
    DisplayLockGuard lock(this);
    if (!refresh_context_) return {};
    portENTER_CRITICAL(&refresh_context_->mutex);
    const DisplayRefreshStats stats = refresh_context_->stats;
    portEXIT_CRITICAL(&refresh_context_->mutex);
    return stats;
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
