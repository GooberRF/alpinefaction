#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include <xlog/xlog.h>
#include <common/alpine_ambience_region.h>
#include <common/utils/string-utils.h>
#include "ambience_region.h"
#include "sound.h"
#include "../misc/alpine_settings.h"
#include "../misc/level.h"
#include "../rf/os/frametime.h"
#include "../rf/sound/sound.h"

namespace aar = alpine_ambience_region;

namespace
{

struct AmbienceRegion
{
    int uid = -1;
    bool enabled = true;
    rf::Vector3 center{};
    rf::Matrix3 orient{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
    rf::Vector3 half_extents{};
    float radius = 0.0f;
    aar::Shape shape = aar::Shape::box;
    float volume = aar::default_volume;
    float fade_distance = aar::default_fade_distance;
    float fade_time = aar::default_fade_time;
    std::size_t voice = 0;
};

// Regions playing the same file share one voice at the loudest region's volume, so overlapping regions that
// build up an irregular area neither stack nor phase against each other.
struct AmbienceVoice
{
    std::string sound; // lowercase
    int handle = -1;
    int sig = -1;
    float volume = 0.0f; // the faded volume, before the group volume
    float target = 0.0f;
    float fade_time = aar::default_fade_time;
};

std::vector<AmbienceRegion> g_regions;
std::vector<AmbienceVoice> g_voices;

std::size_t voice_for_sound(const std::string& sound)
{
    std::string name = string_to_lower(sound);
    for (std::size_t i = 0; i < g_voices.size(); ++i) {
        if (g_voices[i].sound == name) {
            return i;
        }
    }
    AmbienceVoice& voice = g_voices.emplace_back(AmbienceVoice{std::move(name)});
    voice.handle = snd_get_handle_2d(sound.c_str());
    if (voice.handle < 0 && rf::sound_enabled) {
        xlog::warn("[AmbienceRegion] sound {} could not be loaded", voice.sound);
    }
    return g_voices.size() - 1;
}

// How far inside the region the listener is; negative outside.
float region_depth(const AmbienceRegion& region, const rf::Vector3& pos)
{
    const rf::Vector3 delta = pos - region.center;
    switch (region.shape) {
        case aar::Shape::box:
            return std::min({region.half_extents.x - std::fabs(delta.dot_prod(region.orient.rvec)),
                             region.half_extents.y - std::fabs(delta.dot_prod(region.orient.uvec)),
                             region.half_extents.z - std::fabs(delta.dot_prod(region.orient.fvec))});
        case aar::Shape::sphere:
            return region.radius - delta.len();
    }
    return -1.0f;
}

float region_weight(const AmbienceRegion& region, const rf::Vector3& pos)
{
    const float depth = region_depth(region, pos);
    if (depth < 0.0f) {
        return 0.0f;
    }
    return region.fade_distance > 0.0f ? std::min(depth / region.fade_distance, 1.0f) : 1.0f;
}

void voice_stop(AmbienceVoice& voice)
{
    if (voice.sig >= 0) {
        rf::snd_pc_stop(voice.sig);
        voice.sig = -1;
    }
}

} // namespace

void ambience_region_load_chunk(rf::File& file, std::size_t chunk_len)
{
    std::size_t remaining = chunk_len;
    rf::File::ChunkGuard chunk_guard{file, remaining};
    AlpineChunkReader reader{file, remaining};

    uint32_t count = 0;
    if (!reader.read(count)) {
        xlog::warn("[AmbienceRegion] failed to read the region count (chunk len {})", chunk_len);
        return;
    }
    count = std::min(count, aar::max_regions);

    int rejected = 0;
    for (uint32_t i = 0; i < count; ++i) {
        aar::Record rec;
        if (!aar::read_record(reader, rec)) {
            xlog::warn("[AmbienceRegion] chunk truncated after {} of {} regions", i, count);
            break;
        }
        // The whole record is read before any rejection so the stream stays in step. An unknown shape is a
        // newer level's: playing it as the wrong volume is worse than not playing it.
        if (!aar::all_finite(rec.pos) || !aar::all_finite(rec.orient) || rec.shape > aar::shape_max
            || !aar::sound_name_valid(rec.sound) || g_regions.size() >= aar::max_regions) {
            ++rejected;
            continue;
        }

        AmbienceRegion region;
        region.uid = rec.uid;
        region.enabled = (rec.flags & aar::flag_initially_enabled) != 0;
        region.center = rf::Vector3{rec.pos[0], rec.pos[1], rec.pos[2]};
        region.orient.rvec = rf::Vector3{rec.orient[0], rec.orient[1], rec.orient[2]};
        region.orient.uvec = rf::Vector3{rec.orient[3], rec.orient[4], rec.orient[5]};
        region.orient.fvec = rf::Vector3{rec.orient[6], rec.orient[7], rec.orient[8]};
        region.shape = static_cast<aar::Shape>(rec.shape);
        region.half_extents = rf::Vector3{aar::sanitize_dim(rec.width, 0.0f) * 0.5f,
                                          aar::sanitize_dim(rec.height, 0.0f) * 0.5f,
                                          aar::sanitize_dim(rec.depth, 0.0f) * 0.5f};
        region.radius = aar::sanitize_dim(rec.radius, 0.0f);
        region.volume = aar::sanitize_volume(rec.volume);
        region.fade_distance = aar::sanitize_fade_distance(rec.fade_distance);
        region.fade_time = aar::sanitize_fade_time(rec.fade_time);
        region.voice = voice_for_sound(rec.sound);
        g_regions.push_back(region);
    }

    if (rejected > 0) {
        xlog::warn("[AmbienceRegion] rejected {} region record(s): non-finite transform, unknown shape, missing or "
                   "invalid sound file name, or past the {} region limit", rejected, aar::max_regions);
    }
    xlog::debug("[AmbienceRegion] {} region(s) loaded", g_regions.size());
}

void ambience_region_clear_state()
{
    for (AmbienceVoice& voice : g_voices) {
        voice_stop(voice);
    }
    g_voices.clear();
    g_regions.clear();
}

bool ambience_region_set_enabled(int uid, bool enabled)
{
    bool matched = false;
    for (AmbienceRegion& region : g_regions) {
        if (region.uid == uid) {
            region.enabled = enabled;
            matched = true;
        }
    }
    return matched;
}

std::vector<std::pair<int, bool>> ambience_region_get_states()
{
    std::vector<std::pair<int, bool>> out;
    for (const AmbienceRegion& region : g_regions) {
        out.emplace_back(region.uid, region.enabled);
    }
    return out;
}

bool ambience_region_move(int uid, const rf::Vector3& pos)
{
    bool matched = false;
    for (AmbienceRegion& region : g_regions) {
        if (region.uid == uid) {
            region.center = pos;
            matched = true;
        }
    }
    return matched;
}

bool ambience_region_move(int uid, const rf::Vector3& pos, const rf::Matrix3& orient)
{
    bool matched = false;
    for (AmbienceRegion& region : g_regions) {
        if (region.uid == uid) {
            region.center = pos;
            region.orient = orient;
            matched = true;
        }
    }
    return matched;
}

void ambience_region_update(const rf::Vector3& listener_pos)
{
    if (g_voices.empty() || !rf::sound_enabled) {
        return;
    }

    for (AmbienceVoice& voice : g_voices) {
        voice.target = 0.0f;
    }
    for (const AmbienceRegion& region : g_regions) {
        if (!region.enabled) {
            continue;
        }
        const float target = region.volume * region_weight(region, listener_pos);
        AmbienceVoice& voice = g_voices[region.voice];
        if (target > voice.target) {
            voice.target = target;
            voice.fade_time = region.fade_time;
        }
    }

    const float group_volume = rf::snd_group_volume[rf::SOUND_GROUP_EFFECTS] * g_alpine_game_config.level_sound_volume;
    for (AmbienceVoice& voice : g_voices) {
        if (voice.handle < 0) {
            continue;
        }

        if (voice.fade_time > 0.0f) {
            const float step = rf::frametime / voice.fade_time;
            voice.volume = voice.volume < voice.target ? std::min(voice.volume + step, voice.target)
                                                       : std::max(voice.volume - step, voice.target);
        }
        else {
            voice.volume = voice.target;
        }

        if (voice.volume <= 0.0f) {
            voice_stop(voice);
            continue;
        }
        const float play_volume = voice.volume * group_volume;
        // A cutscene skip stops every channel.
        if (voice.sig >= 0 && !rf::snd_pc_is_playing(voice.sig)) {
            voice.sig = -1;
        }
        if (voice.sig < 0) {
            // The engine keeps no record of a failed load, so a broken file would be decoded again every frame.
            if (rf::snd_pc_load_hint(voice.handle, false, false) < 0) {
                xlog::warn("[AmbienceRegion] sound {} could not be loaded", voice.sound);
                voice.handle = -1;
                continue;
            }
            voice.sig = rf::snd_pc_play_looping(voice.handle, play_volume, 0.0f, 0.0f, true);
        }
        else {
            rf::snd_pc_set_volume(voice.sig, play_volume);
        }
    }
}
