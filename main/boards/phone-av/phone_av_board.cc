#include "wifi_board.h"
#include "button.h"
#include "audio_codec.h"
#include "camera.h"
#include "display.h"
#include "config.h"
#include "settings.h"
#include "system_info.h"
#include "application.h"
#include <esp_timer.h>

#include "led/led.h"
#include "led/single_led.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <esp_http_server.h>
#include <esp_wifi.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "PhoneAvBoard"

namespace {

bool PhoneAvNetworkReady();

// Diagnostics endpoint handler; defined below (after PhoneAvBoard is complete).
esp_err_t PhoneAvStatusHandler(httpd_req_t* req);

/* This board has no physical display, so xiaozhi's "screen" content (status,
 * notifications and chat messages -- including the 6-digit activation code in
 * activation.message) is mirrored here and served to the phone over the LAN
 * config page. Display callbacks run from the application task while the HTTP
 * handler runs from the httpd task, so all state is mutex protected. */
struct PhoneAvScreenState {
    std::mutex mutex;
    std::string status;
    std::string emotion;
    std::string chat_message;
    std::string notification;
    std::vector<std::string> history;
};

PhoneAvScreenState& PhoneAvScreen() {
    static PhoneAvScreenState state;
    return state;
}

std::string PhoneAvJsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (unsigned char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

void PhoneAvScreenAppendLocked(PhoneAvScreenState& state, const char* kind, const char* text) {
    std::string line = "[";
    line += kind;
    line += "] ";
    line += text != nullptr ? text : "";
    state.history.push_back(line);
    if (state.history.size() > 24) {
        state.history.erase(state.history.begin());
    }
}

class PhoneAvDisplay : public Display {
public:
    using Display::ShowNotification;

    void SetStatus(const char* status) override {
        Display::SetStatus(status);
        auto& state = PhoneAvScreen();
        std::lock_guard<std::mutex> lock(state.mutex);
        state.status = status != nullptr ? status : "";
        PhoneAvScreenAppendLocked(state, "status", status);
    }

    void SetEmotion(const char* emotion) override {
        Display::SetEmotion(emotion);
        auto& state = PhoneAvScreen();
        std::lock_guard<std::mutex> lock(state.mutex);
        state.emotion = emotion != nullptr ? emotion : "";
    }

    void SetChatMessage(const char* role, const char* content) override {
        Display::SetChatMessage(role, content);
        auto& state = PhoneAvScreen();
        std::lock_guard<std::mutex> lock(state.mutex);
        state.chat_message = content != nullptr ? content : "";
        PhoneAvScreenAppendLocked(state, role != nullptr ? role : "chat", content);
    }

    void ShowNotification(const char* notification, int duration_ms) override {
        Display::ShowNotification(notification, duration_ms);
        auto& state = PhoneAvScreen();
        std::lock_guard<std::mutex> lock(state.mutex);
        state.notification = notification != nullptr ? notification : "";
        PhoneAvScreenAppendLocked(state, "notice", notification);
    }

    void ClearChatMessages() override {
        Display::ClearChatMessages();
        auto& state = PhoneAvScreen();
        std::lock_guard<std::mutex> lock(state.mutex);
        state.chat_message.clear();
        // Keep the history so an activation code shown earlier remains readable
        // on the phone page after the device goes back to idle.
    }

private:
    bool Lock(int timeout_ms = 0) override { return true; }
    void Unlock() override {}
};

constexpr uint16_t kPhoneAvConfigPort = 8080;
constexpr const char* kPhoneAvConfigNs = "phone_av";
constexpr const char* kPhoneAvConfigKey = "host";

/* The phone address is runtime configuration, not a build-time constant.
 * It is kept in NVS so it survives reboots and firmware updates. */
std::string PhoneAvHost() {
    Settings settings(kPhoneAvConfigNs, false);
    return settings.GetString(kPhoneAvConfigKey, PHONE_AV_HOST);
}

std::string PhoneAvUrl(const char* path) {
    return std::string("http://") + PhoneAvHost() + ":" + std::to_string(PHONE_AV_PORT) + path;
}

bool IsValidPhoneAvHost(const std::string& host) {
    if (host.empty() || host.size() > 64) {
        return false;
    }
    for (char ch : host) {
        auto c = static_cast<unsigned char>(ch);
        if (!std::isalnum(c) && c != '.' && c != '-' && c != ':' && c != '_') {
            return false;
        }
    }
    return true;
}

std::string PhoneAvConfigPage(const std::string& message) {
    const std::string host = PhoneAvHost();
    std::string html =
        "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>phone-av 设置</title><style>"
        "body{font-family:sans-serif;max-width:460px;margin:40px auto;padding:0 16px;line-height:1.6}"
        "input{font-size:16px;padding:8px;width:100%;box-sizing:border-box}"
        "button{font-size:16px;padding:10px 20px;margin-top:12px}"
        "code{background:#eee;padding:2px 6px}</style></head><body>"
        "<h2>手机 AV 地址设置</h2>"
        "<p>当前手机地址：<code>" + host + "</code></p>"
        "<p>端口：<code>" + std::to_string(PHONE_AV_PORT) +
        "</code>（需与手机 App 的「推流端口」一致）</p>"
        "<form method=\"post\" action=\"/set\">"
        "<label>手机 IP 或域名<br><input name=\"host\" value=\"" + host +
        "\" required></label>"
        "<button type=\"submit\">保存并重启</button></form>";
    if (!message.empty()) {
        html += "<p><b>" + message + "</b></p>";
    }
    html +=
        "<p>保存后板子会自动重启一次让新地址生效；WiFi 配置不会丢失。"
        "以后手机 IP 变了只需再来这里改，不用重刷固件。</p>"
        "<p><a href=\"/api/host\">查看当前地址（纯文本）</a></p>"
        "<h2>设备屏幕（虚拟）</h2>"
        "<p>本板无物理屏幕，xiaozhi 的屏幕内容（含 6 位设备验证码）显示在这里，每秒刷新。</p>"
        "<div id=\"screen\" style=\"white-space:pre-wrap;word-break:break-all;"
        "background:#111;color:#eee;padding:12px;border-radius:6px;font-family:monospace\">"
        "读取中...</div>"
        "<script>"
        "async function refreshScreen(){"
        "  try{"
        "    const r=await fetch('/status',{cache:'no-store'});"
        "    const j=await r.json();"
        "    let t='状态: '+(j.screen_status||'')+'\\n';"
        "    if(j.screen_chat_message)t+='消息: '+j.screen_chat_message+'\\n';"
        "    if(j.screen_notification)t+='通知: '+j.screen_notification+'\\n';"
        "    t+='\\n--- 最近屏幕记录 ---\\n';"
        "    t+=(j.screen_history||[]).join('\\n');"
        "    document.getElementById('screen').textContent=t;"
        "  }catch(e){document.getElementById('screen').textContent='读取失败: '+e;}"
        "}"
        "setInterval(refreshScreen,1000);refreshScreen();"
        "</script>"
        "</body></html>";
    return html;
}

esp_err_t PhoneAvConfigGetHandler(httpd_req_t* req) {
    const std::string page = PhoneAvConfigPage("");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, page.c_str(), HTTPD_RESP_USE_STRLEN);
}

esp_err_t PhoneAvConfigHostHandler(httpd_req_t* req) {
    const std::string host = PhoneAvHost();
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, host.c_str(), HTTPD_RESP_USE_STRLEN);
}

void PhoneAvRestartTask(void*) {
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    vTaskDelete(nullptr);
}

esp_err_t PhoneAvConfigSetHandler(httpd_req_t* req) {
    char body[256] = {0};
    int received = httpd_req_recv(req, body, static_cast<size_t>(sizeof(body) - 1));
    if (received < 0) {
        const std::string page = PhoneAvConfigPage("读取请求失败，请重试");
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_send(req, page.c_str(), HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }
    body[received] = '\0';
    const std::string form(body, static_cast<std::size_t>(received));
    const std::string key = "host=";
    std::string host;
    auto pos = form.find(key);
    if (pos != std::string::npos) {
        host = form.substr(pos + key.size());
        auto amp = host.find('&');
        if (amp != std::string::npos) {
            host.resize(amp);
        }
    }
    if (!IsValidPhoneAvHost(host)) {
        const std::string page = PhoneAvConfigPage("地址无效，请填写 IP（如 192.168.0.108）或域名");
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_send(req, page.c_str(), HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    {
        Settings settings(kPhoneAvConfigNs, true);
        settings.SetString(kPhoneAvConfigKey, host);
    }
    ESP_LOGI(TAG, "Phone AV host updated to %s, restarting", host.c_str());
    const std::string page = PhoneAvConfigPage("已保存为 " + host + "，板子即将自动重启…");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, page.c_str(), HTTPD_RESP_USE_STRLEN);
    xTaskCreate(PhoneAvRestartTask, "phone_av_restart", 2048, nullptr, 5, nullptr);
    return ESP_OK;
}

void StartPhoneAvConfigServer() {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) {
        return;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = kPhoneAvConfigPort;
    config.max_uri_handlers = 5;
    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start phone-av config server");
        return;
    }
    httpd_uri_t root_uri = {
        .uri = "/", .method = HTTP_GET,
        .handler = PhoneAvConfigGetHandler, .user_ctx = nullptr,
    };
    httpd_uri_t host_uri = {
        .uri = "/api/host", .method = HTTP_GET,
        .handler = PhoneAvConfigHostHandler, .user_ctx = nullptr,
    };
    httpd_uri_t set_uri = {
        .uri = "/set", .method = HTTP_POST,
        .handler = PhoneAvConfigSetHandler, .user_ctx = nullptr,
    };
    httpd_register_uri_handler(server, &root_uri);
    httpd_register_uri_handler(server, &host_uri);
    httpd_register_uri_handler(server, &set_uri);
    httpd_uri_t status_uri = {
        .uri = "/status", .method = HTTP_GET,
        .handler = PhoneAvStatusHandler, .user_ctx = nullptr,
    };
    httpd_register_uri_handler(server, &status_uri);

    esp_netif_t* netif = esp_netif_get_default_netif();
    esp_netif_ip_info_t info{};
    if (netif != nullptr && esp_netif_get_ip_info(netif, &info) == ESP_OK) {
        ESP_LOGI(TAG, "Phone AV config page: http://" IPSTR ":%u/", IP2STR(&info.ip),
                 static_cast<unsigned>(kPhoneAvConfigPort));
    } else {
        ESP_LOGI(TAG, "Phone AV config page listening on port %u",
                 static_cast<unsigned>(kPhoneAvConfigPort));
    }
    ESP_LOGI(TAG, "Phone AV host: %s (change it on the config page)", PhoneAvHost().c_str());
}

class PcmRingBuffer {
public:
    explicit PcmRingBuffer(std::size_t capacity)
        : buffer_(capacity), capacity_(capacity) {}

    void Push(const int16_t* data, std::size_t samples) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (samples >= capacity_) {
            data += samples - capacity_;
            samples = capacity_;
            read_pos_ = 0;
            write_pos_ = 0;
            count_ = 0;
        }
        for (std::size_t i = 0; i < samples; ++i) {
            if (count_ == capacity_) {
                read_pos_ = (read_pos_ + 1) % capacity_;
                --count_;
            }
            buffer_[write_pos_] = data[i];
            write_pos_ = (write_pos_ + 1) % capacity_;
            ++count_;
        }
    }

    std::size_t Pop(int16_t* dest, std::size_t samples) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t count = std::min(samples, count_);
        for (std::size_t i = 0; i < count; ++i) {
            dest[i] = buffer_[read_pos_];
            read_pos_ = (read_pos_ + 1) % capacity_;
            --count_;
        }
        return count;
    }

private:
    std::vector<int16_t> buffer_;
    std::size_t capacity_;
    std::size_t read_pos_ = 0;
    std::size_t write_pos_ = 0;
    std::size_t count_ = 0;
    std::mutex mutex_;
};

class NetworkAudioCodec : public AudioCodec {
public:
    NetworkAudioCodec(int input_sample_rate, int output_sample_rate)
        : mic_buffer_(kBufferSamples), speaker_buffer_(kBufferSamples) {
        duplex_ = true;
        input_reference_ = false;
        input_channels_ = 1;
        output_channels_ = 1;
        input_sample_rate_ = input_sample_rate;
        output_sample_rate_ = output_sample_rate;
    }

    virtual void Start() override {
        AudioCodec::Start();
        if (started_.exchange(true)) {
            return;
        }
        running_ = true;
        BaseType_t mic_result = xTaskCreate(MicTaskEntry, "phone_av_mic", 8192, this, 5, nullptr);
        BaseType_t speaker_result = xTaskCreate(SpeakerTaskEntry, "phone_av_spk", 8192, this, 5, nullptr);
        if (mic_result != pdPASS || speaker_result != pdPASS) {
            ESP_LOGE(TAG, "Failed to create phone-av network audio tasks");
        } else {
            ESP_LOGI(TAG, "phone-av network audio tasks started");
        }
    }

public:
    virtual int Read(int16_t* dest, int samples) override {
        if (dest == nullptr || samples <= 0) {
            return 0;
        }
        // Pace this read to the real audio rate. The upstream AudioService audio_input
        // task calls Read() in a tight loop with no internal delay; for a real I2S mic the
        // codec blocks until realtime samples arrive, which naturally paces the loop.
        // Here the mic comes from the phone over HTTP and Pop() returns immediately, so
        // without pacing the task spins at 100% CPU, floods the AFE feed ringbuffer
        // ("AFE ringbuffer full"), triggers the task watchdog, and makes the AFE fall
        // behind (encode queue overflow -> audio never reaches the server intact).
        // Delaying to the realtime schedule keeps the pipeline at input_sample_rate and
        // yields to other tasks.
        const TickType_t now = xTaskGetTickCount();
        if (last_read_tick_ != 0) {
            const TickType_t expected =
                last_read_tick_ + pdMS_TO_TICKS(samples * 1000 / input_sample_rate_);
            if ((int32_t)(expected - now) > 0) {
                vTaskDelay(expected - now);
            }
        }
        last_read_tick_ = xTaskGetTickCount();

        std::size_t requested = static_cast<std::size_t>(samples);
        std::size_t got = mic_buffer_.Pop(dest, requested);
        if (got < requested) {
            std::memset(dest + got, 0, (requested - got) * sizeof(int16_t));
        }
        return samples;
    }

    virtual int Write(const int16_t* data, int samples) override {
        if (data == nullptr || samples <= 0) {
            return 0;
        }
        speaker_buffer_.Push(data, static_cast<std::size_t>(samples));
        return samples;
    }

    // Audio for this board is streamed to the phone, not played on the on-board
    // DAC. Force the local output off so the board's amplifier / power stage stays
    // silent (no idle current noise / hum). The actual samples still reach the
    // phone via SpeakerTask -> speaker_buffer_.
    void EnableOutput(bool /*enable*/) override {
        AudioCodec::EnableOutput(false);
    }

    // True once the phone's upstream microphone stream is actually open. The
    // auto-listen logic uses this to avoid entering a chat session (and blinking
    // the LED) when the phone app that provides the mic/speaker is not running.
    bool IsPhoneMicConnected() const { return mic_connected_; }
    uint32_t MicConnectCount() const { return mic_connect_count_; }
    uint32_t MicDisconnectCount() const { return mic_disconnect_count_; }

    private:
    static constexpr std::size_t kBufferSamples = 16000;
    static constexpr std::size_t kHttpReadSize = 1024;
    static constexpr std::size_t kPostSamples = 320;

    static void MicTaskEntry(void* arg) {
        auto* codec = static_cast<NetworkAudioCodec*>(arg);
        codec->MicTask();
        vTaskDelete(nullptr);
    }

    static void SpeakerTaskEntry(void* arg) {
        auto* codec = static_cast<NetworkAudioCodec*>(arg);
        codec->SpeakerTask();
        vTaskDelete(nullptr);
    }

    void PushAudioBytes(const uint8_t* data, std::size_t len, bool& pending_valid, uint8_t& pending) {
        int16_t samples[kHttpReadSize / 2];
        std::size_t count = 0;
        for (std::size_t i = 0; i < len; ++i) {
            uint8_t byte = data[i];
            if (!pending_valid) {
                pending = byte;
                pending_valid = true;
                continue;
            }
            uint16_t value = static_cast<uint16_t>(pending) | (static_cast<uint16_t>(byte) << 8);
            pending_valid = false;
            samples[count++] = static_cast<int16_t>(value);
            if (count == kHttpReadSize / 2) {
                mic_buffer_.Push(samples, count);
                count = 0;
            }
        }
        if (count > 0) {
            mic_buffer_.Push(samples, count);
        }
    }

    void MicTask() {
        const std::string url = PhoneAvUrl(PHONE_AV_MIC_PATH);
        uint8_t raw[kHttpReadSize];
        bool pending_valid = false;
        uint8_t pending = 0;

        while (running_) {
            while (running_ && !PhoneAvNetworkReady()) {
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            if (!running_) {
                break;
            }

            auto network = Board::GetInstance().GetNetwork();
            auto http = network->CreateHttp(0);
            if (auto opened = http->Open("GET", url); !opened) {
                ESP_LOGW(TAG, "Failed to open phone microphone stream");
                http->Close();
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            auto status = http->GetStatusCode();
            if (!status || *status != 200) {
                ESP_LOGW(TAG, "Phone microphone stream rejected the request");
                http->Close();
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }

            mic_connected_ = true;
            mic_connect_count_++;
            pending_valid = false;
            zero_read_count_ = 0;
            while (running_ && PhoneAvNetworkReady()) {
                // Keep the WiFi modem fully awake while the phone's upstream mic stream is
                // active. If the radio is allowed to enter modem sleep the TCP flow stalls,
                // the phone's send buffer fills, its writeChunk() times out and closes the
                // socket, which used to trigger a tight reconnect storm (and the green LED
                // blinking). This board is a tethered streaming device, so always-on WiFi is
                // the right trade-off.
                esp_wifi_set_ps(WIFI_PS_NONE);
                auto ret = http->Read(reinterpret_cast<char*>(raw), sizeof(raw));
                if (!ret) {
                    ESP_LOGW(TAG, "Phone microphone stream read failed");
                    break;
                }
                int n = *ret;
                if (n == 0) {
                    // A zero-length read is a transient gap or silence from the phone, not
                    // necessarily a closed socket. Keep the connection open and retry; a
                    // genuine close is detected through the !ret path above. Only bail out
                    // after a very long idle (dead / half-open socket) so we can recover.
                    if (++zero_read_count_ > 10000) {
                        ESP_LOGW(TAG, "Phone microphone stream idle too long, reconnecting");
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(5));
                    continue;
                }
                zero_read_count_ = 0;
                PushAudioBytes(raw, static_cast<std::size_t>(n), pending_valid, pending);
            }
            http->Close();
            mic_connected_ = false;
            mic_disconnect_count_++;
            // The phone mic just dropped. If we were actively listening on it,
            // leave the chat session so the device returns to idle (LED off)
            // instead of sitting in a dead "listening" state with no audio.
            if (Application::GetInstance().GetDeviceState() == kDeviceStateListening) {
                Application::GetInstance().StopListening();
            }
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }

    bool PostPcm(const std::string& url, const int16_t* data, std::size_t samples) {
        if (!PhoneAvNetworkReady()) {
            return false;
        }
        auto network = Board::GetInstance().GetNetwork();
        auto http = network->CreateHttp(0);
        std::string body(reinterpret_cast<const char*>(data), samples * sizeof(int16_t));
        http->SetHeader("Content-Type", "application/octet-stream");
        http->SetContent(std::move(body));
        if (auto opened = http->Open("POST", url); !opened) {
            http->Close();
            return false;
        }
        auto status = http->GetStatusCode();
        http->ReadAll();
        http->Close();
        return status && *status == 200;
    }

    void SpeakerTask() {
        const std::string url = PhoneAvUrl(PHONE_AV_SPEAKER_PATH);
        int16_t chunk[kPostSamples];
        while (running_) {
            if (!PhoneAvNetworkReady()) {
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
            std::size_t count = speaker_buffer_.Pop(chunk, kPostSamples);
            if (count == 0) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            if (!PostPcm(url, chunk, count)) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
    }

    PcmRingBuffer mic_buffer_;
    PcmRingBuffer speaker_buffer_;
    std::atomic<bool> started_{false};
    std::atomic<bool> running_{false};
    TickType_t last_read_tick_{0};
    int zero_read_count_ = 0;
    std::atomic<bool> mic_connected_{false};
    std::atomic<uint32_t> mic_connect_count_{0};
    std::atomic<uint32_t> mic_disconnect_count_{0};
    };

class NetworkCamera : public Camera {
public:
    virtual void SetExplainUrl(const std::string& url, const std::string& token) override {
        std::lock_guard<std::mutex> lock(state_mutex_);
        explain_url_ = url;
        explain_token_ = token;
    }

    virtual bool Capture() override {
        if (!PhoneAvNetworkReady()) {
            return false;
        }
        auto network = Board::GetInstance().GetNetwork();
        auto http = network->CreateHttp(0);
        const std::string url = PhoneAvUrl(PHONE_AV_SNAPSHOT_PATH);
        if (auto opened = http->Open("GET", url); !opened) {
            ESP_LOGW(TAG, "Failed to open phone camera snapshot");
            http->Close();
            return false;
        }
        auto status = http->GetStatusCode();
        if (!status || *status != 200) {
            ESP_LOGW(TAG, "Phone camera snapshot rejected the request");
            http->Close();
            return false;
        }
        std::string jpeg = http->ReadAll();
        http->Close();
        if (jpeg.size() < 4 ||
            static_cast<uint8_t>(jpeg[0]) != 0xFF || static_cast<uint8_t>(jpeg[1]) != 0xD8 ||
            static_cast<uint8_t>(jpeg[jpeg.size() - 2]) != 0xFF || static_cast<uint8_t>(jpeg[jpeg.size() - 1]) != 0xD9) {
            ESP_LOGW(TAG, "Phone camera snapshot is not a complete JPEG");
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            jpeg_ = std::move(jpeg);
        }
        return true;
    }

    virtual bool SetHMirror(bool enabled) override {
        (void)enabled;
        return false;
    }

    virtual bool SetVFlip(bool enabled) override {
        (void)enabled;
        return false;
    }

    virtual std::expected<std::string, std::string> Explain(const std::string& question) override {
        if (!PhoneAvNetworkReady()) {
            return std::unexpected(std::string("Network not connected"));
        }
        std::string jpeg;
        std::string url;
        std::string token;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (jpeg_.empty() || explain_url_.empty()) {
                return std::unexpected(std::string("No phone camera frame available"));
            }
            jpeg = jpeg_;
            url = explain_url_;
            token = explain_token_;
        }

        const std::string boundary = "----PHONE_AV_CAMERA_BOUNDARY";
        std::string body;
        body.reserve(jpeg.size() + question.size() + 512);
        body += "--";
        body += boundary;
        body += "\r\nContent-Disposition: form-data; name=\"question\"\r\n\r\n";
        body += question;
        body += "\r\n--";
        body += boundary;
        body += "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"camera.jpg\"\r\n";
        body += "Content-Type: image/jpeg\r\n\r\n";
        body.append(jpeg.data(), jpeg.size());
        body += "\r\n--";
        body += boundary;
        body += "--\r\n";

        auto network = Board::GetInstance().GetNetwork();
        auto http = network->CreateHttp(0);
        http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
        http->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
        if (!token.empty()) {
            http->SetHeader("Authorization", std::string("Bearer ") + token);
        }
        http->SetHeader("Content-Type", std::string("multipart/form-data; boundary=") + boundary);
        http->SetContent(std::move(body));
        if (auto opened = http->Open("POST", url); !opened) {
            return std::unexpected(std::string("Failed to connect to explain URL"));
        }
        auto status = http->GetStatusCode();
        if (!status || *status != 200) {
            http->Close();
            return std::unexpected(std::string("Failed to upload phone camera frame"));
        }
        std::string result = http->ReadAll();
        http->Close();
        return result;
    }

private:
    std::mutex state_mutex_;
    std::string jpeg_;
    std::string explain_url_;
    std::string explain_token_;
};

// The dev-board RGB (GPIO 48) is a standard GRB SK6812/WS2812. SingleLed drives
// it as GRB too, so SetColor(r,g,b) maps directly: (0,4,0) -> green, (0,0,4) ->
// blue, (4,0,0) -> red. The stock listening state also breathes/flickers with
// voice detection, which looks like a blinking light. Use a custom LED that stays
// solid and is off when idle.
class PhoneAvLed : public SingleLed {
public:
    explicit PhoneAvLed(gpio_num_t gpio) : SingleLed(gpio) {}

    void OnStateChanged() override {
        auto& app = Application::GetInstance();
        switch (app.GetDeviceState()) {
            case kDeviceStateIdle:
                TurnOff();
                break;
            case kDeviceStateListening:
            case kDeviceStateAudioTesting:
                // Solid green.
                SetColor(0, 4, 0);
                TurnOn();
                break;
            case kDeviceStateSpeaking:
            case kDeviceStateNotifying:
                SetColor(0, 0, 4);
                TurnOn();
                break;
            default:
                // starting / configuring / activating / upgrading / connecting:
                // keep the light off to avoid a distracting blink.
                TurnOff();
                break;
        }
    }
};

class PhoneAvBoard : public WifiBoard {
public:
    PhoneAvBoard() : boot_button_(BOOT_BUTTON_GPIO) {
        // Standard xiaozhi behaviour: a tap on the BOOT button enters WiFi
        // config mode while the device is still starting (so you can re-bind /
        // factory-reset from the captive portal at http://192.168.4.1), and
        // toggles the chat during normal operation. Without this the board has
        // no way to reach the setup page (port 80 is not otherwise reachable).
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
        boot_button_.OnLongPress(
            [this]() { Application::GetInstance().Schedule([this]() { EnterWifiConfigMode(); }); });

        // Keep the console at the stock log level. This board has no physical
        // display (Board::GetDisplay() returns NoDisplay), so xiaozhi prints the
        // activation screen -- including the 6-digit binding code -- to the UART
        // via Display::SetChatMessage()/Application::Alert(). Silencing the logs
        // would make the binding code impossible to read.

        // This board ships without a wake-word model (the assets partition is
        // disabled), so the default wake-word activation can never fire and the
        // device would sit idle forever. Instead, once the device is online and
        // activated, automatically start a hands-free conversation — no button
        // needed. ToggleChatState() uses the default AutoStop mode, so after each
        // reply the device returns to Listening and the chat stays alive. A slow
        // periodic timer re-arms listening whenever the device is back to Idle
        // (e.g. after a dropped connection), keeping the conversation going.
        constexpr uint64_t kAutoListenIntervalUs = 1000000;  // 1 s
        esp_timer_create_args_t auto_listen_timer_args = {
            .callback =
                [](void* arg) {
                    auto* board = static_cast<PhoneAvBoard*>(arg);
                    if (!board->IsNetworkReady()) {
                        return;
                    }
                    // Only start a conversation when the phone's microphone stream is
                    // actually open. If the phone app is closed (or the host is wrong)
                    // there is no audio device, so entering listening would immediately
                    // fail and the device would blink forever in an idle<->listening loop.
                    if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
                        auto* codec = static_cast<NetworkAudioCodec*>(board->GetAudioCodec());
                        if (codec != nullptr && codec->IsPhoneMicConnected()) {
                            // ManualStop keeps the conversation open across turns: the device
                            // does not auto-stop after a reply, so the server does not send
                            // "goodbye" and the audio channel stays open. The device therefore
                            // remains in the listening state (solid green) instead of dropping
                            // to idle -> connecting.
                            Application::GetInstance().SetListeningMode(kListeningModeManualStop);
                            Application::GetInstance().ToggleChatState();
                        }
                    }
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "auto_listen",
            .skip_unhandled_events = true,
        };
        esp_timer_create(&auto_listen_timer_args, &auto_listen_timer_);
        esp_timer_start_periodic(auto_listen_timer_, kAutoListenIntervalUs);
    }

    virtual void SetNetworkEventCallback(NetworkEventCallback callback) override {
        user_network_event_callback_ = std::move(callback);
        WifiBoard::SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
            network_ready_ = (event == NetworkEvent::Connected);
            if (event == NetworkEvent::Connected) {
                StartPhoneAvConfigServer();
            }
            if (user_network_event_callback_) {
                user_network_event_callback_(event, data);
            }
        });
    }

    bool IsNetworkReady() const {
        return network_ready_.load();
    }

    bool IsPhoneMicConnected() {
        auto* codec = static_cast<NetworkAudioCodec*>(GetAudioCodec());
        return codec != nullptr && codec->IsPhoneMicConnected();
    }
    uint32_t MicConnectCount() {
        auto* codec = static_cast<NetworkAudioCodec*>(GetAudioCodec());
        return codec != nullptr ? codec->MicConnectCount() : 0;
    }
    uint32_t MicDisconnectCount() {
        auto* codec = static_cast<NetworkAudioCodec*>(GetAudioCodec());
        return codec != nullptr ? codec->MicDisconnectCount() : 0;
    }
    uint32_t UptimeMs() const {
        return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    }

    virtual AudioCodec* GetAudioCodec() override {
        static NetworkAudioCodec codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE);
        return &codec;
    }

    virtual Camera* GetCamera() override {
        static NetworkCamera camera;
        return &camera;
    }

    virtual Display* GetDisplay() override {
        static PhoneAvDisplay display;
        return &display;
    }

    virtual Led* GetLed() override {
        static PhoneAvLed led(BUILTIN_LED_GPIO);
        return &led;
    }

private:
    std::atomic<bool> network_ready_{false};
    NetworkEventCallback user_network_event_callback_;
    esp_timer_handle_t auto_listen_timer_{nullptr};
    Button boot_button_;
};

bool PhoneAvNetworkReady() {
    return static_cast<PhoneAvBoard&>(Board::GetInstance()).IsNetworkReady();
}

esp_err_t PhoneAvStatusHandler(httpd_req_t* req) {
    auto& app = Application::GetInstance();
    DeviceState state = app.GetDeviceState();

    const char* state_str = "unknown";
    switch (state) {
        case kDeviceStateIdle:         state_str = "idle"; break;
        case kDeviceStateListening:    state_str = "listening"; break;
        case kDeviceStateSpeaking:     state_str = "speaking"; break;
        case kDeviceStateConnecting:   state_str = "connecting"; break;
        case kDeviceStateActivating:   state_str = "activating"; break;
        case kDeviceStateUpgrading:    state_str = "upgrading"; break;
        case kDeviceStateNotifying:    state_str = "notifying"; break;
        case kDeviceStateAudioTesting: state_str = "audio_testing"; break;
        default:                      state_str = "unknown"; break;
    }

    auto* board = static_cast<PhoneAvBoard*>(&Board::GetInstance());
    const bool mic = board->IsPhoneMicConnected();
    const bool net = board->IsNetworkReady();
    const bool voice = app.IsVoiceDetected();
    const uint32_t mc = board->MicConnectCount();
    const uint32_t md = board->MicDisconnectCount();
    const uint32_t up = board->UptimeMs();

    const char* rr = "unknown";
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   rr = "poweron"; break;
        case ESP_RST_SW:        rr = "software"; break;
        case ESP_RST_PANIC:     rr = "panic"; break;
        case ESP_RST_INT_WDT:   rr = "int_wdt"; break;
        case ESP_RST_TASK_WDT:  rr = "task_wdt"; break;
        case ESP_RST_WDT:       rr = "wdt"; break;
        case ESP_RST_DEEPSLEEP: rr = "deepsleep"; break;
        case ESP_RST_BROWNOUT:  rr = "brownout"; break;
        case ESP_RST_SDIO:      rr = "sdio"; break;
        default:                rr = "unknown"; break;
    }

    auto& screen = PhoneAvScreen();
    std::string screen_status, screen_emotion, screen_chat, screen_notice, screen_history;
    {
        std::lock_guard<std::mutex> lock(screen.mutex);
        screen_status = PhoneAvJsonEscape(screen.status);
        screen_emotion = PhoneAvJsonEscape(screen.emotion);
        screen_chat = PhoneAvJsonEscape(screen.chat_message);
        screen_notice = PhoneAvJsonEscape(screen.notification);
        screen_history = "[";
        for (size_t i = 0; i < screen.history.size(); ++i) {
            if (i != 0) {
                screen_history += ",";
            }
            screen_history += "\"" + PhoneAvJsonEscape(screen.history[i]) + "\"";
        }
        screen_history += "]";
    }

    std::string body = std::string("{\n") +
        "  \"state\": \"" + state_str + "\",\n" +
        "  \"voice_detected\": " + (voice ? "true" : "false") + ",\n" +
        "  \"phone_mic_connected\": " + (mic ? "true" : "false") + ",\n" +
        "  \"network_ready\": " + (net ? "true" : "false") + ",\n" +
        "  \"mic_connect_count\": " + std::to_string(mc) + ",\n" +
        "  \"mic_disconnect_count\": " + std::to_string(md) + ",\n" +
        "  \"uptime_ms\": " + std::to_string(up) + ",\n" +
        "  \"reset_reason\": \"" + rr + "\",\n" +
        "  \"screen_status\": \"" + screen_status + "\",\n" +
        "  \"screen_emotion\": \"" + screen_emotion + "\",\n" +
        "  \"screen_chat_message\": \"" + screen_chat + "\",\n" +
        "  \"screen_notification\": \"" + screen_notice + "\",\n" +
        "  \"screen_history\": " + screen_history + "\n" +
        "}";
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, body.c_str(), HTTPD_RESP_USE_STRLEN);
}

}  // namespace

DECLARE_BOARD(PhoneAvBoard);
