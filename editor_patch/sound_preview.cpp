#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <msacm.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <stb_vorbis.h>
#include <xlog/xlog.h>
#include <common/scope_guard.h>
#include <common/utils/string-utils.h>
#include "sound_preview.h"
#include "vfs_browser.h"

namespace
{

constexpr int player_buffer_ms = 100;
// About 12 minutes of 44.1 kHz stereo; a longer sound previews only its start.
constexpr std::size_t max_decoded_samples = 64u * 1024 * 1024;
constexpr int max_sample_rate = 384000;
constexpr std::size_t waveform_columns = 2048;
constexpr int vorbis_chunk_frames = 4096;
constexpr std::size_t wav_format_min_size = 16;
constexpr std::size_t wav_format_ex_size = 18;

struct WavChunks
{
    const std::uint8_t* format = nullptr;
    std::size_t format_size = 0;
    const std::uint8_t* data = nullptr;
    std::size_t data_size = 0;
};

bool find_wav_chunks(const std::vector<std::uint8_t>& bytes, WavChunks& out)
{
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0
        || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        return false;
    }
    std::size_t pos = 12;
    while (pos <= bytes.size() && bytes.size() - pos >= 8) {
        const std::uint8_t* chunk = bytes.data() + pos;
        const auto len = vfs_browser_read_le<std::uint32_t>(chunk, 4);
        const std::size_t body = pos + 8;
        const std::size_t avail = bytes.size() - body;
        // A size running past the end of the file is cut there: writers often leave the data size wrong.
        const std::size_t size = std::min<std::size_t>(len, avail);
        if (!out.format && std::memcmp(chunk, "fmt ", 4) == 0) {
            out.format = bytes.data() + body;
            out.format_size = size;
        }
        else if (!out.data && std::memcmp(chunk, "data", 4) == 0) {
            out.data = bytes.data() + body;
            out.data_size = size;
        }
        if (len > avail) {
            break;
        }
        pos = body + len + (len & 1);
    }
    return out.format && out.data && out.format_size >= wav_format_min_size;
}

void decode_wav_pcm(const WavChunks& chunks, int channels, int bits, int sample_rate, DecodedSound& out)
{
    if (bits != 8 && bits != 16) {
        out.problem = "Only 8 and 16-bit PCM can be previewed";
        return;
    }
    const std::size_t frame_bytes = static_cast<std::size_t>(channels) * bits / 8;
    std::size_t frames = chunks.data_size / frame_bytes;
    out.seconds = static_cast<double>(frames) / sample_rate;
    if (frames > max_decoded_samples / channels) {
        frames = max_decoded_samples / channels;
        out.truncated = true;
    }
    auto pcm = std::make_shared<SoundPcm>();
    pcm->channels = channels;
    pcm->sample_rate = sample_rate;
    pcm->samples.resize(frames * channels);
    if (bits == 16) {
        std::memcpy(pcm->samples.data(), chunks.data, pcm->samples.size() * sizeof(std::int16_t));
    }
    else {
        for (std::size_t i = 0; i < pcm->samples.size(); ++i) {
            pcm->samples[i] = static_cast<std::int16_t>((static_cast<int>(chunks.data[i]) - 128) * 256);
        }
    }
    out.pcm = std::move(pcm);
}

// The game hands MS ADPCM to the system's ACM codec, so the preview does too.
void decode_wav_ms_adpcm(const WavChunks& chunks, int channels, int sample_rate, DecodedSound& out)
{
    const auto block_align = vfs_browser_read_le<WORD>(chunks.format, 12);
    if (chunks.format_size < wav_format_ex_size || block_align == 0) {
        out.problem = "Not a valid WAV file";
        return;
    }
    const std::size_t extra = vfs_browser_read_le<WORD>(chunks.format, 16);
    if (extra > chunks.format_size - wav_format_ex_size) {
        out.problem = "Not a valid WAV file";
        return;
    }
    std::vector<std::uint8_t> source_format(chunks.format, chunks.format + wav_format_ex_size + extra);
    WAVEFORMATEX pcm_format{};
    pcm_format.wFormatTag = WAVE_FORMAT_PCM;
    pcm_format.nChannels = static_cast<WORD>(channels);
    pcm_format.nSamplesPerSec = static_cast<DWORD>(sample_rate);
    pcm_format.wBitsPerSample = 16;
    pcm_format.nBlockAlign = static_cast<WORD>(channels * 2);
    pcm_format.nAvgBytesPerSec = pcm_format.nSamplesPerSec * pcm_format.nBlockAlign;

    HACMSTREAM stream = nullptr;
    if (acmStreamOpen(&stream, nullptr, reinterpret_cast<WAVEFORMATEX*>(source_format.data()), &pcm_format, nullptr,
                      0, 0, ACM_STREAMOPENF_NONREALTIME)
        != MMSYSERR_NOERROR) {
        out.problem = "No codec on this system decodes this sound";
        return;
    }
    ScopeGuard close_stream{[stream] { acmStreamClose(stream, 0); }};
    const std::size_t blocks = chunks.data_size / block_align;
    auto source_size = static_cast<DWORD>(blocks * block_align);
    DWORD pcm_size = 0;
    if (blocks == 0
        || acmStreamSize(stream, source_size, &pcm_size, ACM_STREAMSIZEF_SOURCE) != MMSYSERR_NOERROR) {
        out.problem = "Not a valid WAV file";
        return;
    }
    out.seconds = static_cast<double>(pcm_size / pcm_format.nBlockAlign) / sample_rate;
    if (pcm_size / sizeof(std::int16_t) > max_decoded_samples) {
        const std::size_t pcm_per_block = pcm_size / blocks;
        source_size = static_cast<DWORD>(max_decoded_samples * sizeof(std::int16_t) / pcm_per_block * block_align);
        if (acmStreamSize(stream, source_size, &pcm_size, ACM_STREAMSIZEF_SOURCE) != MMSYSERR_NOERROR) {
            out.problem = "Not a valid WAV file";
            return;
        }
        out.truncated = true;
    }

    auto pcm = std::make_shared<SoundPcm>();
    pcm->channels = channels;
    pcm->sample_rate = sample_rate;
    pcm->samples.resize(pcm_size / sizeof(std::int16_t));
    ACMSTREAMHEADER header{};
    header.cbStruct = sizeof(header);
    header.pbSrc = const_cast<LPBYTE>(chunks.data);
    header.cbSrcLength = source_size;
    header.pbDst = reinterpret_cast<LPBYTE>(pcm->samples.data());
    header.cbDstLength = pcm_size;
    if (acmStreamPrepareHeader(stream, &header, 0) != MMSYSERR_NOERROR) {
        out.problem = "No codec on this system decodes this sound";
        return;
    }
    const MMRESULT converted = acmStreamConvert(
        stream, &header, ACM_STREAMCONVERTF_BLOCKALIGN | ACM_STREAMCONVERTF_START | ACM_STREAMCONVERTF_END);
    const DWORD used = header.cbDstLengthUsed;
    acmStreamUnprepareHeader(stream, &header, 0);
    if (converted != MMSYSERR_NOERROR) {
        out.problem = "No codec on this system decodes this sound";
        return;
    }
    pcm->samples.resize(used / pcm_format.nBlockAlign * channels);
    out.pcm = std::move(pcm);
}

void decode_wav(const std::vector<std::uint8_t>& bytes, DecodedSound& out)
{
    WavChunks chunks;
    if (!find_wav_chunks(bytes, chunks)) {
        out.format = "WAV";
        out.problem = "Not a valid WAV file";
        return;
    }
    const auto tag = vfs_browser_read_le<WORD>(chunks.format, 0);
    out.channels = vfs_browser_read_le<WORD>(chunks.format, 2);
    out.sample_rate = static_cast<int>(vfs_browser_read_le<DWORD>(chunks.format, 4));
    out.bits = vfs_browser_read_le<WORD>(chunks.format, 14);
    if (tag == WAVE_FORMAT_PCM) {
        out.format = "WAV, PCM";
    }
    else if (tag == WAVE_FORMAT_ADPCM) {
        out.format = "WAV, MS ADPCM";
    }
    else {
        out.format = std::format("WAV, format tag {:#06x}", tag);
        out.problem = "The game cannot load this WAV format";
        return;
    }
    if (out.channels < 1 || out.sample_rate <= 0 || out.sample_rate > max_sample_rate) {
        out.problem = "Not a valid WAV file";
        return;
    }
    if (out.channels > 2) {
        out.problem = "Only mono and stereo sounds can be previewed";
        return;
    }
    if (tag == WAVE_FORMAT_PCM) {
        decode_wav_pcm(chunks, out.channels, out.bits, out.sample_rate, out);
    }
    else {
        decode_wav_ms_adpcm(chunks, out.channels, out.sample_rate, out);
    }
}

void decode_vorbis(const std::vector<std::uint8_t>& bytes, const std::atomic<bool>& cancelled, DecodedSound& out)
{
    out.format = "Ogg Vorbis";
    int error = 0;
    stb_vorbis* vorbis = stb_vorbis_open_memory(bytes.data(), static_cast<int>(bytes.size()), &error, nullptr);
    if (!vorbis) {
        out.problem = "Not a valid Ogg Vorbis file";
        return;
    }
    ScopeGuard close_vorbis{[vorbis] { stb_vorbis_close(vorbis); }};

    const stb_vorbis_info info = stb_vorbis_get_info(vorbis);
    out.channels = info.channels;
    out.sample_rate = static_cast<int>(info.sample_rate);
    if (out.channels < 1 || out.sample_rate <= 0 || out.sample_rate > max_sample_rate) {
        out.problem = "Not a valid Ogg Vorbis file";
        return;
    }

    const int channels = std::min(out.channels, 2);
    auto pcm = std::make_shared<SoundPcm>();
    pcm->channels = channels;
    pcm->sample_rate = out.sample_rate;
    std::vector<std::int16_t>& samples = pcm->samples;
    const std::size_t chunk_samples = static_cast<std::size_t>(vorbis_chunk_frames) * channels;
    while (!cancelled.load(std::memory_order_relaxed)) {
        const std::size_t old_size = samples.size();
        if (old_size + chunk_samples > max_decoded_samples) {
            out.truncated = true;
            break;
        }
        // Growth stops at the cap rather than overshooting it by half in one reallocation.
        if (samples.capacity() < old_size + chunk_samples) {
            samples.reserve(
                std::min(max_decoded_samples, std::max(samples.capacity() * 3 / 2, old_size + chunk_samples)));
        }
        samples.resize(old_size + chunk_samples);
        const int got = stb_vorbis_get_samples_short_interleaved(vorbis, channels, samples.data() + old_size,
                                                                 static_cast<int>(chunk_samples));
        samples.resize(old_size + static_cast<std::size_t>(std::max(got, 0)) * channels);
        if (got <= 0) {
            break;
        }
    }
    // Only a truncated decode needs the length the last page claims, which a damaged file can get wrong.
    const std::size_t decoded_frames = pcm->frames();
    const std::size_t total_frames =
        out.truncated ? std::max<std::size_t>(stb_vorbis_stream_length_in_samples(vorbis), decoded_frames)
                      : decoded_frames;
    out.seconds = static_cast<double>(total_frames) / out.sample_rate;
    out.pcm = std::move(pcm);
}

std::vector<std::pair<std::int16_t, std::int16_t>> compute_peaks(const SoundPcm& pcm)
{
    const std::uint64_t frames = pcm.frames();
    const std::uint64_t columns = std::min<std::uint64_t>(frames, waveform_columns);
    std::vector<std::pair<std::int16_t, std::int16_t>> peaks;
    peaks.reserve(static_cast<std::size_t>(columns));
    for (std::uint64_t column = 0; column < columns; ++column) {
        const std::size_t begin = static_cast<std::size_t>(column * frames / columns) * pcm.channels;
        const std::size_t end = static_cast<std::size_t>((column + 1) * frames / columns) * pcm.channels;
        std::int16_t low = 0;
        std::int16_t high = 0;
        for (std::size_t i = begin; i < end; ++i) {
            low = std::min(low, pcm.samples[i]);
            high = std::max(high, pcm.samples[i]);
        }
        peaks.emplace_back(low, high);
    }
    return peaks;
}

} // namespace

DecodedSound sound_decode(const std::string& name, const std::vector<std::uint8_t>& bytes,
                          const std::atomic<bool>& cancelled)
{
    DecodedSound out;
    if (string_iequals(get_ext_from_filename(name), "ogg")) {
        decode_vorbis(bytes, cancelled, out);
    }
    else if (!cancelled.load(std::memory_order_relaxed)) {
        decode_wav(bytes, out);
    }
    if (cancelled.load(std::memory_order_relaxed)) {
        out.pcm.reset();
        return out;
    }
    if (out.pcm && out.pcm->frames() == 0) {
        out.pcm.reset();
        if (out.problem.empty()) {
            out.problem = "The sound is empty";
        }
    }
    if (out.pcm) {
        out.peaks = compute_peaks(*out.pcm);
    }
    return out;
}

SoundPlayer::~SoundPlayer()
{
    stop();
}

bool SoundPlayer::play(std::shared_ptr<const SoundPcm> pcm, std::size_t start_frame)
{
    if (!pcm || pcm->frames() == 0 || pcm->channels < 1 || pcm->channels > 2 || pcm->sample_rate <= 0) {
        stop();
        return false;
    }
    if (device_ && pcm == pcm_) {
        // A seek: the device keeps its format, and a reset returns every buffer and zeroes its position.
        waveOutReset(device_);
        release_buffers();
    }
    else {
        stop();
        const std::size_t buffer_frames =
            std::max<std::size_t>(static_cast<std::size_t>(pcm->sample_rate) * player_buffer_ms / 1000, 256);
        for (Buffer& buffer : buffers_) {
            buffer.data.resize(buffer_frames * pcm->channels);
        }
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = static_cast<WORD>(pcm->channels);
        format.nSamplesPerSec = static_cast<DWORD>(pcm->sample_rate);
        format.wBitsPerSample = 16;
        format.nBlockAlign = static_cast<WORD>(pcm->channels * 2);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        const MMRESULT result = waveOutOpen(&device_, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL);
        if (result != MMSYSERR_NOERROR) {
            xlog::warn("[SoundBrowser] waveOutOpen failed ({})", result);
            device_ = nullptr;
            return false;
        }
        pcm_ = std::move(pcm);
    }
    const std::size_t frames = pcm_->frames();
    cursor_ = start_frame < frames ? start_frame : 0;
    next_buffer_ = 0;
    queued_frames_ = 0;
    exhausted_ = false;
    return pump();
}

void SoundPlayer::release_buffers()
{
    for (Buffer& buffer : buffers_) {
        if (buffer.queued) {
            waveOutUnprepareHeader(device_, &buffer.header, sizeof(buffer.header));
            buffer.queued = false;
        }
        buffer.frames = 0;
    }
}

void SoundPlayer::stop()
{
    if (device_) {
        waveOutReset(device_);
        release_buffers();
        waveOutClose(device_);
        device_ = nullptr;
    }
    pcm_.reset();
}

void SoundPlayer::fill(Buffer& buffer)
{
    const std::size_t total = pcm_->frames();
    const int channels = pcm_->channels;
    const std::size_t capacity = buffer.data.size() / channels;
    if (cursor_ >= total && loop_) {
        cursor_ = 0;
    }
    buffer.source_frame = cursor_;
    buffer.device_frame = queued_frames_;
    std::size_t filled = 0;
    while (filled < capacity) {
        if (cursor_ >= total) {
            if (!loop_) {
                break;
            }
            cursor_ = 0;
        }
        const std::size_t take = std::min(capacity - filled, total - cursor_);
        const std::int16_t* src = pcm_->samples.data() + cursor_ * channels;
        std::int16_t* dst = buffer.data.data() + filled * channels;
        for (std::size_t i = 0; i < take * channels; ++i) {
            dst[i] = static_cast<std::int16_t>(src[i] * gain_ / 256);
        }
        filled += take;
        cursor_ += take;
    }
    buffer.frames = filled;
    queued_frames_ += static_cast<DWORD>(filled);
}

bool SoundPlayer::pump()
{
    if (!device_) {
        return false;
    }
    for (Buffer& buffer : buffers_) {
        if (buffer.queued && (buffer.header.dwFlags & WHDR_DONE)) {
            waveOutUnprepareHeader(device_, &buffer.header, sizeof(buffer.header));
            buffer.queued = false;
        }
    }
    // The device plays buffers in the order they were written, so they are refilled in that order too.
    while (!exhausted_) {
        Buffer& buffer = buffers_[next_buffer_];
        if (buffer.queued) {
            break;
        }
        fill(buffer);
        if (buffer.frames == 0) {
            exhausted_ = true;
            break;
        }
        buffer.header = {};
        buffer.header.lpData = reinterpret_cast<LPSTR>(buffer.data.data());
        buffer.header.dwBufferLength = static_cast<DWORD>(buffer.frames * pcm_->channels * sizeof(std::int16_t));
        if (waveOutPrepareHeader(device_, &buffer.header, sizeof(buffer.header)) != MMSYSERR_NOERROR) {
            stop();
            return false;
        }
        if (waveOutWrite(device_, &buffer.header, sizeof(buffer.header)) != MMSYSERR_NOERROR) {
            waveOutUnprepareHeader(device_, &buffer.header, sizeof(buffer.header));
            stop();
            return false;
        }
        buffer.queued = true;
        next_buffer_ = (next_buffer_ + 1) % buffer_count;
    }
    const bool any_queued =
        std::any_of(buffers_.begin(), buffers_.end(), [](const Buffer& buffer) { return buffer.queued; });
    if (exhausted_ && !any_queued) {
        stop();
        return false;
    }
    return true;
}

std::int64_t SoundPlayer::position() const
{
    if (!device_ || !pcm_) {
        return -1;
    }
    MMTIME time{};
    time.wType = TIME_SAMPLES;
    if (waveOutGetPosition(device_, &time, sizeof(time)) != MMSYSERR_NOERROR) {
        return -1;
    }
    DWORD played = 0;
    if (time.wType == TIME_SAMPLES) {
        played = time.u.sample;
    }
    else if (time.wType == TIME_BYTES) {
        played = time.u.cb / (pcm_->channels * sizeof(std::int16_t));
    }
    else {
        return -1;
    }
    const std::size_t total = pcm_->frames();
    for (const Buffer& buffer : buffers_) {
        // Unsigned, so a buffer that starts after `played` is out of range too
        const DWORD offset = played - buffer.device_frame;
        if (buffer.frames > 0 && offset < buffer.frames) {
            return static_cast<std::int64_t>((buffer.source_frame + offset) % total);
        }
    }
    return -1;
}

void SoundPlayer::set_loop(bool loop)
{
    loop_ = loop;
    // Playback that already ran out of sound to queue picks up again from the start.
    if (loop && device_) {
        exhausted_ = false;
    }
}

void SoundPlayer::set_volume(float volume)
{
    gain_ = std::clamp(static_cast<int>(std::lround(volume * 256.0f)), 0, 256);
}
