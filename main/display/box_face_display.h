#pragma once

#include "lcd_display.h"
#include "face/face_screen.h"
#include <memory>
#include <string>

// BOX-only adapter. Other boards retain their existing display implementations.
class BoxFaceDisplay final : public SpiLcdDisplay {
public:
    BoxFaceDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                   int width, int height, int offset_x, int offset_y,
                   bool mirror_x, bool mirror_y, bool swap_xy, DisplayFonts fonts);
    ~BoxFaceDisplay() override;

    void SetUiState(DisplayUiState state) override;
    void SetUpgradeProgress(int progress) override;
    void SetStatus(const char* status) override;
    void SetChatMessage(const char* role, const char* content) override;
    void SetEmotion(const char* emotion) override;
    void SetIcon(const char* icon) override;
    void SetPreviewImage(const lv_img_dsc_t* image) override;
    void ShowNotification(const char* notification, int duration_ms = 3000) override;
    void ShowNotification(const std::string& notification, int duration_ms = 3000) override;
    void SetTheme(const std::string& theme_name) override;
    std::string GetTheme() override;
    void SetPowerSaveMode(bool on) override;
    void UpdateStatusBar(bool update_all = false) override;

private:
    std::unique_ptr<face_ui::FaceScreen> face_screen_;
    DisplayUiState ui_state_ = DisplayUiState::Connecting;
    std::string message_;
    std::string status_;
    bool system_message_ = false;
    bool saw_wifi_connection_ = false;
    bool wifi_lost_ = false;

    // Caller holds the LVGL port lock; timer drawing runs on the LVGL task.
    void ApplyStateLocked(DisplayUiState state);
    void ApplyMessageLocked();
    void ClearMessageLocked();
};
