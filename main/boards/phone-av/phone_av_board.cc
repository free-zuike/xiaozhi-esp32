#include "wifi_board.h"
#include "audio_codec.h"
#include "camera.h"
#include "config.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <string>

/* Stage 1: board identity + AudioCodec / Camera wiring, so CI can build a
 * phone-av firmware. Stage 2 fills in the HTTP transport to the phone
 * (android-ip-camera: GET /audio/raw, POST /audio/upload, GET /video/snapshot).
 * GetDisplay() / GetLed() fall back to NoDisplay / NoLed from Board, which
 * avoids the SSD1306 init failure seen on bare ESP32-S3 boards.
 */

class NetworkAudioCodec : public AudioCodec {
public:
    NetworkAudioCodec(int input_sample_rate, int output_sample_rate) {
        duplex_ = true;
        input_reference_ = false;
        input_channels_ = 1;
        output_channels_ = 1;
        input_sample_rate_ = input_sample_rate;
        output_sample_rate_ = output_sample_rate;
    }
    virtual ~NetworkAudioCodec() = default;

protected:
    virtual int Read(int16_t* dest, int samples) override {
        if (dest == nullptr || samples <= 0) {
            return 0;
        }
        // TODO: pull 16-bit PCM from GET /audio/raw, resample 44.1k -> 16k.
        std::memset(dest, 0, static_cast<std::size_t>(samples) * sizeof(int16_t));
        return samples;
    }

    virtual int Write(const int16_t* data, int samples) override {
        if (data == nullptr || samples <= 0) {
            return 0;
        }
        // TODO: resample 24k -> 44.1k and POST to /audio/upload.
        return samples;
    }
};

class NetworkCamera : public Camera {
public:
    virtual void SetExplainUrl(const std::string& url, const std::string& token) override {
        explain_url_ = url;
        explain_token_ = token;
    }

    virtual bool Capture() override {
        // TODO: GET /video/snapshot and cache the latest JPEG.
        return false;
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
        (void)question;
        return std::unexpected(std::string("phone camera stream not connected yet"));
    }

private:
    std::string explain_url_;
    std::string explain_token_;
};

class PhoneAvBoard : public WifiBoard {
public:
    virtual AudioCodec* GetAudioCodec() override {
        static NetworkAudioCodec codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE);
        return &codec;
    }

    virtual Camera* GetCamera() override {
        static NetworkCamera camera;
        return &camera;
    }
};

DECLARE_BOARD(PhoneAvBoard);
