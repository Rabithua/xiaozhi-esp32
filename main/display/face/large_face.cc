#include "large_face.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <vector>

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#include <limits>
#include <new>
#include <type_traits>
#endif

namespace face_ui {
namespace {

#ifdef ESP_PLATFORM
// Drawing surfaces must not consume the internal heap needed by I2S, DMA and
// voice processing. Failure is explicit so the display adapter can keep its
// existing UI rather than retrying these large allocations in internal RAM.
template <typename T>
struct PsramAllocator {
    using value_type = T;
    using is_always_equal = std::true_type;

    PsramAllocator() noexcept = default;
    template <typename U>
    PsramAllocator(const PsramAllocator<U> &) noexcept {}

    T *allocate(std::size_t count) {
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T))
            throw std::bad_alloc();
        auto *memory = heap_caps_malloc(count * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!memory) throw std::bad_alloc();
        return static_cast<T *>(memory);
    }

    void deallocate(T *memory, std::size_t) noexcept { heap_caps_free(memory); }
};

template <typename T, typename U>
bool operator==(const PsramAllocator<T> &, const PsramAllocator<U> &) noexcept { return true; }
template <typename T, typename U>
bool operator!=(const PsramAllocator<T> &, const PsramAllocator<U> &) noexcept { return false; }

template <typename T>
using FaceBuffer = std::vector<T, PsramAllocator<T>>;
#else
template <typename T>
using FaceBuffer = std::vector<T>;
#endif

constexpr int kWidth = 320;
constexpr int kHeight = 210;
constexpr int kSamples = 64;
constexpr float kPi = 3.14159265358979323846f;
constexpr float kTau = 2.0f * kPi;
constexpr float kCentreX = 160.0f;
constexpr float kCentreY = 104.0f;

float Mix(float a, float b, float amount) { return a + (b - a) * amount; }
float Clamp(float value) { return std::clamp(value, 0.0f, 1.0f); }

struct Point { float x = 0, y = 0; };
struct Eye {
    float x = 0, y = 0, width = 12, height = 28, tilt = -0.42f, alpha = 1;
};
struct Dot { float x = kCentreX, y = kCentreY, radius = 0; };
struct Pose {
    std::array<Point, kSamples> contour;
    std::array<Eye, 2> eyes;
    std::array<Dot, 2> side_dots;
    std::array<Dot, 3> connection_dots = {{{263, 61, 0}, {263, 61, 0}, {263, 61, 0}}};
    float wifi_strength = 0;
    float offline_strength = 0;
    float upgrade_strength = 0;
    float activation_strength = 0;
};

// The pure pose function makes screenshots and exported motion reproducible.
// These parameters were authored for the local face preview; no web code/assets
// are embedded or interpreted by the component.
Pose Sample(FaceState state, double seconds) {
    const float t = static_cast<float>(seconds);
    Pose pose;
    float radius = 75.5f;
    float sx = 1, sy = 1;
    float cx = kCentreX, cy = kCentreY;
    float gaze_x = 18, gaze_y = -25;
    float eye_width = 11.5f, eye_height = 28, eye_tilt = -0.42f;
    float separation = 29;
    float eye_row_offset = 4.5f;
    float shape_motion = 0.008f;
    float eye_alpha = 1;
    float pulse = 0;
    float head_tilt = 0;

    switch (state) {
        case FaceState::Idle: {
            const float breath = std::sin(t * 1.6f);
            sx = 1 + 0.010f * breath;
            sy = 1 - 0.009f * breath;
            gaze_x += 2.2f * std::sin(t * 0.73f);
            gaze_y += 1.4f * std::sin(t * 0.53f + 0.4f);
            break;
        }
        case FaceState::Listening: {
            const float breath = std::sin(t * 2.1f);
            radius = 76.2f;
            sx = 0.965f + 0.005f * breath;
            sy = 1.045f - 0.005f * breath;
            gaze_x = 6.0f + 0.7f * std::sin(t * 0.8f);
            gaze_y = -18.0f + 0.4f * std::sin(t * 0.65f);
            eye_width = 13.5f;
            eye_height = 31;
            eye_tilt = -0.13f;
            separation = 32;
            shape_motion = 0.004f;
            break;
        }
        case FaceState::Thinking: {
            // The main silhouette becomes the middle dot. The two side dots
            // start inside the face on entry and emerge as its body contracts.
            const float phase = t * kTau / 1.55f;
            const float middle = 0.5f + 0.5f * std::sin(phase - 0.9f);
            radius = 11.5f + 4.0f * middle;
            cy -= 3.4f * middle;
            shape_motion = 0;
            eye_alpha = 0;
            for (int i = 0; i < 2; ++i) {
                const int sequence_index = i == 0 ? 0 : 2;
                const float wave = 0.5f + 0.5f * std::sin(phase - sequence_index * 0.9f);
                pose.side_dots[i] = {
                    kCentreX + (i == 0 ? -49.0f : 49.0f),
                    kCentreY - 3.4f * wave,
                    11.5f + 4.0f * wave
                };
            }
            break;
        }
        case FaceState::Speaking: {
            // A quiet syllabic rhythm gives the whole face a voice without
            // introducing a mouth or a fake microphone-level display.
            pulse = (std::sin(t * kTau * 2.35f) +
                     0.45f * std::sin(t * kTau * 3.7f + 0.8f)) / 1.45f;
            sx = 1.018f + 0.026f * pulse;
            sy = 0.985f - 0.035f * pulse;
            gaze_x = 13 + 1.8f * std::sin(t * 0.85f);
            gaze_y = -23;
            eye_width = 12;
            eye_height = 23 + 3.5f * pulse;
            eye_tilt = -0.34f + 0.035f * pulse;
            shape_motion = 0.022f;
            break;
        }
        case FaceState::WifiConfiguring: {
            // Wait for the person to act: slow searching eyes and a small,
            // persistent Wi-Fi mark rather than a busy loading animation.
            radius = 75;
            sx = 0.975f + 0.005f * std::sin(t * 1.4f);
            sy = 1.025f - 0.005f * std::sin(t * 1.4f);
            head_tilt = 0.085f;
            gaze_x = 7 + 12 * std::sin(t * 0.63f);
            gaze_y = -22 + std::sin(t * 0.45f);
            eye_width = 12.5f;
            eye_height = 26;
            eye_tilt = -0.28f;
            separation = 30;
            shape_motion = 0.009f;
            pose.wifi_strength = 0.86f + 0.14f * std::sin(t * 1.5f);
            break;
        }
        case FaceState::Connecting: {
            // An attentive, forward gaze while a small activity mark does the
            // waiting. This is a preview rhythm, not measured network progress.
            const float breath = std::sin(t * 2.0f);
            radius = 75.5f;
            sx = 0.985f + 0.006f * breath;
            sy = 1.02f - 0.006f * breath;
            cy -= 1.5f;
            gaze_x = 0.7f * std::sin(t * 0.65f);
            gaze_y = -17;
            eye_width = 11.8f;
            eye_height = 27;
            eye_tilt = -0.045f;
            eye_row_offset = 0;
            separation = 31;
            shape_motion = 0.004f;
            for (int i = 0; i < 3; ++i) {
                const float angle = -kPi * 0.5f + t * kTau / 2.2f - i * kTau / 3;
                pose.connection_dots[i] = {
                    263 + std::cos(angle) * 11,
                    61 + std::sin(angle) * 11,
                    3.5f - i * 0.65f
                };
            }
            break;
        }
        case FaceState::Offline: {
            // A quiet, slightly lowered face, rather than an alarming flash.
            // The persistent slashed Wi-Fi mark also reads when motion pauses.
            const float breath = std::sin(t * 1.15f);
            radius = 75;
            sx = 1.025f + 0.003f * breath;
            sy = 0.95f - 0.003f * breath;
            cy += 4.5f;
            gaze_x = 0.6f * std::sin(t * 0.45f);
            gaze_y = -4;
            eye_width = 11.5f;
            eye_height = 19;
            eye_tilt = 0;
            eye_row_offset = 0;
            separation = 31;
            shape_motion = 0.004f;
            pose.wifi_strength = 1;
            pose.offline_strength = 1;
            break;
        }
        case FaceState::Upgrading: {
            // The progress bar and percentage belong to the surrounding UI.
            // Keep the face clear of y=191 and watch the progress quietly.
            const float breath = std::sin(t * 1.45f);
            radius = 70.5f;
            sx = 0.98f + 0.004f * breath;
            sy = 1.015f - 0.004f * breath;
            cy = 98;
            gaze_x = 0.5f * std::sin(t * 0.5f);
            gaze_y = -9;
            eye_width = 11.8f;
            eye_height = 24;
            eye_tilt = -0.025f;
            eye_row_offset = 0;
            separation = 30;
            shape_motion = 0.003f;
            pose.upgrade_strength = 1;
            break;
        }
        case FaceState::WaitingActivation: {
            // An inviting, slightly curious pose while the person completes
            // activation. The key is symbolic; no real code is fabricated.
            const float breath = std::sin(t * 1.35f);
            radius = 74.5f;
            sx = 0.985f + 0.005f * breath;
            sy = 1.015f - 0.005f * breath;
            head_tilt = -0.075f;
            gaze_x = 9 + 1.4f * std::sin(t * 0.6f);
            gaze_y = -19 + 0.5f * std::sin(t * 0.4f);
            eye_width = 12.5f;
            eye_height = 27;
            eye_tilt = -0.19f;
            eye_row_offset = 3;
            separation = 31;
            shape_motion = 0.005f;
            pose.activation_strength = 1;
            break;
        }
    }

    const float head_cosine = std::cos(head_tilt);
    const float head_sine = std::sin(head_tilt);
    for (int i = 0; i < kSamples; ++i) {
        const float angle = kTau * i / kSamples;
        const float soft_shape = 1 + shape_motion * std::sin(3 * angle + t * 0.65f) +
                                 shape_motion * 0.55f * std::cos(2 * angle - t * 0.3f);
        const float r = radius * soft_shape;
        const float local_x = std::cos(angle) * r * sx;
        const float local_y = std::sin(angle) * r * sy;
        pose.contour[i] = {cx + local_x * head_cosine - local_y * head_sine,
                           cy + local_x * head_sine + local_y * head_cosine};
    }

    // A deterministic, short blink rather than constant blinking. Listening
    // holds its gaze longer; the blink remains a small cue of life.
    const float blink_period = state == FaceState::Listening ? 6.2f : 4.7f;
    const float blink_phase = std::fmod(std::max(0.0f, t), blink_period);
    const float blink_centre = blink_period - 1.0f;
    const float blink_distance = (blink_phase - blink_centre) / 0.073f;
    const float lid = 1 - 0.94f * std::exp(-blink_distance * blink_distance);
    for (int i = 0; i < 2; ++i) {
        const float local_x = gaze_x + (i == 0 ? -0.5f : 0.5f) * separation;
        const float local_y = gaze_y - (i == 1 ? eye_row_offset : 0.0f);
        const float concerned_tilt = state == FaceState::Offline ? (i == 0 ? 0.42f : -0.42f) : 0;
        pose.eyes[i] = {
            cx + local_x * head_cosine - local_y * head_sine,
            cy + local_x * head_sine + local_y * head_cosine,
            eye_width,
            std::max(2.0f, eye_height * lid),
            eye_tilt + head_tilt + concerned_tilt,
            eye_alpha
        };
    }
    return pose;
}

Pose Blend(const Pose &from, const Pose &to, float amount) {
    Pose pose;
    pose.wifi_strength = Mix(from.wifi_strength, to.wifi_strength, amount);
    pose.offline_strength = Mix(from.offline_strength, to.offline_strength, amount);
    pose.upgrade_strength = Mix(from.upgrade_strength, to.upgrade_strength, amount);
    pose.activation_strength = Mix(from.activation_strength, to.activation_strength, amount);
    for (int i = 0; i < 3; ++i) {
        const auto &a = from.connection_dots[i];
        const auto &b = to.connection_dots[i];
        pose.connection_dots[i] = {Mix(a.x, b.x, amount), Mix(a.y, b.y, amount),
                                   Mix(a.radius, b.radius, amount)};
    }
    for (int i = 0; i < kSamples; ++i) {
        pose.contour[i] = {Mix(from.contour[i].x, to.contour[i].x, amount),
                           Mix(from.contour[i].y, to.contour[i].y, amount)};
    }
    for (int i = 0; i < 2; ++i) {
        const auto &a = from.eyes[i];
        const auto &b = to.eyes[i];
        pose.eyes[i] = {Mix(a.x, b.x, amount), Mix(a.y, b.y, amount),
                        Mix(a.width, b.width, amount), Mix(a.height, b.height, amount),
                        Mix(a.tilt, b.tilt, amount), Mix(a.alpha, b.alpha, amount)};
        const auto &da = from.side_dots[i];
        const auto &db = to.side_dots[i];
        pose.side_dots[i] = {Mix(da.x, db.x, amount), Mix(da.y, db.y, amount),
                             Mix(da.radius, db.radius, amount)};
    }
    return pose;
}

} // namespace

struct LargeFace::Impl {
    lv_obj_t *canvas = nullptr;
    lv_timer_t *timer = nullptr;
    // Canvas owns no buffer; these outlive its LVGL object.
    FaceBuffer<uint16_t> pixels = FaceBuffer<uint16_t>(kWidth * kHeight);
    FaceBuffer<uint8_t> grey = FaceBuffer<uint8_t>(kWidth * kHeight);
    FaceState state = FaceState::Idle;
    bool playing = true;
    bool visible = true;
    bool manual_time = false;
    bool compact = false;
#ifdef ESP_PLATFORM
    bool render_pending = false;
#endif
    uint32_t last_tick = 0;
    double time = 0;
    double state_start = 0;
    double transition_start = 0;
    double transition_duration = 0;
    Pose origin = Sample(FaceState::Idle, 0);
    uint64_t frame_count = 0;

    Impl(lv_obj_t *parent, uint32_t frame_period_ms) {
        canvas = lv_canvas_create(parent);
        if (!canvas) throw std::runtime_error("Could not create the face canvas");
        lv_obj_remove_style_all(canvas);
        lv_canvas_set_buffer(canvas, pixels.data(), kWidth, kHeight, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(canvas, 0, 0);
        lv_image_set_pivot(canvas, kWidth / 2, kHeight / 2);
        lv_obj_remove_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
        last_tick = lv_tick_get();
        Render();
        timer = lv_timer_create([](lv_timer_t *timer) {
            auto *self = static_cast<Impl *>(lv_timer_get_user_data(timer));
            self->Advance();
#ifdef ESP_PLATFORM
            // Setters can run on the smaller ESP event/app task stacks. Only
            // the LVGL task performs the stack-heavy raster pass after startup.
            if (self->visible && (self->render_pending || (self->playing && !self->manual_time))) {
                self->render_pending = false;
                self->Render();
            }
#else
            if (self->visible && self->playing && !self->manual_time) self->Render();
#endif
        }, std::max<uint32_t>(1, frame_period_ms), this);
        if (!timer) {
            lv_obj_delete(canvas);
            canvas = nullptr;
            throw std::runtime_error("Could not create the face animation timer");
        }
    }

    ~Impl() {
        if (timer) lv_timer_delete(timer);
        if (canvas) lv_obj_delete(canvas);
    }

    void Advance() {
        const uint32_t now = lv_tick_get();
        if (playing && visible && !manual_time) time += static_cast<uint32_t>(now - last_tick) / 1000.0;
        last_tick = now;
    }

    Pose CurrentPose() const {
        const Pose target = Sample(state, std::max(0.0, time - state_start));
        if (transition_duration <= 0) return target;
        const float progress = Clamp(static_cast<float>((time - transition_start) / transition_duration));
        if (progress >= 1) return target;
        const float inverse = 1 - progress;
        const float eased = 1 - inverse * inverse * inverse * inverse * inverse;
        return Blend(origin, target, eased);
    }

    // Four vertical samples, exact horizontal interval coverage. The 64-point
    // contour needs no full SVG parser and no per-pixel point-in-polygon loop.
    void FillContour(const std::array<Point, kSamples> &points) {
        float top = kHeight, bottom = 0;
        for (const auto &point : points) {
            top = std::min(top, point.y);
            bottom = std::max(bottom, point.y);
        }
        const int y0 = std::max(0, static_cast<int>(std::floor(top)));
        const int y1 = std::min(kHeight - 1, static_cast<int>(std::ceil(bottom)));
        std::array<float, kSamples> intersections;
        std::array<float, kWidth> coverage;
        for (int y = y0; y <= y1; ++y) {
            coverage.fill(0);
            for (int sub = 0; sub < 4; ++sub) {
                const float sample_y = y + (sub + 0.5f) / 4;
                int count = 0;
                for (int i = 0; i < kSamples; ++i) {
                    const auto &a = points[i];
                    const auto &b = points[(i + 1) % kSamples];
                    if ((a.y <= sample_y && b.y > sample_y) ||
                        (b.y <= sample_y && a.y > sample_y)) {
                        intersections[count++] = a.x + (sample_y - a.y) * (b.x - a.x) / (b.y - a.y);
                    }
                }
                std::sort(intersections.begin(), intersections.begin() + count);
                for (int i = 0; i + 1 < count; i += 2) {
                    const float left = intersections[i], right = intersections[i + 1];
                    const int x0 = std::max(0, static_cast<int>(std::floor(left)));
                    const int x1 = std::min(kWidth - 1, static_cast<int>(std::floor(right)));
                    for (int x = x0; x <= x1; ++x) {
                        coverage[x] += Clamp(std::min(right, x + 1.0f) - std::max(left, static_cast<float>(x))) * 0.25f;
                    }
                }
            }
            for (int x = 0; x < kWidth; ++x) {
                grey[y * kWidth + x] = static_cast<uint8_t>(std::round(Clamp(coverage[x]) * 255));
            }
        }
    }

    void FillDot(const Dot &dot) {
        if (dot.radius <= 0.01f) return;
        const int x0 = std::max(0, static_cast<int>(std::floor(dot.x - dot.radius - 1)));
        const int x1 = std::min(kWidth - 1, static_cast<int>(std::ceil(dot.x + dot.radius + 1)));
        const int y0 = std::max(0, static_cast<int>(std::floor(dot.y - dot.radius - 1)));
        const int y1 = std::min(kHeight - 1, static_cast<int>(std::ceil(dot.y + dot.radius + 1)));
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const float dx = x + 0.5f - dot.x, dy = y + 0.5f - dot.y;
                const float alpha = Clamp(dot.radius + 0.5f - std::sqrt(dx * dx + dy * dy));
                auto &value = grey[y * kWidth + x];
                value = std::max(value, static_cast<uint8_t>(std::round(alpha * 255)));
            }
        }
    }

    void CutEye(const Eye &eye) {
        if (eye.alpha <= 0.001f) return;
        const float radius = std::min(eye.width, eye.height) * 0.5f;
        const bool horizontal = eye.width > eye.height;
        const float half_segment = std::abs(eye.height - eye.width) * 0.5f;
        const float extent = std::max(eye.width, eye.height) * 0.5f + 1;
        const int x0 = std::max(0, static_cast<int>(std::floor(eye.x - extent)));
        const int x1 = std::min(kWidth - 1, static_cast<int>(std::ceil(eye.x + extent)));
        const int y0 = std::max(0, static_cast<int>(std::floor(eye.y - extent)));
        const int y1 = std::min(kHeight - 1, static_cast<int>(std::ceil(eye.y + extent)));
        const float cosine = std::cos(eye.tilt), sine = std::sin(eye.tilt);
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const float dx = x + 0.5f - eye.x, dy = y + 0.5f - eye.y;
                const float local_x = cosine * dx + sine * dy;
                const float local_y = -sine * dx + cosine * dy;
                const float a = horizontal ? std::max(0.0f, std::abs(local_x) - half_segment) : local_x;
                const float b = horizontal ? local_y : std::max(0.0f, std::abs(local_y) - half_segment);
                const float alpha = Clamp(radius + 0.5f - std::sqrt(a * a + b * b)) * eye.alpha;
                auto &value = grey[y * kWidth + x];
                // Subtracting the eyes from the body also clips them to its
                // outline: outside a white shape the background remains black.
                value = static_cast<uint8_t>(std::round(value * (1 - alpha)));
            }
        }
    }

    void FillWifi(float strength) {
        if (strength <= 0.001f) return;
        constexpr float anchor_x = 263;
        constexpr float anchor_y = 73;
        constexpr float cosine_45 = 0.70710678118f;
        constexpr std::array<float, 3> radii = {10, 18, 26};
        // All three arcs span the upper 90 degrees and have round ends. The
        // glyph occupies about x=243..283, y=45..76, clear of the face.
        for (int y = 44; y <= 77; ++y) {
            for (int x = 242; x <= 284; ++x) {
                const float dx = x + 0.5f - anchor_x;
                const float dy = y + 0.5f - anchor_y;
                const float length = std::sqrt(dx * dx + dy * dy);
                float distance = std::max(0.0f, length - 2.2f);
                float coverage = Clamp(0.5f - (length - 2.2f));
                for (float radius : radii) {
                    if (dy < 0 && std::abs(dx) <= -dy) {
                        distance = std::abs(length - radius);
                    } else {
                        const float endpoint = radius * cosine_45;
                        const float left_x = dx + endpoint;
                        const float right_x = dx - endpoint;
                        const float end_y = dy + endpoint;
                        distance = std::sqrt(std::min(left_x * left_x, right_x * right_x) + end_y * end_y);
                    }
                    coverage = std::max(coverage, Clamp(1.85f - distance));
                }
                auto &value = grey[y * kWidth + x];
                value = std::max(value, static_cast<uint8_t>(std::round(coverage * Clamp(strength) * 255)));
            }
        }
    }

    void FillOfflineSlash(float strength) {
        if (strength <= 0.001f) return;
        constexpr float start_x = 247;
        constexpr float start_y = 46;
        constexpr float segment_x = 32;
        constexpr float segment_y = 30;
        constexpr float segment_length_squared = segment_x * segment_x + segment_y * segment_y;
        for (int y = 42; y <= 80; ++y) {
            for (int x = 242; x <= 284; ++x) {
                const float dx = x + 0.5f - start_x;
                const float dy = y + 0.5f - start_y;
                const float along = Clamp((dx * segment_x + dy * segment_y) / segment_length_squared);
                const float distance_x = dx - along * segment_x;
                const float distance_y = dy - along * segment_y;
                const float distance = std::sqrt(distance_x * distance_x + distance_y * distance_y);
                auto &value = grey[y * kWidth + x];
                // A black clearance around the white slash keeps it readable
                // over the white radio arcs, without adding a new colour.
                const float clearance = Clamp(4.2f - distance) * Clamp(strength);
                value = static_cast<uint8_t>(std::round(value * (1 - clearance)));
                const float ink = Clamp(1.8f - distance) * Clamp(strength);
                value = std::max(value, static_cast<uint8_t>(std::round(ink * 255)));
            }
        }
    }

    // Small status marks use rounded strokes, with the same coverage-based
    // antialiasing as the face. A zero strength leaves earlier states untouched.
    void StrokeLine(float ax, float ay, float bx, float by, float width, float strength) {
        if (strength <= 0.001f) return;
        const float radius = width * 0.5f;
        const int x0 = std::max(0, static_cast<int>(std::floor(std::min(ax, bx) - radius - 1)));
        const int x1 = std::min(kWidth - 1, static_cast<int>(std::ceil(std::max(ax, bx) + radius + 1)));
        const int y0 = std::max(0, static_cast<int>(std::floor(std::min(ay, by) - radius - 1)));
        const int y1 = std::min(kHeight - 1, static_cast<int>(std::ceil(std::max(ay, by) + radius + 1)));
        const float dx = bx - ax, dy = by - ay;
        const float length_squared = dx * dx + dy * dy;
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const float px = x + 0.5f - ax, py = y + 0.5f - ay;
                const float along = length_squared > 0 ? Clamp((px * dx + py * dy) / length_squared) : 0;
                const float ex = px - along * dx, ey = py - along * dy;
                const float alpha = Clamp(radius + 0.5f - std::sqrt(ex * ex + ey * ey)) * Clamp(strength);
                auto &value = grey[y * kWidth + x];
                value = std::max(value, static_cast<uint8_t>(std::round(alpha * 255)));
            }
        }
    }

    void StrokeCircle(float cx, float cy, float radius, float width, float strength) {
        if (strength <= 0.001f) return;
        const float extent = radius + width * 0.5f + 1;
        const int x0 = std::max(0, static_cast<int>(std::floor(cx - extent)));
        const int x1 = std::min(kWidth - 1, static_cast<int>(std::ceil(cx + extent)));
        const int y0 = std::max(0, static_cast<int>(std::floor(cy - extent)));
        const int y1 = std::min(kHeight - 1, static_cast<int>(std::ceil(cy + extent)));
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
                const float distance = std::abs(std::sqrt(dx * dx + dy * dy) - radius);
                const float alpha = Clamp(width * 0.5f + 0.5f - distance) * Clamp(strength);
                auto &value = grey[y * kWidth + x];
                value = std::max(value, static_cast<uint8_t>(std::round(alpha * 255)));
            }
        }
    }

    void FillUpgrade(float strength) {
        if (strength <= 0.001f) return;
        StrokeLine(263, 45, 263, 65, 3.2f, strength);
        StrokeLine(253, 55, 263, 65, 3.2f, strength);
        StrokeLine(263, 65, 273, 55, 3.2f, strength);
        StrokeLine(249, 70, 249, 76, 3.0f, strength);
        StrokeLine(249, 76, 277, 76, 3.0f, strength);
        StrokeLine(277, 76, 277, 70, 3.0f, strength);
    }

    void FillActivation(float strength) {
        if (strength <= 0.001f) return;
        StrokeCircle(253, 54, 7, 3.0f, strength);
        StrokeLine(258, 59, 277, 76, 3.5f, strength);
        StrokeLine(268, 68, 264, 72, 3.0f, strength);
        StrokeLine(274, 73, 270, 77, 3.0f, strength);
    }

    void Render() {
        const Pose pose = CurrentPose();
        std::fill(grey.begin(), grey.end(), 0);
        FillContour(pose.contour);
        for (const auto &dot : pose.side_dots) FillDot(dot);
        for (const auto &eye : pose.eyes) CutEye(eye);
        FillWifi(pose.wifi_strength);
        FillOfflineSlash(pose.offline_strength);
        for (const auto &dot : pose.connection_dots) FillDot(dot);
        FillUpgrade(pose.upgrade_strength);
        FillActivation(pose.activation_strength);
        for (size_t i = 0; i < grey.size(); ++i) {
            const uint16_t value = grey[i];
            pixels[i] = static_cast<uint16_t>(((value >> 3) << 11) | ((value >> 2) << 5) | (value >> 3));
        }
        lv_obj_invalidate(canvas);
        ++frame_count;
    }

    void RequestRender() {
#ifdef ESP_PLATFORM
        render_pending = true;
        if (timer) lv_timer_ready(timer);
#else
        // Desktop preview APIs remain synchronous for deterministic snapshots.
        Render();
#endif
    }
};

LargeFace::LargeFace(lv_obj_t *parent, uint32_t frame_period_ms)
    : impl_(std::make_unique<Impl>(parent, frame_period_ms)) {}
LargeFace::~LargeFace() = default;

void LargeFace::SetState(FaceState state) {
    auto &self = *impl_;
    self.Advance();
    if (state == self.state && !self.manual_time) return;
    const Pose current = self.CurrentPose();
    self.state = state;
    self.origin = current;
    self.state_start = self.time;
    self.transition_start = self.time;
    self.transition_duration = self.playing ? 0.52 : 0;
    self.manual_time = false;
    self.RequestRender();
}

FaceState LargeFace::State() const { return impl_->state; }

void LargeFace::SetPlaying(bool playing) {
    auto &self = *impl_;
    self.Advance();
    self.playing = playing;
    // Resuming from a sampled frame continues from that phase, without a jump.
    if (playing) self.manual_time = false;
    self.last_tick = lv_tick_get();
    self.RequestRender();
}

bool LargeFace::Playing() const { return impl_->playing; }

void LargeFace::SetTimeForPreview(double seconds) {
    if (!std::isfinite(seconds)) return;
    auto &self = *impl_;
    self.time = std::max(0.0, seconds);
    self.state_start = 0;
    self.transition_duration = 0;
    self.manual_time = true;
    self.last_tick = lv_tick_get();
    self.RequestRender();
}

void LargeFace::AdvanceForPreview(double seconds) {
    if (!std::isfinite(seconds)) return;
    auto &self = *impl_;
    self.time += std::max(0.0, seconds);
    self.manual_time = true;
    self.last_tick = lv_tick_get();
    self.RequestRender();
}

void LargeFace::SetVisible(bool visible) {
    auto &self = *impl_;
    self.Advance();
    self.visible = visible;
    if (visible) {
        lv_obj_remove_flag(self.canvas, LV_OBJ_FLAG_HIDDEN);
        self.RequestRender();
    } else {
        lv_obj_add_flag(self.canvas, LV_OBJ_FLAG_HIDDEN);
    }
}

void LargeFace::SetCompact(bool compact) {
    auto &self = *impl_;
    if (self.compact == compact) return;
    self.compact = compact;
    lv_image_set_scale(self.canvas, compact ? 154 : 256);
    lv_obj_set_pos(self.canvas, 0, compact ? -44 : 0);
    lv_obj_invalidate(self.canvas);
}

uint64_t LargeFace::FrameCount() const { return impl_->frame_count; }

} // namespace face_ui
