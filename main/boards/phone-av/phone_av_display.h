#ifndef _PHONE_AV_DISPLAY_H_
#define _PHONE_AV_DISPLAY_H_

#include "display.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

/* The virtual screen state mirrored on the 8080 page. It is always updated,
 * even when a physical LCD is also active, so the page can show the activation
 * code, subtitles and the status log in either mode. */
struct PhoneAvScreenState {
    std::mutex mutex;
    std::string status;
    std::string emotion;
    std::string chat_message;
    std::string notification;
    std::string theme;
    bool custom_emoji_collection = false;
    std::string last_chat_role;
    std::vector<std::string> messages;  // complete [user]/[assistant] lines
    std::vector<std::string> history;   // device status / notification log
};

PhoneAvScreenState& PhoneAvScreen();

/* Records Display calls for the 8080 page. It does not draw anything itself. */
class PhoneAvDisplay : public Display {
public:
    using Display::ShowNotification;

    void SetStatus(const char* status) override;
    void SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;
    void ShowNotification(const char* notification, int duration_ms) override;
    void ClearChatMessages() override;
    void SetTheme(Theme* theme) override;
    void SetEmojiCollection(std::shared_ptr<EmojiCollection> collection) override;

private:
    bool Lock(int timeout_ms = 0) override { return true; }
    void Unlock() override {}
};

/* Fans every Display call out to the virtual phone screen and, when a local
 * LCD is configured and selected, to the physical display as well. */
class PhoneAvDisplayRouter : public Display {
public:
    explicit PhoneAvDisplayRouter(Display* hardware = nullptr);

    void SetHardwareDisplay(Display* hardware);

    void SetupUI() override;

    void SetStatus(const char* status) override;
    void SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;
    void ShowNotification(const char* notification, int duration_ms) override;
    void ShowNotification(const std::string& notification, int duration_ms) override;
    void ClearChatMessages() override;

    void SetTheme(Theme* theme) override;
    Theme* GetTheme() override;
    void SetEmojiCollection(std::shared_ptr<EmojiCollection> collection) override;

    void UpdateStatusBar(bool update_all = false) override;
    void SetPowerSaveMode(bool on) override;
    void SetHideSubtitle(bool hide) override;

    bool AddTextGlyphs(const std::vector<TextGlyph>& glyphs, uint8_t bpp) override;
    void ClearTextGlyphs() override;
    bool InsertAnimDialog(const char* name, uint32_t duration_ms) override;

    bool MountAssets(const char* partition_label) override;
    void UnmountAssets() override;
    bool GetAssetData(const std::string& name, const uint8_t*& data, size_t& size) override;
    void LoadAssets() override;

    void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    bool SetTextFont(std::shared_ptr<LvglFont> text_font) override;
    bool SnapshotToJpeg(std::string& jpeg_data, int quality = 80) override;

    bool IsMonochrome() const override;
    bool SupportsGuiOperations() const override;

    Display* hardware_display() const { return hardware_; }

private:
    bool Lock(int timeout_ms = 0) override;
    void Unlock() override;

    PhoneAvDisplay phone_;
    Display* hardware_ = nullptr;
    std::timed_mutex mutex_;
};

#endif  // _PHONE_AV_DISPLAY_H_
