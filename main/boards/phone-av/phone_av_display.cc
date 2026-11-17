#include "phone_av_display.h"

#include "display/lvgl_display/lvgl_font.h"
#include "display/lvgl_display/lvgl_image.h"

#include <chrono>
#include <utility>

namespace {

constexpr std::size_t kPhoneAvMaxLogLines = 64;
constexpr std::size_t kPhoneAvMaxChatLines = 60;

void AppendLocked(PhoneAvScreenState& state, const char* kind, const char* text) {
    if (text == nullptr || *text == '\0') {
        // Upstream clears bubbles with empty messages. Those blank lines used
        // to consume the history budget and push real subtitles out of view.
        return;
    }
    std::string line = "[";
    line += kind;
    line += "] ";
    line += text;
    state.history.push_back(line);
    if (state.history.size() > kPhoneAvMaxLogLines) {
        state.history.erase(state.history.begin());
    }
}

}  // namespace

PhoneAvScreenState& PhoneAvScreen() {
    static PhoneAvScreenState state;
    return state;
}

void PhoneAvDisplay::SetStatus(const char* status) {
    Display::SetStatus(status);
    auto& state = PhoneAvScreen();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.status = status != nullptr ? status : "";
    // A state change (idle / listening / speaking ...) marks a new turn, so
    // later fragments are not merged into the previous line.
    state.last_chat_role.clear();
    AppendLocked(state, "status", status);
}

void PhoneAvDisplay::SetEmotion(const char* emotion) {
    Display::SetEmotion(emotion);
    auto& state = PhoneAvScreen();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.emotion = emotion != nullptr ? emotion : "";
}

void PhoneAvDisplay::SetChatMessage(const char* role, const char* content) {
    Display::SetChatMessage(role, content);
    auto& state = PhoneAvScreen();
    std::lock_guard<std::mutex> lock(state.mutex);
    const std::string text = content != nullptr ? content : "";
    const std::string r = role != nullptr ? role : "chat";
    state.chat_message = text;
    if (text.empty()) {
        // Upstream clears the bubble with an empty message at the end of a
        // turn. Keep the recorded conversation intact but end the merge.
        state.last_chat_role.clear();
        return;
    }
    if (r == "user" || r == "assistant") {
        if (state.last_chat_role == r && !state.messages.empty()) {
            // TTS/STT subtitles arrive as consecutive fragments of one
            // sentence; append instead of creating another line.
            state.messages.back() += text;
        } else {
            state.messages.push_back("[" + r + "] " + text);
            if (state.messages.size() > kPhoneAvMaxChatLines) {
                state.messages.erase(state.messages.begin());
            }
        }
        state.last_chat_role = r;
    } else {
        AppendLocked(state, r.c_str(), text.c_str());
    }
}

void PhoneAvDisplay::ShowNotification(const char* notification, int duration_ms) {
    Display::ShowNotification(notification, duration_ms);
    auto& state = PhoneAvScreen();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.notification = notification != nullptr ? notification : "";
    AppendLocked(state, "notice", notification);
}

void PhoneAvDisplay::ClearChatMessages() {
    Display::ClearChatMessages();
    auto& state = PhoneAvScreen();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.chat_message.clear();
    state.last_chat_role.clear();
    // Keep the recorded conversation and history so the activation code and
    // subtitles remain readable when the device goes back to idle.
}

void PhoneAvDisplay::SetTheme(Theme* theme) {
    Display::SetTheme(theme);
    auto& state = PhoneAvScreen();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.theme = theme != nullptr ? theme->name() : "";
}

void PhoneAvDisplay::SetEmojiCollection(std::shared_ptr<EmojiCollection> collection) {
    const bool present = collection != nullptr;
    Display::SetEmojiCollection(std::move(collection));
    auto& state = PhoneAvScreen();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.custom_emoji_collection = present;
}

PhoneAvDisplayRouter::PhoneAvDisplayRouter(Display* hardware) {
    SetHardwareDisplay(hardware);
}

void PhoneAvDisplayRouter::SetHardwareDisplay(Display* hardware) {
    hardware_ = hardware;
    width_ = hardware_ != nullptr ? hardware_->width() : 0;
    height_ = hardware_ != nullptr ? hardware_->height() : 0;
}

void PhoneAvDisplayRouter::SetupUI() {
    phone_.SetupUI();
    if (hardware_ != nullptr) {
        hardware_->SetupUI();
    }
    setup_ui_called_ = true;
}

void PhoneAvDisplayRouter::SetStatus(const char* status) {
    phone_.SetStatus(status);
    if (hardware_ != nullptr) {
        hardware_->SetStatus(status);
    }
}

void PhoneAvDisplayRouter::SetEmotion(const char* emotion) {
    phone_.SetEmotion(emotion);
    if (hardware_ != nullptr) {
        hardware_->SetEmotion(emotion);
    }
}

void PhoneAvDisplayRouter::SetChatMessage(const char* role, const char* content) {
    phone_.SetChatMessage(role, content);
    if (hardware_ != nullptr) {
        hardware_->SetChatMessage(role, content);
    }
}

void PhoneAvDisplayRouter::ShowNotification(const char* notification, int duration_ms) {
    phone_.ShowNotification(notification, duration_ms);
    if (hardware_ != nullptr) {
        hardware_->ShowNotification(notification, duration_ms);
    }
}

void PhoneAvDisplayRouter::ShowNotification(const std::string& notification,
                                            int duration_ms) {
    phone_.ShowNotification(notification, duration_ms);
    if (hardware_ != nullptr) {
        hardware_->ShowNotification(notification, duration_ms);
    }
}

void PhoneAvDisplayRouter::ClearChatMessages() {
    phone_.ClearChatMessages();
    if (hardware_ != nullptr) {
        hardware_->ClearChatMessages();
    }
}

void PhoneAvDisplayRouter::SetTheme(Theme* theme) {
    phone_.SetTheme(theme);
    if (hardware_ != nullptr) {
        hardware_->SetTheme(theme);
    }
}

Theme* PhoneAvDisplayRouter::GetTheme() {
    return hardware_ != nullptr ? hardware_->GetTheme() : phone_.GetTheme();
}

void PhoneAvDisplayRouter::SetEmojiCollection(std::shared_ptr<EmojiCollection> collection) {
    phone_.SetEmojiCollection(collection);
    if (hardware_ != nullptr) {
        hardware_->SetEmojiCollection(std::move(collection));
    }
}

void PhoneAvDisplayRouter::UpdateStatusBar(bool update_all) {
    phone_.UpdateStatusBar(update_all);
    if (hardware_ != nullptr) {
        hardware_->UpdateStatusBar(update_all);
    }
}

void PhoneAvDisplayRouter::SetPowerSaveMode(bool on) {
    phone_.SetPowerSaveMode(on);
    if (hardware_ != nullptr) {
        hardware_->SetPowerSaveMode(on);
    }
}

void PhoneAvDisplayRouter::SetHideSubtitle(bool hide) {
    phone_.SetHideSubtitle(hide);
    if (hardware_ != nullptr) {
        hardware_->SetHideSubtitle(hide);
    }
}

bool PhoneAvDisplayRouter::AddTextGlyphs(const std::vector<TextGlyph>& glyphs, uint8_t bpp) {
    return hardware_ != nullptr ? hardware_->AddTextGlyphs(glyphs, bpp) : false;
}

void PhoneAvDisplayRouter::ClearTextGlyphs() {
    if (hardware_ != nullptr) {
        hardware_->ClearTextGlyphs();
    }
}

bool PhoneAvDisplayRouter::InsertAnimDialog(const char* name, uint32_t duration_ms) {
    return hardware_ != nullptr ? hardware_->InsertAnimDialog(name, duration_ms) : false;
}

bool PhoneAvDisplayRouter::MountAssets(const char* partition_label) {
    return hardware_ != nullptr ? hardware_->MountAssets(partition_label) : false;
}

void PhoneAvDisplayRouter::UnmountAssets() {
    if (hardware_ != nullptr) {
        hardware_->UnmountAssets();
    }
}

bool PhoneAvDisplayRouter::GetAssetData(const std::string& name, const uint8_t*& data,
                                        size_t& size) {
    return hardware_ != nullptr ? hardware_->GetAssetData(name, data, size) : false;
}

void PhoneAvDisplayRouter::LoadAssets() {
    if (hardware_ != nullptr) {
        hardware_->LoadAssets();
    }
}

void PhoneAvDisplayRouter::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    if (hardware_ != nullptr) {
        hardware_->SetPreviewImage(std::move(image));
    }
}

bool PhoneAvDisplayRouter::SetTextFont(std::shared_ptr<LvglFont> text_font) {
    return hardware_ != nullptr ? hardware_->SetTextFont(std::move(text_font)) : false;
}

bool PhoneAvDisplayRouter::SnapshotToJpeg(std::string& jpeg_data, int quality) {
    return hardware_ != nullptr ? hardware_->SnapshotToJpeg(jpeg_data, quality) : false;
}

bool PhoneAvDisplayRouter::IsMonochrome() const {
    return hardware_ != nullptr ? hardware_->IsMonochrome() : false;
}

bool PhoneAvDisplayRouter::SupportsGuiOperations() const {
    return hardware_ != nullptr && hardware_->SupportsGuiOperations();
}

bool PhoneAvDisplayRouter::Lock(int timeout_ms) {
    if (timeout_ms <= 0) {
        return mutex_.try_lock();
    }
    return mutex_.try_lock_for(std::chrono::milliseconds(timeout_ms));
}

void PhoneAvDisplayRouter::Unlock() {
    mutex_.unlock();
}
