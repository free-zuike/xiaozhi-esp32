#ifndef _PHONE_AV_AUDIO_H_
#define _PHONE_AV_AUDIO_H_

#include "audio_codec.h"
#include "config.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <driver/gpio.h>
#include <driver/i2s_std.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <soc/soc_caps.h>

#if SOC_I2S_SUPPORTS_PDM_RX
#include <driver/i2s_pdm.h>
#endif

/* Which side handles one capability. The value is persisted in NVS by the
 * board and read once at boot, so changing it requires a reboot. */
enum class PhoneAvSource {
    kPhone,
    kHardware,
};

/* These two live in phone_av_board.cc. The network audio backends need them,
 * but keeping the definitions in the board file avoids a second copy of the
 * NVS host lookup and the network-ready callback. */
std::string PhoneAvHost();
std::string PhoneAvUrl(const char* path);
bool PhoneAvNetworkReady();

/* Single-producer/single-consumer PCM ring. Pushing into a full buffer drops
 * the oldest samples, matching the previous phone-av implementation. */
class PcmRingBuffer {
public:
    explicit PcmRingBuffer(std::size_t capacity);

    void Push(const int16_t* data, std::size_t samples);
    std::size_t Pop(int16_t* dest, std::size_t samples);
    std::size_t PeekLatest(int16_t* dest, std::size_t samples) const;
    std::size_t Available() const;

private:
    std::vector<int16_t> buffer_;
    std::size_t capacity_;
    std::size_t read_pos_ = 0;
    std::size_t write_pos_ = 0;
    std::size_t count_ = 0;
    mutable std::mutex mutex_;
};

/* Phone microphone stream: HTTP GET /audio/pcm, raw 16 kHz mono PCM. */
class PhoneMicStream {
public:
    explicit PhoneMicStream(std::size_t capacity_samples = 16000);
    ~PhoneMicStream();

    void Start();
    bool Connected() const { return connected_.load(); }
    uint32_t ConnectCount() const { return connect_count_.load(); }
    uint32_t DisconnectCount() const { return disconnect_count_.load(); }
    std::size_t Pop(int16_t* dest, std::size_t samples) { return buffer_.Pop(dest, samples); }
    std::size_t PeekLatest(int16_t* dest, std::size_t samples) const {
        return buffer_.PeekLatest(dest, samples);
    }

private:
    static void TaskEntry(void* arg);
    void Task();
    void PushAudioBytes(const uint8_t* data, std::size_t len, bool& pending_valid,
                        uint8_t& pending);

    PcmRingBuffer buffer_;
    std::atomic<bool> running_{false};
    std::atomic<bool> started_{false};
    std::atomic<bool> connected_{false};
    std::atomic<uint32_t> connect_count_{0};
    std::atomic<uint32_t> disconnect_count_{0};
    int zero_read_count_ = 0;
    bool primed_ = false;
};

/* Phone speaker sink: batches PCM and POSTs it to /audio/upload/16000. */
class PhoneSpeakerSink {
public:
    explicit PhoneSpeakerSink(std::size_t capacity_samples = 32000);
    ~PhoneSpeakerSink();

    void Start();
    void Push(const int16_t* data, std::size_t samples) { buffer_.Push(data, samples); }
    uint32_t PostOk() const { return post_ok_.load(); }
    uint32_t PostFail() const { return post_fail_.load(); }
    uint32_t Samples() const { return samples_.load(); }

private:
    static void TaskEntry(void* arg);
    void Task();
    bool PostPcm(const std::string& url, const int16_t* data, std::size_t samples);

    static constexpr std::size_t kPostSamples = 16000;       // 1 s cap @ 16 kHz
    static constexpr std::size_t kPostFlushMs = 600;          // steady-state flush window
    static constexpr std::size_t kLowLatencyFlushMs = 120;    // first batch after idle

    PcmRingBuffer buffer_;
    // Use the size constructor, NOT braces: {16000} selects the
    // initializer_list<int16_t> overload and allocates one element, which used
    // to smash the heap when SpeakerTask wrote a full second of PCM.
    std::vector<int16_t> speak_chunk_ = std::vector<int16_t>(kPostSamples, 0);
    std::atomic<bool> running_{false};
    std::atomic<bool> started_{false};
    std::atomic<uint32_t> post_ok_{0};
    std::atomic<uint32_t> post_fail_{0};
    std::atomic<uint32_t> samples_{0};
};

/* Local I2S/PDM audio. Either direction can be absent; the phone-av board only
 * creates the hardware backend when at least one source is "hardware". */
class PhoneAvHwAudio {
public:
    PhoneAvHwAudio() = default;
    ~PhoneAvHwAudio();

    bool Init(bool enable_speaker, bool enable_mic);
    bool SpeakerReady() const { return tx_ != nullptr; }
    bool MicReady() const { return rx_ != nullptr; }

    void EnableSpeaker(bool enable);
    void EnableMic(bool enable);

    int Write(const int16_t* data, int samples, int volume);
    int Read(int16_t* dest, int samples, float gain);
    /* Enable RX for one short read, then restore the previous state. Used by
     * the idle auto-listen voice probe, when AudioService is not reading. */
    int ProbeMic(int16_t* dest, int samples);
    int32_t RawMin() const { return raw_min_.load(); }
    int32_t RawMax() const { return raw_max_.load(); }
    int32_t RawFirst() const { return raw_first_.load(); }

private:
    int ReadLocked(int16_t* dest, int samples, float gain);
    i2s_chan_handle_t tx_ = nullptr;
    i2s_chan_handle_t rx_ = nullptr;
    bool tx_enabled_ = false;
    bool rx_enabled_ = false;
    bool pdm_mic_ = false;
    std::vector<int32_t> tx_buf_;
    std::vector<int32_t> rx_buf_;
    std::mutex tx_mutex_;
    std::mutex rx_mutex_;
    std::atomic<int32_t> raw_min_{0};
    std::atomic<int32_t> raw_max_{0};
    std::atomic<int32_t> raw_first_{0};
};

/* The codec handed to AudioService. It combines one microphone backend with
 * one speaker backend, so the 8080 page can mix phone and local hardware. */
class PhoneAvAudioCodec : public AudioCodec {
public:
    PhoneAvAudioCodec();
    ~PhoneAvAudioCodec() override;

    void Start() override;
    void EnableInput(bool enable) override;
    void EnableOutput(bool enable) override;

    bool IsMicAvailable() const;
    bool IsPhoneMicConnected() const;
    uint32_t MicConnectCount() const;
    uint32_t MicDisconnectCount() const;
    uint32_t SpeakerPostOk() const;
    uint32_t SpeakerPostFail() const;
    uint32_t SpeakerSamples() const;

    /* Auto-listen gating: true when speech-like energy arrived recently. */
    bool VoiceDetectedRecently() const;
    /* Idle probe: read the latest mic samples directly. AudioService does not
     * read the codec while idle, so VoiceDetectedRecently() alone would never
     * arm. Returns true when this probe saw speech-like energy. */
    bool ProbeVoiceActivity();
    int ProbeRms() const { return probe_rms_.load(); }
    int ProbePeak() const { return probe_peak_.load(); }
    int ReadRms() const { return read_rms_.load(); }
    int ReadPeak() const { return read_peak_.load(); }
    int32_t RawMin() const { return hw_ ? hw_->RawMin() : 0; }
    int32_t RawMax() const { return hw_ ? hw_->RawMax() : 0; }
    int32_t RawFirst() const { return hw_ ? hw_->RawFirst() : 0; }
    /* True while the last playback is still recent (echo guard window). */
    bool IsPlaybackActive() const;
    /* When true the microphone is muted while playback is active (and for a
     * short tail), which prevents the assistant from hearing its own TTS.
     * Turning this off restores barge-in but can cause self-QA on a loud
     * speaker. */
    void SetMuteMicDuringPlayback(bool mute) { mute_mic_during_playback_ = mute; }
    bool MuteMicDuringPlayback() const { return mute_mic_during_playback_.load(); }

    PhoneAvSource SelectedMicSource() const { return selected_mic_; }
    PhoneAvSource SelectedSpkSource() const { return selected_spk_; }
    PhoneAvSource ActiveMicSource() const { return active_mic_; }
    PhoneAvSource ActiveSpkSource() const { return active_spk_; }
    bool HwMicActive() const { return active_mic_ == PhoneAvSource::kHardware && hw_mic_ready_; }
    bool HwSpkActive() const { return active_spk_ == PhoneAvSource::kHardware && hw_spk_ready_; }

protected:
    int Read(int16_t* dest, int samples) override;
    int Write(const int16_t* data, int samples) override;

private:
    static constexpr std::size_t kRefBufferSamples = 16000;  // 1 s @ 16 kHz
    static constexpr std::size_t kMicFrameBufferSamples = 2048;
    static constexpr std::size_t kProbeSamples = 960;  // 60 ms @ 16 kHz
    // Playback is "recent" for this long after the last Write(); it covers the
    // 600 ms phone POST batch, HTTP/phone latency and the TTS tail. The mic is
    // muted during this window by default, and the auto-listen voice detector
    // ignores it.
    static constexpr uint32_t kPlaybackGuardMs = 1500;
    // A voice burst keeps the auto-listen gate open for this long.
    static constexpr uint32_t kVoiceHoldMs = 700;

    void PacePhoneRead(int frames);
    static PhoneAvSource ReadSource(const char* key);

    PhoneAvSource selected_mic_ = PhoneAvSource::kPhone;
    PhoneAvSource selected_spk_ = PhoneAvSource::kPhone;
    PhoneAvSource active_mic_ = PhoneAvSource::kPhone;
    PhoneAvSource active_spk_ = PhoneAvSource::kPhone;

    bool hw_mic_ready_ = false;
    bool hw_spk_ready_ = false;

    std::unique_ptr<PhoneMicStream> phone_mic_;
    std::unique_ptr<PhoneSpeakerSink> phone_spk_;
    std::unique_ptr<PhoneAvHwAudio> hw_;

    PcmRingBuffer ref_buffer_;
    std::vector<int16_t> ref_frames_;
    std::vector<int16_t> mic_frames_;
    std::vector<int16_t> probe_frames_;
    TickType_t last_read_tick_ = 0;
    std::atomic<bool> started_{false};
    std::atomic<bool> mute_mic_during_playback_{true};
    std::atomic<TickType_t> last_output_tick_{0};
    std::atomic<TickType_t> last_voice_tick_{0};
    std::atomic<int> probe_rms_{0};
    std::atomic<int> probe_peak_{0};
    std::atomic<int> read_rms_{0};
    std::atomic<int> read_peak_{0};
};

#endif  // _PHONE_AV_AUDIO_H_
