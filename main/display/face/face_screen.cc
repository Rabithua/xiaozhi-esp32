#include "face_screen.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace face_ui {
namespace {

const char *StateCaption(FaceState state) {
    switch (state) {
        case FaceState::Idle: return "待命";
        case FaceState::Listening: return "聆听";
        case FaceState::Thinking: return "思考";
        case FaceState::Speaking: return "说话";
        case FaceState::WifiConfiguring: return "等待配网";
        case FaceState::Connecting: return "连接中…";
        case FaceState::Offline: return "离线 · 请重试";
        case FaceState::Upgrading: return "升级中";
        case FaceState::WaitingActivation: return "等待激活";
    }
    return "待命";
}

bool Reached(uint32_t now, uint32_t deadline) {
    return static_cast<int32_t>(now - deadline) >= 0;
}

void RequireObject(const void *object) {
    if (!object) throw std::runtime_error("Could not create the face screen objects");
}

struct ScrollPosition {
    int offset = 0;
    int direction = 1;
    int overflow = 0;
    uint32_t hold_until = 0;

    void Reset(uint32_t now) {
        offset = 0;
        direction = 1;
        overflow = 0;
        hold_until = now + 2000;
    }
};

enum class TextSource { None, Message, Notification };

} // namespace

struct FaceScreen::Impl {
    lv_obj_t *root = nullptr;
    lv_obj_t *caption = nullptr;
    lv_obj_t *upgrade_bar = nullptr;
    lv_obj_t *viewport = nullptr;
    lv_obj_t *message_label = nullptr;
    lv_timer_t *timer = nullptr;
    std::unique_ptr<LargeFace> face;
    int upgrade_progress = 0;
    bool visible = true;
    std::string message;
    bool important_message = false;
    std::string notification;
    bool notification_active = false;
    uint32_t notification_until = 0;
    ScrollPosition message_scroll;
    ScrollPosition notification_scroll;
    TextSource shown_source = TextSource::None;
    std::string rendered_text;

    Impl(lv_obj_t *parent, const lv_font_t *font, uint32_t frame_period_ms) {
        try {
            root = lv_obj_create(parent);
            RequireObject(root);
            lv_obj_remove_style_all(root);
            lv_obj_set_pos(root, 0, 0);
            lv_obj_set_size(root, 320, 240);
            lv_obj_set_style_bg_color(root, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
            lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

            face = std::make_unique<LargeFace>(root, frame_period_ms);

            upgrade_bar = lv_bar_create(root);
            RequireObject(upgrade_bar);
            lv_obj_remove_style_all(upgrade_bar);
            lv_obj_set_pos(upgrade_bar, 84, 191);
            lv_obj_set_size(upgrade_bar, 152, 4);
            lv_obj_set_style_bg_color(upgrade_bar, lv_color_hex(0x242424), 0);
            lv_obj_set_style_bg_opa(upgrade_bar, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(upgrade_bar, 2, 0);
            lv_obj_set_style_bg_color(upgrade_bar, lv_color_white(), LV_PART_INDICATOR);
            lv_obj_set_style_bg_opa(upgrade_bar, LV_OPA_COVER, LV_PART_INDICATOR);
            lv_obj_set_style_radius(upgrade_bar, 2, LV_PART_INDICATOR);
            lv_bar_set_range(upgrade_bar, 0, 100);
            lv_bar_set_value(upgrade_bar, 0, LV_ANIM_OFF);

            caption = lv_label_create(root);
            RequireObject(caption);
            lv_obj_set_style_text_font(caption, font ? font : LV_FONT_DEFAULT, 0);
            lv_obj_set_style_text_color(caption, lv_color_hex(0x858585), 0);

            viewport = lv_obj_create(root);
            RequireObject(viewport);
            lv_obj_remove_style_all(viewport);
            lv_obj_remove_flag(viewport, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_bg_color(viewport, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(viewport, LV_OPA_COVER, 0);

            message_label = lv_label_create(viewport);
            RequireObject(message_label);
            lv_obj_set_style_text_font(message_label, font ? font : LV_FONT_DEFAULT, 0);
            lv_obj_set_style_text_color(message_label, lv_color_white(), 0);
            lv_obj_set_width(message_label, 296);
            lv_obj_set_height(message_label, LV_SIZE_CONTENT);
            lv_label_set_long_mode(message_label, LV_LABEL_LONG_WRAP);

            const uint32_t now = lv_tick_get();
            message_scroll.Reset(now);
            notification_scroll.Reset(now);
            ApplyLayout();
            timer = lv_timer_create([](lv_timer_t *timer) {
                static_cast<Impl *>(lv_timer_get_user_data(timer))->Tick();
            }, 100, this);
            if (!timer) throw std::runtime_error("Could not create the face text timer");
        } catch (...) {
            if (timer) lv_timer_delete(timer);
            face.reset();
            if (root) lv_obj_delete(root);
            root = nullptr;
            throw;
        }
    }

    ~Impl() {
        if (timer) lv_timer_delete(timer);
        face.reset();
        if (root) lv_obj_delete(root);
    }

    void UpdateCaption() {
        const bool upgrading = face->State() == FaceState::Upgrading;
        const std::string text = upgrading ? "升级中 " + std::to_string(upgrade_progress) + "%"
                                           : StateCaption(face->State());
        lv_label_set_text(caption, text.c_str());
        lv_obj_set_style_text_color(caption, upgrading ? lv_color_white() : lv_color_hex(0x858585), 0);
        lv_obj_align(caption, LV_ALIGN_BOTTOM_MID, 0, -13);
    }

    bool ExpireNotification(uint32_t now) {
        if (!notification_active || !Reached(now, notification_until)) return false;
        notification_active = false;
        return true;
    }

    ScrollPosition *ActiveScroll() {
        if (shown_source == TextSource::Message) return &message_scroll;
        if (shown_source == TextSource::Notification) return &notification_scroll;
        return nullptr;
    }

    void ApplyLayout() {
        ExpireNotification(lv_tick_get());
        const TextSource previous_source = shown_source;
        const bool upgrading = face->State() == FaceState::Upgrading;
        // Upgrade progress must stay visible. Important operational information
        // still gets room, while ordinary subtitles/notifications wait behind it.
        const bool show_text = (notification_active || !message.empty()) && (!upgrading || important_message);
        const bool compact = show_text && important_message;
        face->SetCompact(compact);
        UpdateCaption();

        if (upgrading && !compact) lv_obj_remove_flag(upgrade_bar, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(upgrade_bar, LV_OBJ_FLAG_HIDDEN);

        if (!show_text) {
            shown_source = TextSource::None;
            lv_obj_add_flag(viewport, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(caption, LV_OBJ_FLAG_HIDDEN);
            return;
        }

        shown_source = notification_active ? TextSource::Notification : TextSource::Message;
        const auto &text = notification_active ? notification : message;
        if (rendered_text != text) {
            lv_label_set_text(message_label, text.c_str());
            rendered_text = text;
        }
        // The BOX font has a 25px line height; leave room for two full lines.
        lv_obj_set_pos(viewport, 12, compact ? 110 : 184);
        lv_obj_set_size(viewport, 296, compact ? 94 : 56);
        lv_obj_set_style_text_align(message_label, compact ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_remove_flag(viewport, LV_OBJ_FLAG_HIDDEN);
        if (compact) lv_obj_remove_flag(caption, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(caption, LV_OBJ_FLAG_HIDDEN);
        lv_obj_update_layout(root);
        auto *scroll = ActiveScroll();
        if (previous_source != shown_source) scroll->hold_until = lv_tick_get() + 2000;
        scroll->overflow = std::max(0, static_cast<int>(lv_obj_get_height(message_label) - lv_obj_get_height(viewport)));
        scroll->offset = std::clamp(scroll->offset, 0, scroll->overflow);
        lv_obj_set_pos(message_label, 0, -scroll->offset);
    }

    void Tick() {
        const uint32_t now = lv_tick_get();
        if (ExpireNotification(now)) ApplyLayout();
        if (!visible) return;
        auto *scroll = ActiveScroll();
        if (!scroll || scroll->overflow <= 0 || !Reached(now, scroll->hold_until)) return;
        scroll->offset += 2 * scroll->direction;
        if (scroll->offset >= scroll->overflow) {
            scroll->offset = scroll->overflow;
            scroll->direction = -1;
            scroll->hold_until = now + 2000;
        } else if (scroll->offset <= 0) {
            scroll->offset = 0;
            scroll->direction = 1;
            scroll->hold_until = now + 2000;
        }
        lv_obj_set_pos(message_label, 0, -scroll->offset);
    }
};

FaceScreen::FaceScreen(lv_obj_t *parent, const lv_font_t *font, uint32_t frame_period_ms)
    : impl_(std::make_unique<Impl>(parent, font, frame_period_ms)) {}

FaceScreen::~FaceScreen() = default;

LargeFace &FaceScreen::Face() { return *impl_->face; }

void FaceScreen::SetState(FaceState state) {
    impl_->face->SetState(state);
    impl_->ApplyLayout();
}

void FaceScreen::SetUpgradeProgress(int progress) {
    impl_->upgrade_progress = std::clamp(progress, 0, 100);
    lv_bar_set_value(impl_->upgrade_bar, impl_->upgrade_progress, LV_ANIM_OFF);
    if (impl_->face->State() == FaceState::Upgrading) impl_->UpdateCaption();
}

int FaceScreen::UpgradeProgress() const { return lv_bar_get_value(impl_->upgrade_bar); }

const char *FaceScreen::Caption() const { return lv_label_get_text(impl_->caption); }

void FaceScreen::SetMessage(const std::string &message, bool important) {
    auto &self = *impl_;
    const bool same_text = self.message == message;
    const bool same_importance = self.important_message == (important && !message.empty());
    if (same_text && same_importance) return;
    self.message = message;
    self.important_message = important && !message.empty();
    if (!same_text) self.message_scroll.Reset(lv_tick_get());
    self.ApplyLayout();
}

void FaceScreen::ClearMessage() { SetMessage(""); }

void FaceScreen::ShowNotification(const std::string &message, uint32_t duration_ms) {
    auto &self = *impl_;
    const uint32_t now = lv_tick_get();
    self.ExpireNotification(now);
    if (message.empty() || duration_ms == 0) {
        self.notification_active = false;
    } else {
        if (!self.notification_active || self.notification != message) self.notification_scroll.Reset(now);
        self.notification = message;
        self.notification_active = true;
        // Keep the signed wraparound comparison valid, even for an accidental
        // caller-supplied duration longer than half the LVGL tick period.
        self.notification_until = now + std::min(duration_ms, static_cast<uint32_t>(std::numeric_limits<int32_t>::max()));
    }
    self.ApplyLayout();
}

void FaceScreen::SetVisible(bool visible) {
    auto &self = *impl_;
    if (self.visible == visible) return;
    self.visible = visible;
    self.face->SetVisible(visible);
    if (visible) {
        lv_obj_remove_flag(self.root, LV_OBJ_FLAG_HIDDEN);
        self.ApplyLayout();
        if (auto *scroll = self.ActiveScroll()) scroll->hold_until = lv_tick_get() + 2000;
    } else {
        lv_obj_add_flag(self.root, LV_OBJ_FLAG_HIDDEN);
    }
}

lv_obj_t *FaceScreen::Root() const { return impl_->root; }

} // namespace face_ui
