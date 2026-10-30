#include <patch_common/FunHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/ShortTypes.h>
#include <patch_common/StaticBufferResizePatch.h>
#include <patch_common/AsmOpcodes.h>
#include <stb_vorbis.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include "sound.h"
#include "sound_stretch.h"
#include "../rf/sound/sound.h"
#include "../rf/sound/sound_ds.h"
#include "../rf/crt.h"
#include "../rf/file/file.h"
#include "../os/console.h"

namespace rf
{
    rf::DsChannel ds_channels[rf::num_sound_channels];
}

StaticBufferResizePatch<rf::DsChannel> ds_channels_resize_patch{
    0x01AD7520,
    30,
    rf::ds_channels,
    {
        {0x00521149},
        {0x005211C3},
        {0x0052165D},
        {0x00522306},
        {0x005224DA},
        {0x0052250D},
        {0x005225A3},
        {0x005225B7},
        {0x005225F2},
        {0x00522608},
        {0x00522632},
        {0x0052265A},
        {0x00522734},
        {0x00522741},
        {0x00522763},
        {0x005227AD},
        {0x0052282F},
        {0x00522A61},
        {0x00522AAD},
        {0x00522B3A},
        {0x00522BDA},
        {0x00522C8E},
        {0x00522D4B},
        {0x00522D66},
        {0x00522D98},
        {0x00522DDF},
        {0x00522E1B},
        {0x00522F0B},
        {0x0052312B},
        {0x0054E8CC},
        {0x0054E956},
        {0x0054EAB5},
        {0x0054EB64},
        {0x0054EBB6},
        {0x0054EC93},
        {0x005211F5},
        {0x005225BD},
        {0x00562DC9},
        {0x0052264C},
        {0x00522781},
        {0x00522792},
        {0x00505E96},
        {0x00522688},
        {0x005226A0},
        {0x005227D8},
        {0x005227F0},
        {0x00522CFE},
        {0x00522F3A},
        {0x00522626},
        {0x0052275D},
        {0x00522D75},
        {0x0052262C},
        {0x0052271E},
        {0x00521945},
        {0x00522478},
        {0x0052251A},
        {0x005225E4},
        {0x005225EC},
        {0x00522724},
        {0x0052272E},
        {0x00522A57},
        {0x00522AA3},
        {0x00522AF4},
        {0x00522BE6},
        {0x00522BEE},
        {0x00522C04},
        {0x00522C57},
        {0x00522C80},
        {0x00522C9F},
        {0x00522CA7},
        {0x00522CB4},
        {0x0052194E},
        {0x00522F47, true},
        {0x005224A7, true},
        {0x00522B0B, true},
        {0x00522C29, true},
        {0x00522CCB, true},
    },
};

// Global playback rate (snd_ds_set_global_rate): sounds are time-stretched to it within 0.5 to 2 so they keep their
// pitch, and resampled (pitched) for the rest of it. A channel plays at its own pitch ratio (snd_instance_set_pitch)
// times what its buffer's stretch leaves of the rate, unless it is music or was played unscaled.
static float g_ds_rate = 1.0f;
static float g_ds_stretch = 1.0f;
static float g_ds_channel_pitch[std::size(rf::ds_channels)];
static float g_ds_channel_stretch[std::size(rf::ds_channels)];
static bool g_ds_channel_unscaled[std::size(rf::ds_channels)];
// The play call in progress: set by ds_get_free_channel_new and around snd_play_unscaled
static bool g_ds_new_channel_looping = false;
static bool g_ds_new_channel_unscaled = false;

static int ds_get_free_channel_new(int sid, float volume, bool is_looping)
{
    g_ds_new_channel_looping = is_looping;
    float duration = rf::snd_ds_estimate_duration(sid);
    int max_channels = static_cast<int>(std::size(rf::ds_channels));
    if (!is_looping && duration < 10.0f) {
        float effects_volume = rf::snd_group_volume[rf::SOUND_GROUP_EFFECTS];
        float normalized_volume = effects_volume > 0.001f ? volume / effects_volume : 0.0f;
        if (normalized_volume < 0.1f) {
            xlog::trace("sound {} channel priority 1", sid);
            max_channels = max_channels / 4;
        }
        else if (normalized_volume < 0.2f) {
            xlog::trace("sound {} channel priority 2", sid);
            max_channels = max_channels * 2 / 4;
        }
        else if (normalized_volume < 0.5f) {
            xlog::trace("sound {} channel priority 3", sid);
            max_channels = max_channels * 3 / 4;
        }
        else {
            xlog::trace("sound {} channel priority 4", sid);
        }
    }
    for (int i = 0; i < max_channels; ++i) {
        auto& channel = rf::ds_channels[i];
        if (!channel.pdsb) {
            return i;
        }
        if (channel.flags & (rf::SCHF_MUSIC | rf::SCHF_LOOPING | rf::SCHF_PAUSED)) {
            continue;
        }
        DWORD status;
        bool is_playing = SUCCEEDED(channel.pdsb->GetStatus(&status)) && (status & DSBSTATUS_PLAYING) != 0;
        if (!is_playing) {
            rf::snd_ds_close_channel(i);
            return i;
        }
    }
    xlog::trace("No free channel for sound {}", sid);
    return -1;
}

static CodeInjection snd_ds_play_3d_get_free_channel_injection{
    0x005222DA,
    [](auto& regs) {
        auto stack_frame = regs.esp + 0x14;
        auto sid = addr_as_ref<int>(stack_frame + 0x4);
        auto volume = addr_as_ref<float>(stack_frame + 0x18);
        auto is_looping = addr_as_ref<bool>(stack_frame + 0x1C);
        regs.eax = ds_get_free_channel_new(sid, volume, is_looping);
        regs.eip = 0x005222DF;
    },
};

static CodeInjection snd_ds_play_get_free_channel_injection{
    0x00522557,
    [](auto& regs) {
        auto stack_frame = regs.esp + 0x10;
        auto sid = addr_as_ref<int>(stack_frame + 0x4);
        auto volume = addr_as_ref<float>(stack_frame + 0x8);
        auto is_looping = addr_as_ref<bool>(stack_frame + 0x10);
        regs.eax = ds_get_free_channel_new(sid, volume, is_looping);
        regs.eip = 0x0052255C;
    },
};

// Music streams through a 2 s looping buffer refilled every 250 ms (0x0054EE30). When a looping stream reaches the end
// of its file mid-refill, stock rewinds but pads the rest of that refill with silence and counts it toward the 2 s of
// silence that ends a stream, so a looping track gaps at every wrap and stops after a few loops. Leave the rest
// unwritten instead: the next refill continues from the start of the file.
static CodeInjection snd_ds_stream_loop_injection{
    0x0054EEA4,
    [](auto& regs) {
        const int index = static_cast<int>(regs.ebx);
        auto& stream = rf::ds_streams[index];
        if ((stream.flags & rf::DSSF_LOOPING) && rf::snd_ds_stream_rewind(index) == 0) {
            stream.flags &= ~rf::DSSF_END_OF_FILE;
            regs.eip = 0x0054EEEC;
        }
    },
};

CodeInjection snd_ds_init_device_leave_injection{
    0x005215E0,
    []() {
        auto& ds_use_ds3d = addr_as_ref<bool>(0x01AED340);
        auto& ds_use_eax = addr_as_ref<bool>(0x01AD751C);
        xlog::info("DirectSound3D: {}", ds_use_ds3d);
        xlog::info("EAX sound: {}", ds_use_eax);
    },
};

// A buffer's format, with room for a WAVEFORMATEXTENSIBLE: GetFormat fails rather than truncate.
struct DsFormat
{
    alignas(WAVEFORMATEX) BYTE data[64]{};
    WAVEFORMATEX& get() { return *reinterpret_cast<WAVEFORMATEX*>(data); }
    bool read(IDirectSoundBuffer* buffer) { return SUCCEEDED(buffer->GetFormat(&get(), sizeof(data), nullptr)); }
};

static void ds_apply_channel_pitch(int channel)
{
    IDirectSoundBuffer* buffer = rf::ds_channels[channel].pdsb;
    if (!buffer) {
        return;
    }
    const float ratio = g_ds_channel_pitch[channel]
        * (g_ds_channel_unscaled[channel] ? 1.0f : g_ds_rate / g_ds_channel_stretch[channel]);
    if (ratio == 1.0f) {
        buffer->SetFrequency(DSBFREQUENCY_ORIGINAL);
        return;
    }
    DsFormat format;
    if (!format.read(buffer)) {
        return;
    }
    const float freq = format.get().nSamplesPerSec * ratio;
    buffer->SetFrequency(static_cast<DWORD>(std::clamp(freq, static_cast<float>(DSBFREQUENCY_MIN),
                                                       static_cast<float>(DSBFREQUENCY_MAX))));
}

// A new static buffer holding source's sound time-stretched to speed, or null when it cannot be stretched
static IDirectSoundBuffer* ds_create_stretched_buffer(IDirectSoundBuffer* source, float speed, bool looping)
{
    DsFormat format;
    DSBCAPS caps{};
    caps.dwSize = sizeof(caps);
    if (!format.read(source) || FAILED(source->GetCaps(&caps)) || format.get().wFormatTag != WAVE_FORMAT_PCM
        || (format.get().wBitsPerSample != 8 && format.get().wBitsPerSample != 16) || format.get().nBlockAlign == 0) {
        return nullptr;
    }
    const bool is_16bit = format.get().wBitsPerSample == 16;
    void* data = nullptr;
    DWORD size = 0;
    if (FAILED(source->Lock(0, 0, &data, &size, nullptr, nullptr, DSBLOCK_ENTIREBUFFER))) {
        return nullptr;
    }
    const size_t num_samples = size / format.get().nBlockAlign * format.get().nChannels;
    std::vector<float> samples(num_samples);
    for (size_t i = 0; i < num_samples; ++i) {
        samples[i] = is_16bit ? static_cast<const int16_t*>(data)[i] / 32768.0f
                              : (static_cast<const uint8_t*>(data)[i] - 128) / 128.0f;
    }
    source->Unlock(data, size, nullptr, 0);

    const std::vector<float> stretched =
        wsola_stretch(samples, format.get().nChannels, format.get().nSamplesPerSec, speed, looping);
    if (stretched.empty()) {
        return nullptr;
    }
    // The source's controls, located the way snd_ds_create_buffer_from_pcm locates them
    constexpr DWORD kept_flags = DSBCAPS_CTRL3D | DSBCAPS_CTRLFREQUENCY | DSBCAPS_CTRLPAN | DSBCAPS_CTRLVOLUME
        | DSBCAPS_STATIC | DSBCAPS_MUTE3DATMAXDISTANCE;
    DSBUFFERDESC desc{};
    desc.dwSize = sizeof(desc);
    desc.dwFlags = (caps.dwFlags & kept_flags) | ((caps.dwFlags & DSBCAPS_CTRL3D) ? DSBCAPS_LOCDEFER : DSBCAPS_LOCSOFTWARE);
    desc.dwBufferBytes = static_cast<DWORD>(stretched.size() * (is_16bit ? 2 : 1));
    desc.lpwfxFormat = &format.get();
    IDirectSoundBuffer* buffer = nullptr;
    if (FAILED(rf::direct_sound->CreateSoundBuffer(&desc, &buffer, nullptr))) {
        return nullptr;
    }
    if (FAILED(buffer->Lock(0, 0, &data, &size, nullptr, nullptr, DSBLOCK_ENTIREBUFFER))) {
        buffer->Release();
        return nullptr;
    }
    for (size_t i = 0; i < stretched.size() && i * (is_16bit ? 2 : 1) < size; ++i) {
        const float sample = std::clamp(stretched[i], -1.0f, 1.0f);
        if (is_16bit) {
            static_cast<int16_t*>(data)[i] = static_cast<int16_t>(std::lround(sample * 32767.0f));
        }
        else {
            static_cast<uint8_t*>(data)[i] = static_cast<uint8_t>(std::lround(sample * 127.0f + 128.0f));
        }
    }
    buffer->Unlock(data, size, nullptr, 0);
    return buffer;
}

// Stretched copies of sound buffers for g_ds_stretch, made on first play. Each holds a reference to its source so the
// pointer stays unique while cached; channels playing a copy keep its sound data alive after the cache drops it.
struct DsStretchedBuffer
{
    IDirectSoundBuffer* source;
    bool looping;
    IDirectSoundBuffer* stretched; // null when the sound could not be stretched
};
static std::vector<DsStretchedBuffer> g_ds_stretched_buffers;

static IDirectSoundBuffer* ds_get_stretched_buffer(IDirectSoundBuffer* source, bool looping)
{
    for (const auto& entry : g_ds_stretched_buffers) {
        if (entry.source == source && entry.looping == looping) {
            return entry.stretched;
        }
    }
    source->AddRef();
    IDirectSoundBuffer* stretched = ds_create_stretched_buffer(source, g_ds_stretch, looping);
    g_ds_stretched_buffers.push_back({source, looping, stretched});
    return stretched;
}

static void ds_clear_stretched_buffers()
{
    for (const auto& entry : g_ds_stretched_buffers) {
        if (entry.stretched) {
            entry.stretched->Release();
        }
        entry.source->Release();
    }
    g_ds_stretched_buffers.clear();
}

// Moves a looping channel onto its sound's buffer for the current stretch, at the same point of the loop
static void ds_restretch_looping_channel(int channel)
{
    auto& chnl = rf::ds_channels[channel];
    IDirectSoundBuffer* source = rf::ds_buffers[chnl.buf_id].ds_buffer;
    if (!source) {
        return;
    }
    IDirectSoundBuffer* target = g_ds_stretch != 1.0f ? ds_get_stretched_buffer(source, true) : nullptr;
    const float stretch = target ? g_ds_stretch : 1.0f;
    if (!target) {
        target = source;
    }
    DsFormat format;
    DSBCAPS caps{};
    caps.dwSize = sizeof(caps);
    DWORD status = 0;
    DWORD play_pos = 0;
    if (stretch == g_ds_channel_stretch[channel] || !format.read(target) || FAILED(target->GetCaps(&caps))
        || FAILED(chnl.pdsb->GetStatus(&status)) || FAILED(chnl.pdsb->GetCurrentPosition(&play_pos, nullptr))) {
        return;
    }

    IDirectSoundBuffer* buffer = nullptr;
    if (FAILED(rf::direct_sound->DuplicateSoundBuffer(target, &buffer))) {
        return;
    }
    IDirectSound3DBuffer* buffer_3d = nullptr;
    if (chnl.pds3db) {
        // IID_IDirectSound3DBuffer
        static constexpr GUID iid_ds3d_buffer{0x279AFA86, 0x4981, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};
        DS3DBUFFER params{};
        params.dwSize = sizeof(params);
        if (FAILED(buffer->QueryInterface(iid_ds3d_buffer, reinterpret_cast<void**>(&buffer_3d)))
            || FAILED(chnl.pds3db->GetAllParameters(&params)) || FAILED(buffer_3d->SetAllParameters(&params, DS3D_IMMEDIATE))) {
            if (buffer_3d) {
                buffer_3d->Release();
            }
            buffer->Release();
            return;
        }
    }
    LONG volume = 0;
    if (SUCCEEDED(chnl.pdsb->GetVolume(&volume))) {
        buffer->SetVolume(volume);
    }
    LONG pan = 0;
    if (SUCCEEDED(chnl.pdsb->GetPan(&pan))) {
        buffer->SetPan(pan);
    }
    // Same source time: a buffer stretched by s holds 1/s of the source's frames
    const DWORD block = format.get().nBlockAlign;
    const double source_frame = static_cast<double>(play_pos / block) * g_ds_channel_stretch[channel];
    const DWORD num_frames = caps.dwBufferBytes / block;
    buffer->SetCurrentPosition(static_cast<DWORD>(source_frame / stretch) % num_frames * block);
    if (status & DSBSTATUS_PLAYING) {
        buffer->Play(0, 0, DSBPLAY_LOOPING);
    }
    chnl.pdsb->Stop();
    if (chnl.pds3db) {
        chnl.pds3db->Release();
    }
    chnl.pdsb->Release();
    chnl.pdsb = buffer;
    chnl.pds3db = buffer_3d;
    g_ds_channel_stretch[channel] = stretch;
}

void snd_ds_set_channel_pitch(int channel, float ratio)
{
    g_ds_channel_pitch[channel] = ratio;
    ds_apply_channel_pitch(channel);
}

void snd_ds_set_new_channel_unscaled(bool unscaled)
{
    g_ds_new_channel_unscaled = unscaled;
}

void snd_ds_set_global_rate(float rate)
{
    if (rate == g_ds_rate) {
        return;
    }
    g_ds_rate = rate;
    const float stretch = std::clamp(rate, 0.5f, 2.0f);
    const int num_channels = static_cast<int>(std::size(rf::ds_channels));
    if (stretch != g_ds_stretch) {
        g_ds_stretch = stretch;
        ds_clear_stretched_buffers();
        // One-shots finish on the buffer they started with, at the rate their pitch makes up
        for (int i = 0; i < num_channels; ++i) {
            if (rf::ds_channels[i].pdsb && !g_ds_channel_unscaled[i] && (rf::ds_channels[i].flags & rf::SCHF_LOOPING)) {
                ds_restretch_looping_channel(i);
            }
        }
    }
    for (int i = 0; i < num_channels; ++i) {
        if (!g_ds_channel_unscaled[i]) {
            ds_apply_channel_pitch(i);
        }
    }
}

// Each play duplicates the sound's buffer into its channel: at a stretched rate, it duplicates the stretched copy
static FunHook<int(IDirectSoundBuffer*, int)> snd_ds_duplicate_sound_buffer_hook{
    0x005211A0,
    [](IDirectSoundBuffer* source, int channel) {
        g_ds_channel_stretch[channel] = 1.0f;
        if (g_ds_stretch != 1.0f && !g_ds_new_channel_unscaled && source) {
            if (IDirectSoundBuffer* stretched = ds_get_stretched_buffer(source, g_ds_new_channel_looping)) {
                source = stretched;
                g_ds_channel_stretch[channel] = g_ds_stretch;
            }
        }
        return snd_ds_duplicate_sound_buffer_hook.call_target(source, channel);
    },
};

// Runs once the new sound is playing on the channel
static void ds_populate_new_sig(int channel)
{
    static auto sig_counter = 1;
    rf::ds_channels[channel].sig = 0x40000000 | (sig_counter << 8) | channel;
    sig_counter = (sig_counter + 1) % 0x10000;

    g_ds_channel_pitch[channel] = 1.0f;
    g_ds_channel_unscaled[channel] = g_ds_new_channel_unscaled || (rf::ds_channels[channel].flags & rf::SCHF_MUSIC);
    if (g_ds_rate != 1.0f && !g_ds_channel_unscaled[channel]) {
        ds_apply_channel_pitch(channel);
    }
}

static CodeInjection snd_ds_play_sig_generation_patch{
    0x00522683,
    [](auto& regs) {
        int channel = static_cast<int>(regs.esi) / sizeof(rf::DsChannel);
        ds_populate_new_sig(channel);
        regs.eip = 0x005226A0;
    },
};

static CodeInjection snd_ds_play_3d_sig_generation_patch{
    0x005223EF,
    [](auto& regs) {
        int channel = static_cast<rf::DsChannel*>(regs.esi) - rf::ds_channels;
        ds_populate_new_sig(channel);
        regs.eip = 0x00522409;
    },
};

static CodeInjection snd_ds_play_music_sig_generation_patch{
    0x005227D3,
    [](auto& regs) {
        int channel = static_cast<int>(regs.esi) / sizeof(rf::DsChannel);
        ds_populate_new_sig(channel);
        regs.eip = 0x005227F0;
    },
};

static FunHook<int(int)> snd_ds_get_channel_hook{
    0x00522F30,
    [](int sig) -> int {
        if (sig < 0) {
            return -1;
        }
        int index = static_cast<rf::ubyte>(sig);
        if (index >= static_cast<int>(std::size(rf::ds_channels)) || rf::ds_channels[index].sig != sig) {
            return -1;
        }
        return index;
    },
};

#ifdef DEBUG
ConsoleCommand2 playing_sounds_cmd{
    "d_playing_sounds",
    []() {
        for (unsigned chnl_num = 0; chnl_num < std::size(rf::ds_channels); ++chnl_num) {
            auto& chnl = rf::ds_channels[chnl_num];
            if (chnl.pdsb) {
                DWORD status;
                chnl.pdsb->GetStatus(&status);
                auto& buf = rf::ds_buffers[chnl.buf_id];
                rf::console::print("Channel {}: filename {} flags {:x} status {:x}", chnl_num, buf.filename, chnl.flags, status);
            }
        }
    },
};
#endif

struct MmioWrapper {
    uint32_t magic = 0xDEADBEEF; // used to validate existence of wrappers
    HMMIO hmmio = nullptr;
    stb_vorbis* vorbis = nullptr;
};

FunHook<int(LPSTR, LONG, HMMIO*, WAVEFORMATEX**, WAVEFORMATEX**, LPMMCKINFO)> snd_mmio_open_hook{
    0x00563370,
    [](LPSTR filename, LONG offset, HMMIO* hmmio, WAVEFORMATEX **wfmt_orig, WAVEFORMATEX **wfmt_ds, LPMMCKINFO chunk_info) {
        char *sound_name = filename - 0x100;
        xlog::trace("Loading sound: {}", sound_name);
        if (!strcmp(rf::file_get_ext(sound_name), ".ogg")) {
            xlog::info("Loading Ogg Vorbis: {}", sound_name);
            int file_size = rf::File{}.size(sound_name);
            if (file_size <= 0) {
                xlog::error("Cannot get file size: {}", filename);
                return -1;
            }
            FILE *file = std::fopen(filename, "rb");
            if (!file) {
                xlog::error("Failed to open: {}", filename);
                return -1;
            }
            std::fseek(file, offset, SEEK_SET);
            int err = 0;
            stb_vorbis* vorbis = stb_vorbis_open_file_section(file, 1, &err, NULL, file_size);
            if (!vorbis) {
                xlog::error("Failed to load Ogg Vorbis: {}", err);
                return -1;
            }

            stb_vorbis_info info = stb_vorbis_get_info(vorbis);
            xlog::info("Ogg Vorbis info: channels {}, sample rate {}", info.channels, info.sample_rate);

            *wfmt_orig = reinterpret_cast<WAVEFORMATEX*>(rf::rf_malloc(sizeof(WAVEFORMATEX)));
            *wfmt_ds = reinterpret_cast<WAVEFORMATEX*>(rf::rf_malloc(sizeof(WAVEFORMATEX)));
            (*wfmt_orig)->wFormatTag = WAVE_FORMAT_PCM;
            (*wfmt_orig)->nChannels = info.channels;
            (*wfmt_orig)->wBitsPerSample = 16;
            (*wfmt_orig)->nSamplesPerSec = info.sample_rate;
            (*wfmt_orig)->nBlockAlign = (*wfmt_orig)->nChannels * (*wfmt_orig)->wBitsPerSample / 8;
            (*wfmt_orig)->nAvgBytesPerSec = (*wfmt_orig)->nSamplesPerSec * (*wfmt_orig)->nBlockAlign;
            (*wfmt_orig)->cbSize = 0;
            **wfmt_ds = **wfmt_orig;
            auto wrapper = new MmioWrapper;
            wrapper->vorbis = vorbis;
            *hmmio = reinterpret_cast<HMMIO>(wrapper);
            return 0;
        }
        auto wrapper = new MmioWrapper;
        *hmmio = reinterpret_cast<HMMIO>(wrapper);
        return snd_mmio_open_hook.call_target(filename, offset, &wrapper->hmmio, wfmt_orig, wfmt_ds, chunk_info);
    },
};

FunHook<int(HMMIO*, LPMMCKINFO, const MMCKINFO*)> snd_mmio_find_data_chunk_hook{
    0x005635D0,
    [](HMMIO* hmmio, LPMMCKINFO data_mmcki, const MMCKINFO* riff_mmcki) {
        // hmmio is invalid, handle as original function
        if (!hmmio || !*hmmio) {
            return snd_mmio_find_data_chunk_hook.call_target(hmmio, data_mmcki, riff_mmcki);
        }

        // verify we have been passed a wrapper
        // sounds that have already been played will not be passed with a wrapper, and will crash a Music_Start event
        auto* wrapper = reinterpret_cast<MmioWrapper*>(*hmmio);
        if (wrapper->magic == 0xDEADBEEF) {
            if (wrapper->vorbis) {
                xlog::info("Ogg Vorbis seek start");
                stb_vorbis_seek_start(wrapper->vorbis);
                stb_vorbis_info info = stb_vorbis_get_info(wrapper->vorbis);
                float length = stb_vorbis_stream_length_in_seconds(wrapper->vorbis);
                data_mmcki->cksize = static_cast<DWORD>(
                    info.sample_rate * info.channels * sizeof(short) * length
                );
                return 0;
            }
            else {
                return snd_mmio_find_data_chunk_hook.call_target(&wrapper->hmmio, data_mmcki, riff_mmcki);
            }
        }
        else {
            // fallback: not a wrapper, treat as native
            return snd_mmio_find_data_chunk_hook.call_target(hmmio, data_mmcki, riff_mmcki);
        }

    },
};

// Bulk replacement for the stock chunk reader at 0x00563620: the original copies the wav data
// into the destination buffer one byte at a time through the mmio buffered-IO window, which
// costs several ms per sound during level load. One mmioRead call is semantically identical
// (same clamp to mmcki->cksize, same error path) but memcpy-chunked internally.
static int snd_mmio_read_chunk_bulk(HMMIO hmmio, unsigned buf_size, BYTE* buf, MMCKINFO* mmcki, int* bytes_read)
{
    unsigned len = std::min<unsigned>(buf_size, mmcki->cksize);
    mmcki->cksize -= len;
    LONG read = len ? mmioRead(hmmio, reinterpret_cast<HPSTR>(buf), len) : 0;
    if (read < 0 || static_cast<unsigned>(read) != len) {
        *bytes_read = 0;
        return -1;
    }
    *bytes_read = read;
    return 0;
}

FunHook<int(HMMIO, unsigned, BYTE*, MMCKINFO*, int*)> snd_mmio_read_chunk_hook{
    0x00563620,
    [](HMMIO hmmio, unsigned buf_size, BYTE *buf, MMCKINFO *mmcki, int *bytes_read) {
        // hmmio is invalid, handle as original function
        if (!hmmio) {
            return snd_mmio_read_chunk_hook.call_target(hmmio, buf_size, buf, mmcki, bytes_read);
        }

        // verify we have been passed a wrapper
        // sounds that have already been played will not be passed with a wrapper, and will crash a Music_Start event
        auto wrapper = reinterpret_cast<MmioWrapper*>(hmmio);
        if (wrapper->magic == 0xDEADBEEF) {
            auto wrapper = reinterpret_cast<MmioWrapper*>(hmmio);
            if (wrapper->vorbis) {
                stb_vorbis_info info = stb_vorbis_get_info(wrapper->vorbis);
                int samples_read = stb_vorbis_get_samples_short_interleaved(wrapper->vorbis, info.channels, reinterpret_cast<short*>(buf), buf_size / 2);
                *bytes_read = samples_read * info.channels * sizeof(short);
                xlog::info("Ogg Vorbis: read {} samples", samples_read);
                return 0;
            }
            return snd_mmio_read_chunk_bulk(wrapper->hmmio, buf_size, buf, mmcki, bytes_read);
        }
        else {
            // fallback: not a wrapper, treat as native
            return snd_mmio_read_chunk_bulk(hmmio, buf_size, buf, mmcki, bytes_read);
        }
    },
};

FunHook<void(HMMIO*)> snd_mmio_close_hook{
    0x005636E0,
    [](HMMIO *hmmio) {
        // hmmio is invalid or already closed, handle as original function
        if (!hmmio || !*hmmio) {
            return;
        }

        auto wrapper = reinterpret_cast<MmioWrapper*>(*hmmio);
        if (!wrapper) {
            return;
        }

        // verify we have been passed a wrapper
        if (wrapper->magic == 0xDEADBEEF) {
            if (wrapper->vorbis) {
                xlog::info("Closing Ogg Vorbis stream");
                stb_vorbis_close(wrapper->vorbis);
            }
            else {
                snd_mmio_close_hook.call_target(&wrapper->hmmio);
            }
            delete wrapper;
            *hmmio = nullptr;
        }
        else {
            // not our wrapper, treat as native hmmio
            snd_mmio_close_hook.call_target(hmmio);
        }
    },
};

void snd_ds_apply_patch()
{
    // Delete sounds with lowest volume when there is no free slot for a new sound
    snd_ds_play_3d_get_free_channel_injection.install();
    snd_ds_play_get_free_channel_injection.install();

    // Change number of sound channels
    ds_channels_resize_patch.install();
    auto max_ds_channels = static_cast<i8>(std::size(rf::ds_channels));
    write_mem<i8>(0x00521145 + 1, max_ds_channels); // snd_ds_channels_array_ctor
    write_mem<i8>(0x0052163D + 2, max_ds_channels); // snd_ds_init_all_channels
    write_mem<i8>(0x0052191D + 2, max_ds_channels); // snd_ds_close_all_channels
    write_mem<i8>(0x00522F4F + 2, max_ds_channels); // snd_ds_get_channel
    write_mem<i8>(0x00522D1D + 2, max_ds_channels); // snd_ds_close_all_channels2

    // Do not use DSPROPSETID_VoiceManager because it is not implemented by dsoal
    write_mem<i8>(0x00521597 + 4, 0);

    // Loop streamed music without gaps or stopping
    snd_ds_stream_loop_injection.install();

    // Log information about used sound API
    snd_ds_init_device_leave_injection.install();

    // Set rolloff factor to 0 for DirectSound 3D listener
    // It disables distance-based attenuation model from DirectSound 3D. It is necessary because Red Faction original
    // distance model is very different and must be emulated manually using SetVolume API
    write_mem<float>(0x00562EB1 + 1, 0.0f);

    // Optimize performance of mapping a sig to DS channel number
    snd_ds_play_sig_generation_patch.install();
    snd_ds_play_3d_sig_generation_patch.install();
    snd_ds_play_music_sig_generation_patch.install();
    snd_ds_get_channel_hook.install();
    snd_ds_duplicate_sound_buffer_hook.install();
    std::fill(std::begin(g_ds_channel_stretch), std::end(g_ds_channel_stretch), 1.0f);

    // Add support for more formats
    snd_mmio_open_hook.install();
    snd_mmio_find_data_chunk_hook.install();
    snd_mmio_read_chunk_hook.install();
    snd_mmio_close_hook.install();

    // Commands
#ifdef DEBUG
    playing_sounds_cmd.register_cmd();
#endif
}
