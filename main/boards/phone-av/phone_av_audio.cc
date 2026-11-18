#include "phone_av_audio.h"

#include "application.h"
#include "board.h"
#include "settings.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_wifi.h>

#define TAG "PhoneAvAudio"

namespace {

constexpr std::size_t kHttpReadSize = 1024;
constexpr UBaseType_t kAudioTaskStack = 24576;

bool ValidPin(gpio_num_t pin) {
    return pin != GPIO_NUM_NC;
}

const char* SourceName(PhoneAvSource source) {
    return source == PhoneAvSource::kHardware ? "hardware" : "phone";
}

/* The Android app's VOICE_COMMUNICATION AudioRecord only delivers samples
 * after the app enters voice-call mode, which its TTS playback path does by
 * calling enterVoiceCallMode(). When the speaker is local, no TTS is sent to
 * the phone, so the mic stream stays silent. A short silent upload wakes the
 * talk route without making an audible sound. */
bool PrimePhoneTalkRoute() {
    if (!PhoneAvNetworkReady()) {
        return false;
    }
    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
    std::vector<int16_t> silence(320, 0);  // 20 ms @ 16 kHz
    std::string body(reinterpret_cast<const char*>(silence.data()),
                     silence.size() * sizeof(int16_t));
    http->SetHeader("Content-Type", "application/octet-stream");
    http->SetContent(std::move(body));
    if (auto opened = http->Open("POST", PhoneAvUrl(PHONE_AV_SPEAKER_PATH)); !opened) {
        http->Close();
        return false;
    }
    auto status = http->GetStatusCode();
    http->ReadAll();
    http->Close();
    ESP_LOGI(TAG, "Phone talk route prime: %d", status ? *status : -1);
    return status && *status == 200;
}

}  // namespace

PcmRingBuffer::PcmRingBuffer(std::size_t capacity)
    : buffer_(capacity), capacity_(capacity) {}

void PcmRingBuffer::Push(const int16_t* data, std::size_t samples) {
    if (data == nullptr || samples == 0) {
        return;
    }
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

std::size_t PcmRingBuffer::Pop(int16_t* dest, std::size_t samples) {
    if (dest == nullptr || samples == 0) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t count = std::min(samples, count_);
    for (std::size_t i = 0; i < count; ++i) {
        dest[i] = buffer_[read_pos_];
        read_pos_ = (read_pos_ + 1) % capacity_;
        --count_;
    }
    return count;
}

std::size_t PcmRingBuffer::PeekLatest(int16_t* dest, std::size_t samples) const {
    if (dest == nullptr || samples == 0) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t count = std::min(samples, count_);
    const std::size_t start = (write_pos_ + capacity_ - count) % capacity_;
    for (std::size_t i = 0; i < count; ++i) {
        dest[i] = buffer_[(start + i) % capacity_];
    }
    return count;
}

std::size_t PcmRingBuffer::Available() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
}

PhoneMicStream::PhoneMicStream(std::size_t capacity_samples) : buffer_(capacity_samples) {}

PhoneMicStream::~PhoneMicStream() {
    running_ = false;
}

void PhoneMicStream::Start() {
    if (started_.exchange(true)) {
        return;
    }
    running_ = true;
    BaseType_t result =
        xTaskCreate(TaskEntry, "phone_av_mic", kAudioTaskStack, this, 5, nullptr);
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create phone-av microphone task");
        running_ = false;
        started_ = false;
    }
}

void PhoneMicStream::TaskEntry(void* arg) {
    static_cast<PhoneMicStream*>(arg)->Task();
    vTaskDelete(nullptr);
}

void PhoneMicStream::PushAudioBytes(const uint8_t* data, std::size_t len,
                                    bool& pending_valid, uint8_t& pending) {
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
            buffer_.Push(samples, count);
            count = 0;
        }
    }
    if (count > 0) {
        buffer_.Push(samples, count);
    }
}

void PhoneMicStream::Task() {
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

        if (!primed_) {
            if (!PrimePhoneTalkRoute()) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            primed_ = true;
            // Let the app's player thread enter MODE_IN_COMMUNICATION before
            // opening /audio/pcm, so its AudioRecord is created in call mode.
            vTaskDelay(pdMS_TO_TICKS(1000));
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

        connected_ = true;
        connect_count_.fetch_add(1);
        pending_valid = false;
        zero_read_count_ = 0;
        // Keep the WiFi modem fully awake while the phone's mic stream is
        // active (modem sleep stalls TCP and used to cause reconnect storms).
        esp_wifi_set_ps(WIFI_PS_NONE);
        while (running_ && PhoneAvNetworkReady()) {
            auto ret = http->Read(reinterpret_cast<char*>(raw), sizeof(raw));
            if (!ret) {
                ESP_LOGW(TAG, "Phone microphone stream read failed");
                break;
            }
            int n = *ret;
            if (n == 0) {
                // A zero-length read is a transient gap or silence from the
                // phone, not necessarily a closed socket. Keep the connection
                // open and retry; only bail out after a very long idle.
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
        connected_ = false;
        primed_ = false;
        disconnect_count_.fetch_add(1);
        // If the phone mic just dropped while we were listening, leave the
        // chat session so the device returns to idle instead of sitting in a
        // dead "listening" state with no audio.
        if (Application::GetInstance().GetDeviceState() == kDeviceStateListening) {
            Application::GetInstance().StopListening();
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

PhoneSpeakerSink::PhoneSpeakerSink(std::size_t capacity_samples) : buffer_(capacity_samples) {}

PhoneSpeakerSink::~PhoneSpeakerSink() {
    running_ = false;
}

void PhoneSpeakerSink::Start() {
    if (started_.exchange(true)) {
        return;
    }
    running_ = true;
    BaseType_t result =
        xTaskCreate(TaskEntry, "phone_av_spk", kAudioTaskStack, this, 5, nullptr);
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create phone-av speaker task");
        running_ = false;
        started_ = false;
    }
}

void PhoneSpeakerSink::TaskEntry(void* arg) {
    static_cast<PhoneSpeakerSink*>(arg)->Task();
    vTaskDelete(nullptr);
}

bool PhoneSpeakerSink::PostPcm(const std::string& url, const int16_t* data,
                               std::size_t samples) {
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

void PhoneSpeakerSink::Task() {
    const std::string url = PhoneAvUrl(PHONE_AV_SPEAKER_PATH);
    while (running_) {
        if (!PhoneAvNetworkReady()) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        // Accumulate PCM before one POST. The decoded TTS arrives as 20 ms
        // frames; sending each frame separately means ~50 TCP handshakes per
        // second. Batching ~600 ms amortizes the per-request overhead so the
        // phone's AudioTrack can sustain realtime playback.
        const bool was_empty = buffer_.Available() == 0;
        const TickType_t flush_ms = was_empty ? kLowLatencyFlushMs : kPostFlushMs;
        std::size_t count = 0;
        const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(flush_ms);
        while (count < kPostSamples && running_) {
            count += buffer_.Pop(speak_chunk_.data() + count, kPostSamples - count);
            if (count >= kPostSamples || xTaskGetTickCount() >= deadline) {
                break;
            }
            // Must be >= one tick: with CONFIG_FREERTOS_HZ=100, pdMS_TO_TICKS(5)
            // rounds down to 0, so vTaskDelay(0) does not yield and this loop
            // would busy-wait and starve IDLE0.
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (count == 0) {
            continue;
        }
        // Retry once so a transient network hiccup does not punch a hole in
        // the playback.
        bool ok = PostPcm(url, speak_chunk_.data(), count);
        if (!ok) {
            vTaskDelay(pdMS_TO_TICKS(20));
            ok = PostPcm(url, speak_chunk_.data(), count);
        }
        if (ok) {
            post_ok_.fetch_add(1);
            samples_.fetch_add(static_cast<uint32_t>(count));
        } else {
            post_fail_.fetch_add(1);
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

PhoneAvHwAudio::~PhoneAvHwAudio() {
    if (tx_ != nullptr) {
        if (tx_enabled_) {
            i2s_channel_disable(tx_);
        }
        i2s_del_channel(tx_);
        tx_ = nullptr;
    }
    if (rx_ != nullptr) {
        if (rx_enabled_) {
            i2s_channel_disable(rx_);
        }
        i2s_del_channel(rx_);
        rx_ = nullptr;
    }
    if (ValidPin(PHONE_AV_HW_SPK_EN)) {
        gpio_set_level(PHONE_AV_HW_SPK_EN, 0);
    }
}

bool PhoneAvHwAudio::Init(bool enable_speaker, bool enable_mic) {
    pdm_mic_ = PHONE_AV_HW_MIC_PDM != 0;

    const bool want_spk =
        enable_speaker && ValidPin(PHONE_AV_HW_SPK_BCLK) &&
        ValidPin(PHONE_AV_HW_SPK_LRCK) && ValidPin(PHONE_AV_HW_SPK_DIN);
    const bool want_mic =
        enable_mic && ValidPin(PHONE_AV_HW_MIC_SCK) && ValidPin(PHONE_AV_HW_MIC_DIN) &&
        (pdm_mic_ || ValidPin(PHONE_AV_HW_MIC_WS));

    if (enable_speaker && !want_spk) {
        ESP_LOGE(TAG,
                 "Speaker hardware selected but PHONE_AV_HW_SPK_BCLK/LRCK/DIN are not configured");
    }
    if (enable_mic && !want_mic) {
        ESP_LOGE(TAG,
                 "Microphone hardware selected but PHONE_AV_HW_MIC_SCK/DIN%s are not configured",
                 pdm_mic_ ? "" : "/WS");
    }
    if (!want_spk && !want_mic) {
        return false;
    }

    if (want_spk && ValidPin(PHONE_AV_HW_SPK_EN)) {
        const gpio_num_t en_pin = PHONE_AV_HW_SPK_EN;
        // gpio_set_direction avoids building a pin mask from a macro that is
        // GPIO_NUM_NC by default (a constant negative shift would be a
        // compile error even inside a runtime-guarded branch).
        if (gpio_set_direction(en_pin, GPIO_MODE_OUTPUT) == ESP_OK) {
            gpio_set_level(en_pin, 0);
        } else {
            ESP_LOGW(TAG, "Failed to configure speaker enable GPIO");
        }
    }

    // Use separate controllers for TX and RX. PDM RX has to be on I2S0, so a
    // PDM mic and the speaker swap controllers; with only one direction the
    // unused controller id is irrelevant.
    int tx_id = 0;
    int rx_id = 1;
    if (want_mic && pdm_mic_) {
        rx_id = 0;
        tx_id = 1;
    }
    if (!want_spk) {
        rx_id = 0;
    }
    if (!want_mic) {
        tx_id = 0;
    }

    if (want_spk) {
        i2s_chan_config_t chan_cfg =
            I2S_CHANNEL_DEFAULT_CONFIG(XIAOZHI_I2S_PORT(tx_id), I2S_ROLE_MASTER);
        chan_cfg.dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM;
        chan_cfg.dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM;
        chan_cfg.auto_clear_after_cb = true;
        chan_cfg.auto_clear_before_cb = false;
        chan_cfg.intr_priority = 0;
        esp_err_t err = i2s_new_channel(&chan_cfg, &tx_, nullptr);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create I2S TX channel: %s", esp_err_to_name(err));
        } else {
            i2s_std_config_t std_cfg = {};
            std_cfg.clk_cfg.sample_rate_hz = static_cast<uint32_t>(AUDIO_OUTPUT_SAMPLE_RATE);
            std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
#if SOC_I2S_HW_VERSION_2
            std_cfg.clk_cfg.ext_clk_freq_hz = 0;
#endif
            std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
            std_cfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_32BIT;
            std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
            std_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_MONO;
            std_cfg.slot_cfg.slot_mask =
                PHONE_AV_HW_SPK_SLOT_RIGHT ? I2S_STD_SLOT_RIGHT : I2S_STD_SLOT_LEFT;
            std_cfg.slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_32BIT;
            std_cfg.slot_cfg.ws_pol = false;
            std_cfg.slot_cfg.bit_shift = true;
#if SOC_I2S_HW_VERSION_2
            std_cfg.slot_cfg.left_align = true;
            std_cfg.slot_cfg.big_endian = false;
            std_cfg.slot_cfg.bit_order_lsb = false;
#endif
            std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
            std_cfg.gpio_cfg.bclk = PHONE_AV_HW_SPK_BCLK;
            std_cfg.gpio_cfg.ws = PHONE_AV_HW_SPK_LRCK;
            std_cfg.gpio_cfg.dout = PHONE_AV_HW_SPK_DIN;
            std_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
            std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
            std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
            std_cfg.gpio_cfg.invert_flags.ws_inv = false;
            err = i2s_channel_init_std_mode(tx_, &std_cfg);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to init I2S TX: %s", esp_err_to_name(err));
                i2s_del_channel(tx_);
                tx_ = nullptr;
            }
        }
        if (tx_ != nullptr) {
            ESP_LOGI(TAG, "Local speaker ready (BCLK=%d LRCK=%d DIN=%d EN=%d)",
                     PHONE_AV_HW_SPK_BCLK, PHONE_AV_HW_SPK_LRCK, PHONE_AV_HW_SPK_DIN,
                     PHONE_AV_HW_SPK_EN);
        }
    }

    if (want_mic) {
        i2s_chan_config_t chan_cfg =
            I2S_CHANNEL_DEFAULT_CONFIG(XIAOZHI_I2S_PORT(rx_id), I2S_ROLE_MASTER);
        chan_cfg.dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM;
        chan_cfg.dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM;
        chan_cfg.auto_clear_after_cb = true;
        chan_cfg.auto_clear_before_cb = false;
        chan_cfg.intr_priority = 0;
        esp_err_t err = i2s_new_channel(&chan_cfg, nullptr, &rx_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create I2S RX channel: %s", esp_err_to_name(err));
        } else if (pdm_mic_) {
#if SOC_I2S_SUPPORTS_PDM_RX
            i2s_pdm_rx_config_t pdm_cfg = {
                .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(
                    static_cast<uint32_t>(AUDIO_INPUT_SAMPLE_RATE)),
                .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                           I2S_SLOT_MODE_MONO),
                .gpio_cfg = {
                    .clk = PHONE_AV_HW_MIC_SCK,
                    .din = PHONE_AV_HW_MIC_DIN,
                    .invert_flags = {
                        .clk_inv = false,
                    },
                },
            };
            err = i2s_channel_init_pdm_rx_mode(rx_, &pdm_cfg);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to init PDM RX: %s", esp_err_to_name(err));
                i2s_del_channel(rx_);
                rx_ = nullptr;
            }
#else
            ESP_LOGE(TAG, "PDM RX is not supported on this target");
            i2s_del_channel(rx_);
            rx_ = nullptr;
#endif
        } else {
            i2s_std_config_t std_cfg = {};
            std_cfg.clk_cfg.sample_rate_hz = static_cast<uint32_t>(AUDIO_INPUT_SAMPLE_RATE);
            std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
#if SOC_I2S_HW_VERSION_2
            std_cfg.clk_cfg.ext_clk_freq_hz = 0;
#endif
            std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
            std_cfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_32BIT;
            std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
            std_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_MONO;
            std_cfg.slot_cfg.slot_mask =
                PHONE_AV_HW_MIC_SLOT_RIGHT ? I2S_STD_SLOT_RIGHT : I2S_STD_SLOT_LEFT;
            std_cfg.slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_32BIT;
            std_cfg.slot_cfg.ws_pol = false;
            std_cfg.slot_cfg.bit_shift = true;
#if SOC_I2S_HW_VERSION_2
            std_cfg.slot_cfg.left_align = true;
            std_cfg.slot_cfg.big_endian = false;
            std_cfg.slot_cfg.bit_order_lsb = false;
#endif
            std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
            std_cfg.gpio_cfg.bclk = PHONE_AV_HW_MIC_SCK;
            std_cfg.gpio_cfg.ws = PHONE_AV_HW_MIC_WS;
            std_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
            std_cfg.gpio_cfg.din = PHONE_AV_HW_MIC_DIN;
            std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
            std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
            std_cfg.gpio_cfg.invert_flags.ws_inv = false;
            err = i2s_channel_init_std_mode(rx_, &std_cfg);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to init I2S RX: %s", esp_err_to_name(err));
                i2s_del_channel(rx_);
                rx_ = nullptr;
            }
        }
        if (rx_ != nullptr) {
            ESP_LOGI(TAG, "Local microphone ready (%s SCK=%d WS=%d DIN=%d)",
                     pdm_mic_ ? "PDM" : "I2S", PHONE_AV_HW_MIC_SCK, PHONE_AV_HW_MIC_WS,
                     PHONE_AV_HW_MIC_DIN);
        }
    }

    return tx_ != nullptr || rx_ != nullptr;
}

void PhoneAvHwAudio::EnableSpeaker(bool enable) {
    std::lock_guard<std::mutex> lock(tx_mutex_);
    if (tx_ == nullptr || enable == tx_enabled_) {
        return;
    }
    if (enable) {
        if (ValidPin(PHONE_AV_HW_SPK_EN)) {
            gpio_set_level(PHONE_AV_HW_SPK_EN, 1);
        }
        esp_err_t err = i2s_channel_enable(tx_);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to enable I2S TX: %s", esp_err_to_name(err));
            if (ValidPin(PHONE_AV_HW_SPK_EN)) {
                gpio_set_level(PHONE_AV_HW_SPK_EN, 0);
            }
            return;
        }
        tx_enabled_ = true;
    } else {
        esp_err_t err = i2s_channel_disable(tx_);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to disable I2S TX: %s", esp_err_to_name(err));
        }
        tx_enabled_ = false;
        if (ValidPin(PHONE_AV_HW_SPK_EN)) {
            gpio_set_level(PHONE_AV_HW_SPK_EN, 0);
        }
    }
}

void PhoneAvHwAudio::EnableMic(bool enable) {
    std::lock_guard<std::mutex> lock(rx_mutex_);
    if (rx_ == nullptr || enable == rx_enabled_) {
        return;
    }
    esp_err_t err = enable ? i2s_channel_enable(rx_) : i2s_channel_disable(rx_);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to %s I2S RX: %s", enable ? "enable" : "disable",
                 esp_err_to_name(err));
        return;
    }
    rx_enabled_ = enable;
}

int PhoneAvHwAudio::Write(const int16_t* data, int samples, int volume) {
    std::lock_guard<std::mutex> lock(tx_mutex_);
    if (tx_ == nullptr || !tx_enabled_ || data == nullptr || samples <= 0) {
        return 0;
    }
    if (tx_buf_.size() < static_cast<std::size_t>(samples)) {
        tx_buf_.resize(static_cast<std::size_t>(samples));
    }
    const int clamped_volume = std::clamp(volume, 0, 100);
    const int32_t volume_factor =
        static_cast<int32_t>(std::pow(static_cast<double>(clamped_volume) / 100.0, 2.0) *
                             65536.0);
    for (int i = 0; i < samples; ++i) {
        int64_t value = static_cast<int64_t>(data[i]) * volume_factor;
        if (value > INT32_MAX) {
            tx_buf_[i] = INT32_MAX;
        } else if (value < INT32_MIN) {
            tx_buf_[i] = INT32_MIN;
        } else {
            tx_buf_[i] = static_cast<int32_t>(value);
        }
    }
    size_t bytes_written = 0;
    esp_err_t err = i2s_channel_write(
        tx_, tx_buf_.data(), static_cast<size_t>(samples) * sizeof(int32_t), &bytes_written,
        portMAX_DELAY);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2S TX write failed: %s", esp_err_to_name(err));
        return 0;
    }
    return static_cast<int>(bytes_written / sizeof(int32_t));
}

int PhoneAvHwAudio::Read(int16_t* dest, int samples, float gain) {
    std::lock_guard<std::mutex> lock(rx_mutex_);
    if (rx_ == nullptr || !rx_enabled_ || dest == nullptr || samples <= 0) {
        return 0;
    }
    return ReadLocked(dest, samples, gain);
}

int PhoneAvHwAudio::ReadLocked(int16_t* dest, int samples, float gain) {
    size_t bytes_read = 0;
    if (pdm_mic_) {
        esp_err_t err = i2s_channel_read(rx_, dest, static_cast<size_t>(samples) * sizeof(int16_t),
                                         &bytes_read, pdMS_TO_TICKS(200));
        if (err != ESP_OK) {
            return 0;
        }
        int n = static_cast<int>(bytes_read / sizeof(int16_t));
        if (gain > 0.0f) {
            for (int i = 0; i < n; ++i) {
                int32_t value = static_cast<int32_t>(dest[i] * gain);
                dest[i] = static_cast<int16_t>(std::clamp(value, (int32_t)INT16_MIN,
                                                          (int32_t)INT16_MAX));
            }
        }
        return n;
    }

    if (rx_buf_.size() < static_cast<std::size_t>(samples)) {
        rx_buf_.resize(static_cast<std::size_t>(samples));
    }
    esp_err_t err = i2s_channel_read(rx_, rx_buf_.data(),
                                     static_cast<size_t>(samples) * sizeof(int32_t),
                                     &bytes_read, pdMS_TO_TICKS(200));
    if (err != ESP_OK) {
        return 0;
    }
    int n = static_cast<int>(bytes_read / sizeof(int32_t));
    if (n > 0) {
        int32_t minimum = INT32_MAX;
        int32_t maximum = INT32_MIN;
        for (int i = 0; i < n; ++i) {
            if (rx_buf_[i] < minimum) {
                minimum = rx_buf_[i];
            }
            if (rx_buf_[i] > maximum) {
                maximum = rx_buf_[i];
            }
        }
        raw_min_.store(minimum);
        raw_max_.store(maximum);
        raw_first_.store(rx_buf_[0]);
    }
    for (int i = 0; i < n; ++i) {
        int32_t raw = rx_buf_[i] >> PHONE_AV_HW_MIC_SHIFT;
        // First-order DC blocker (~16 Hz at 16 kHz), then gain. The INMP441
        // module on this build has a noticeable DC offset; without this the
        // AFE sees a constant offset and a very low speech level.
        int32_t blocked = raw - dc_prev_in_ +
                          static_cast<int32_t>((static_cast<int64_t>(dc_prev_out_) * 255) / 256);
        dc_prev_in_ = raw;
        dc_prev_out_ = blocked;
        int32_t value = static_cast<int32_t>(blocked * PHONE_AV_HW_MIC_GAIN);
        if (gain > 0.0f) {
            value = static_cast<int32_t>(value * gain);
        }
        dest[i] = static_cast<int16_t>(std::clamp(value, (int32_t)INT16_MIN, (int32_t)INT16_MAX));
    }
    return n;
}

int PhoneAvHwAudio::ProbeMic(int16_t* dest, int samples) {
    std::lock_guard<std::mutex> lock(rx_mutex_);
    if (rx_ == nullptr || dest == nullptr || samples <= 0) {
        return 0;
    }
    const bool was_enabled = rx_enabled_;
    if (!was_enabled) {
        if (i2s_channel_enable(rx_) != ESP_OK) {
            return 0;
        }
        rx_enabled_ = true;
        // Give the mic and I2S DMA a few frames to start producing data;
        // reading immediately after enable returned zero samples and kept the
        // idle auto-listen gate from ever arming.
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    const int n = ReadLocked(dest, samples, 0.0f);
    if (n == 0) {
        ESP_LOGW(TAG, "ProbeMic returned 0 samples (was_enabled=%d)", was_enabled ? 1 : 0);
    }
    if (!was_enabled) {
        i2s_channel_disable(rx_);
        rx_enabled_ = false;
    }
    return n;
}

PhoneAvAudioCodec::PhoneAvAudioCodec()
    : ref_buffer_(kRefBufferSamples),
      ref_frames_(kMicFrameBufferSamples),
      mic_frames_(kMicFrameBufferSamples),
      probe_frames_(kProbeSamples) {
    duplex_ = true;
    input_reference_ = true;
    input_channels_ = 2;
    output_channels_ = 1;
    input_sample_rate_ = AUDIO_INPUT_SAMPLE_RATE;
    output_sample_rate_ = AUDIO_OUTPUT_SAMPLE_RATE;

    selected_mic_ = ReadSource("src_mic");
    selected_spk_ = ReadSource("src_spk");
    active_mic_ = selected_mic_;
    active_spk_ = selected_spk_;

    if (selected_mic_ == PhoneAvSource::kHardware ||
        selected_spk_ == PhoneAvSource::kHardware) {
        hw_ = std::make_unique<PhoneAvHwAudio>();
        hw_->Init(selected_spk_ == PhoneAvSource::kHardware,
                  selected_mic_ == PhoneAvSource::kHardware);
        hw_spk_ready_ = hw_->SpeakerReady();
        hw_mic_ready_ = hw_->MicReady();
        if (selected_spk_ == PhoneAvSource::kHardware && !hw_spk_ready_) {
            ESP_LOGE(TAG, "Local speaker unavailable, falling back to the phone");
            active_spk_ = PhoneAvSource::kPhone;
        }
        if (selected_mic_ == PhoneAvSource::kHardware && !hw_mic_ready_) {
            ESP_LOGE(TAG, "Local microphone unavailable, falling back to the phone");
            active_mic_ = PhoneAvSource::kPhone;
        }
        if (!hw_spk_ready_ && !hw_mic_ready_) {
            hw_.reset();
        }
    }

    if (active_mic_ == PhoneAvSource::kPhone) {
        phone_mic_ = std::make_unique<PhoneMicStream>();
    }
    if (active_spk_ == PhoneAvSource::kPhone) {
        phone_spk_ = std::make_unique<PhoneSpeakerSink>();
    }

    // The phone path historically reported duplex=true so the power manager
    // keeps the (silent) local output enabled while listening. A real I2S amp
    // should instead be turned off after playback goes idle, otherwise its EN
    // pin stays high and the amp draws idle current; separate TX/RX I2S
    // channels do not need duplex=true for correctness.
    duplex_ = active_spk_ != PhoneAvSource::kHardware;

    ESP_LOGI(TAG, "Audio sources: mic=%s spk=%s (selected mic=%s spk=%s)",
             SourceName(active_mic_), SourceName(active_spk_), SourceName(selected_mic_),
             SourceName(selected_spk_));
}

PhoneAvAudioCodec::~PhoneAvAudioCodec() = default;

PhoneAvSource PhoneAvAudioCodec::ReadSource(const char* key) {
    const char* fallback =
        std::strcmp(key, "src_mic") == 0 ? PHONE_AV_DEFAULT_SRC_MIC
                                         : PHONE_AV_DEFAULT_SRC_SPK;
    Settings settings("phone_av", false);
    return settings.GetString(key, fallback) == "hw" ? PhoneAvSource::kHardware
                                                    : PhoneAvSource::kPhone;
}

void PhoneAvAudioCodec::Start() {
    AudioCodec::Start();
    if (started_.exchange(true)) {
        return;
    }
    if (phone_mic_) {
        phone_mic_->Start();
    }
    if (phone_spk_) {
        phone_spk_->Start();
    }
    if (active_mic_ == PhoneAvSource::kHardware && hw_) {
        // Keep the hardware mic clocked while idle. The INMP441 needs a
        // stable clock for longer than one probe window; toggling RX every
        // second made the idle voice probe read zeros.
        hw_->EnableMic(true);
    }
    ESP_LOGI(TAG, "phone-av audio started");
}

void PhoneAvAudioCodec::EnableInput(bool enable) {
    if (enable == input_enabled_) {
        return;
    }
    if (active_mic_ == PhoneAvSource::kHardware && hw_) {
        if (enable) {
            hw_->EnableMic(true);
        }
        // Do not disable RX on false: the idle auto-listen probe needs it
        // clocked. The base input_enabled_ flag still stops AudioService reads.
    }
    AudioCodec::EnableInput(enable);
}

void PhoneAvAudioCodec::EnableOutput(bool enable) {
    if (enable == output_enabled_) {
        return;
    }
    if (active_spk_ == PhoneAvSource::kHardware && hw_) {
        hw_->EnableSpeaker(enable);
    }
    AudioCodec::EnableOutput(enable);
}

void PhoneAvAudioCodec::PacePhoneRead(int frames) {
    const TickType_t now = xTaskGetTickCount();
    if (last_read_tick_ != 0) {
        const TickType_t expected =
            last_read_tick_ + pdMS_TO_TICKS(frames * 1000 / input_sample_rate_);
        if (static_cast<int32_t>(expected - now) > 0) {
            vTaskDelay(expected - now);
        }
    }
    last_read_tick_ = xTaskGetTickCount();
}

int PhoneAvAudioCodec::Read(int16_t* dest, int samples) {
    if (dest == nullptr || samples <= 0) {
        return 0;
    }
    const int frames = samples / input_channels_;
    if (frames <= 0) {
        return 0;
    }
    if (static_cast<int>(mic_frames_.size()) < frames) {
        mic_frames_.resize(static_cast<std::size_t>(frames));
    }
    std::memset(mic_frames_.data(), 0, static_cast<std::size_t>(frames) * sizeof(int16_t));

    if (active_mic_ == PhoneAvSource::kPhone && phone_mic_) {
        PacePhoneRead(frames);
        phone_mic_->Pop(mic_frames_.data(), static_cast<std::size_t>(frames));
    } else if (active_mic_ == PhoneAvSource::kHardware && hw_) {
        hw_->Read(mic_frames_.data(), frames, input_gain_);
    }

    {
        int64_t read_energy = 0;
        int read_peak = 0;
        for (int f = 0; f < frames; ++f) {
            const int32_t value = mic_frames_[f];
            read_energy += static_cast<int64_t>(value) * value;
            const int32_t absolute = value < 0 ? -value : value;
            if (absolute > read_peak) {
                read_peak = absolute;
            }
        }
        read_rms_.store(static_cast<int>(std::sqrt(static_cast<double>(read_energy) / frames)));
        read_peak_.store(read_peak);
    }

    const TickType_t now = xTaskGetTickCount();
    const TickType_t last_out = last_output_tick_.load();
    const bool playback_recent =
        last_out != 0 && (now - last_out) <= pdMS_TO_TICKS(kPlaybackGuardMs);
    if (playback_recent) {
        // The phone/amp is still playing (plus a short tail). By default the
        // mic is muted so the server cannot hear the assistant's own TTS and
        // answer itself. Barge-in can be enabled on the 8080 page.
        if (mute_mic_during_playback_.load()) {
            std::memset(mic_frames_.data(), 0,
                        static_cast<std::size_t>(frames) * sizeof(int16_t));
        }
        // Never let TTS echo arm the auto-listen gate.
    } else {
        int64_t energy = 0;
        int peak = 0;
        for (int f = 0; f < frames; ++f) {
            const int32_t value = mic_frames_[f];
            energy += static_cast<int64_t>(value) * value;
            const int32_t absolute = value < 0 ? -value : value;
            if (absolute > peak) {
                peak = absolute;
            }
        }
        const int rms =
            frames > 0
                ? static_cast<int>(std::sqrt(static_cast<double>(energy) / frames))
                : 0;
        if (rms >= PHONE_AV_VOICE_RMS_THRESHOLD ||
            peak >= PHONE_AV_VOICE_PEAK_THRESHOLD) {
            last_voice_tick_.store(now);
        }
    }

    if (static_cast<int>(ref_frames_.size()) < frames) {
        ref_frames_.resize(static_cast<std::size_t>(frames));
    }
    std::memset(ref_frames_.data(), 0, static_cast<std::size_t>(frames) * sizeof(int16_t));
    ref_buffer_.Pop(ref_frames_.data(), static_cast<std::size_t>(frames));

    int out = 0;
    for (int f = 0; f < frames; ++f) {
        dest[out++] = mic_frames_[f];
        dest[out++] = ref_frames_[f];
    }
    return samples;
}

int PhoneAvAudioCodec::Write(const int16_t* data, int samples) {
    if (data == nullptr || samples <= 0) {
        return 0;
    }
    last_output_tick_.store(xTaskGetTickCount());
    ref_buffer_.Push(data, static_cast<std::size_t>(samples));
    if (active_spk_ == PhoneAvSource::kPhone && phone_spk_) {
        phone_spk_->Push(data, static_cast<std::size_t>(samples));
    } else if (active_spk_ == PhoneAvSource::kHardware && hw_) {
        hw_->Write(data, samples, output_volume_);
    }
    return samples;
}

bool PhoneAvAudioCodec::IsMicAvailable() const {
    if (active_mic_ == PhoneAvSource::kHardware) {
        return hw_mic_ready_;
    }
    return phone_mic_ && phone_mic_->Connected();
}

bool PhoneAvAudioCodec::IsPlaybackActive() const {
    const TickType_t last = last_output_tick_.load();
    if (last == 0) {
        return false;
    }
    return (xTaskGetTickCount() - last) <= pdMS_TO_TICKS(kPlaybackGuardMs);
}

bool PhoneAvAudioCodec::VoiceDetectedRecently() const {
    const TickType_t last = last_voice_tick_.load();
    if (last == 0) {
        return false;
    }
    return (xTaskGetTickCount() - last) <= pdMS_TO_TICKS(kVoiceHoldMs);
}

bool PhoneAvAudioCodec::ProbeVoiceActivity() {
    const TickType_t now = xTaskGetTickCount();
    const TickType_t last_out = last_output_tick_.load();
    if (last_out != 0 && (now - last_out) <= pdMS_TO_TICKS(kPlaybackGuardMs)) {
        return false;
    }

    std::size_t got = 0;
    if (active_mic_ == PhoneAvSource::kPhone && phone_mic_) {
        got = phone_mic_->PeekLatest(probe_frames_.data(), kProbeSamples);
    } else if (active_mic_ == PhoneAvSource::kHardware && hw_) {
        const int n = hw_->ProbeMic(probe_frames_.data(), static_cast<int>(kProbeSamples));
        got = n > 0 ? static_cast<std::size_t>(n) : 0;
    }
    if (got == 0) {
        return false;
    }

    int64_t energy = 0;
    int peak = 0;
    for (std::size_t i = 0; i < got; ++i) {
        const int32_t value = probe_frames_[i];
        energy += static_cast<int64_t>(value) * value;
        const int32_t absolute = value < 0 ? -value : value;
        if (absolute > peak) {
            peak = absolute;
        }
    }
    const int rms = static_cast<int>(std::sqrt(static_cast<double>(energy) / got));
    probe_rms_.store(rms);
    probe_peak_.store(peak);
    if (rms >= PHONE_AV_VOICE_RMS_THRESHOLD || peak >= PHONE_AV_VOICE_PEAK_THRESHOLD) {
        last_voice_tick_.store(now);
        return true;
    }
    return false;
}

bool PhoneAvAudioCodec::IsPhoneMicConnected() const {
    return phone_mic_ && phone_mic_->Connected();
}

uint32_t PhoneAvAudioCodec::MicConnectCount() const {
    return phone_mic_ ? phone_mic_->ConnectCount() : 0;
}

uint32_t PhoneAvAudioCodec::MicDisconnectCount() const {
    return phone_mic_ ? phone_mic_->DisconnectCount() : 0;
}

uint32_t PhoneAvAudioCodec::SpeakerPostOk() const {
    return phone_spk_ ? phone_spk_->PostOk() : 0;
}

uint32_t PhoneAvAudioCodec::SpeakerPostFail() const {
    return phone_spk_ ? phone_spk_->PostFail() : 0;
}

uint32_t PhoneAvAudioCodec::SpeakerSamples() const {
    return phone_spk_ ? phone_spk_->Samples() : 0;
}
