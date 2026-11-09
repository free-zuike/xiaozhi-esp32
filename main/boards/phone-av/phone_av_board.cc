#include "wifi_board.h"
#include "audio_codec.h"
#include "camera.h"
#include "config.h"
#include "settings.h"
#include "system_info.h"

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
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "PhoneAvBoard"

namespace {

bool PhoneAvNetworkReady();

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
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR);
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
    config.max_uri_handlers = 4;
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

protected:
    virtual int Read(int16_t* dest, int samples) override {
        if (dest == nullptr || samples <= 0) {
            return 0;
        }
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

            pending_valid = false;
            while (running_ && PhoneAvNetworkReady()) {
                auto ret = http->Read(reinterpret_cast<char*>(raw), sizeof(raw));
                if (!ret) {
                    ESP_LOGW(TAG, "Phone microphone stream read failed");
                    break;
                }
                int n = *ret;
                if (n == 0) {
                    break;
                }
                PushAudioBytes(raw, static_cast<std::size_t>(n), pending_valid, pending);
            }
            http->Close();
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

class PhoneAvBoard : public WifiBoard {
public:
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

    virtual AudioCodec* GetAudioCodec() override {
        static NetworkAudioCodec codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE);
        return &codec;
    }

    virtual Camera* GetCamera() override {
        static NetworkCamera camera;
        return &camera;
    }

private:
    std::atomic<bool> network_ready_{false};
    NetworkEventCallback user_network_event_callback_;
};

bool PhoneAvNetworkReady() {
    return static_cast<PhoneAvBoard&>(Board::GetInstance()).IsNetworkReady();
}

}  // namespace

DECLARE_BOARD(PhoneAvBoard);
