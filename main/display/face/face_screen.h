#pragma once

#include "large_face.h"

#include <cstdint>
#include <memory>
#include <string>

namespace face_ui {

// A complete 320x240 face screen. Callers hold the LVGL lock (or run on its
// thread) for construction, methods and destruction. Text is never logged.
class FaceScreen {
public:
    FaceScreen(lv_obj_t *parent, const lv_font_t *font, uint32_t frame_period_ms = 33);
    ~FaceScreen();
    FaceScreen(const FaceScreen &) = delete;
    FaceScreen &operator=(const FaceScreen &) = delete;

    LargeFace &Face();
    void SetState(FaceState state);
    void SetUpgradeProgress(int progress);
    int UpgradeProgress() const;
    const char *Caption() const;
    void SetMessage(const std::string &message, bool important = false);
    void ClearMessage();
    void ShowNotification(const std::string &message, uint32_t duration_ms = 3000);
    void SetVisible(bool visible);
    lv_obj_t *Root() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace face_ui
