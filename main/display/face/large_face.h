#pragma once

#include "lvgl.h"
#include <cstdint>
#include <memory>

namespace face_ui {

enum class FaceState {
    Idle, Listening, Thinking, Speaking, WifiConfiguring, Connecting, Offline,
    Upgrading, WaitingActivation
};

// Shared procedural face UI. Owns a 320x210 canvas; never accesses audio or network.
// Callers must hold the LVGL lock (or run on its thread) for construction, methods
// and destruction. On ESP32, the two large pixel buffers live in PSRAM.
class LargeFace {
public:
    explicit LargeFace(lv_obj_t *parent, uint32_t frame_period_ms = 33);
    ~LargeFace();
    LargeFace(const LargeFace &) = delete;
    LargeFace &operator=(const LargeFace &) = delete;

    void SetState(FaceState state);
    FaceState State() const;
    void SetPlaying(bool playing);
    bool Playing() const;

    // Freeze the timeline and draw the requested state's settled pose at t.
    // Does not change Playing(); SetPlaying(true) explicitly resumes the clock.
    void SetTimeForPreview(double seconds);
    // Step a deterministic timeline while retaining the current state morph.
    void AdvanceForPreview(double seconds);
    void SetVisible(bool visible);
    // Scale the canvas to leave room for provisioning and activation details.
    // Layout changes do not advance or reset the animation timeline.
    void SetCompact(bool compact);
    uint64_t FrameCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace face_ui
