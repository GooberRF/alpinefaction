#pragma once

#include <windows.h>
#include <mmsystem.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// 16-bit interleaved PCM of one or two channels, as the sound browser plays it.
struct SoundPcm
{
    std::vector<std::int16_t> samples;
    int channels = 0;
    int sample_rate = 0;

    [[nodiscard]] std::size_t frames() const
    {
        return channels > 0 ? samples.size() / channels : 0;
    }
};

struct DecodedSound
{
    std::string format;
    int channels = 0;     // as stored, before any downmix
    int sample_rate = 0;
    int bits = 0;         // stored bits per sample, 0 for Ogg Vorbis
    double seconds = 0.0; // of the whole sound, also when only the start of it was decoded
    bool truncated = false;
    // Why the game or the preview cannot play the file; empty when both can
    std::string problem;
    // Null when the file cannot be previewed
    std::shared_ptr<const SoundPcm> pcm;
    // Min and max over every channel, one pair per waveform column
    std::vector<std::pair<std::int16_t, std::int16_t>> peaks;
};

// Decodes the image of a .wav or .ogg file. Returns early with no pcm once `cancelled` is set.
DecodedSound sound_decode(const std::string& name, const std::vector<std::uint8_t>& bytes,
                          const std::atomic<bool>& cancelled);

// Plays a SoundPcm through waveOut from a few short buffers that pump() refills, so volume, looping and seeking take
// effect within a fraction of a second.
class SoundPlayer
{
public:
    SoundPlayer() = default;
    SoundPlayer(const SoundPlayer&) = delete;
    SoundPlayer& operator=(const SoundPlayer&) = delete;
    ~SoundPlayer();

    bool play(std::shared_ptr<const SoundPcm> pcm, std::size_t start_frame);
    void stop();
    // Refills the buffers the device has finished with; false once playback has ended.
    bool pump();
    [[nodiscard]] bool playing() const
    {
        return device_ != nullptr;
    }
    // The frame being heard, or -1 when nothing is.
    [[nodiscard]] std::int64_t position() const;
    void set_loop(bool loop);
    void set_volume(float volume);

private:
    struct Buffer
    {
        WAVEHDR header{};
        std::vector<std::int16_t> data;
        std::size_t source_frame = 0;
        std::size_t frames = 0;
        DWORD device_frame = 0; // device sample count when this buffer starts playing
        bool queued = false;
    };

    void fill(Buffer& buffer);
    void release_buffers();

    static constexpr int buffer_count = 4;

    HWAVEOUT device_ = nullptr;
    std::shared_ptr<const SoundPcm> pcm_;
    std::array<Buffer, buffer_count> buffers_;
    int next_buffer_ = 0;
    std::size_t cursor_ = 0;
    DWORD queued_frames_ = 0;
    bool exhausted_ = false;
    bool loop_ = false;
    int gain_ = 256;
};
