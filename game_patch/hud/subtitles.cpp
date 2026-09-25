#include <algorithm>
#include <cctype>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <toml++/toml.hpp>
#include <patch_common/MemUtils.h>
#include <xlog/xlog.h>
#include "subtitles.h"
#include "../misc/vpackfile.h"   // LANG_GR / LANG_FR
#include "../sound/sound.h"      // is_cutscene_music_playing
#include "../rf/file/file.h"
// gr_font.h, not gr.h: split_str / string_aligned / get_font_height / load_font
// live there, and rf/hud.h only pulls in gr/gr.h.
#include "../rf/gr/gr_font.h"
#include "../rf/hud.h"
#include "../rf/misc.h"
#include "../rf/os/frametime.h"
#include "../rf/sound/sound.h"

// ---- NPC voice subtitles ----------------------------------------------------
// Ambient NPC barks have no subtitles in the original game, so a deaf player
// misses them entirely and a translation has nothing to translate. The text is
// data rather than code: an optional TOML table in a packfile maps a wav name to
// the line that should appear on screen.
//
//     [lines]
//     "admf_alert_01.wav" = "Help! Help!"
//
// With no table present the feature is off and behaviour is identical to stock.
constexpr const char* npc_subtitle_base_name = "alpine_npc_subtitles";

// ---- Localized subtitle tables ----------------------------------------------
// Subtitle content is language-specific, so each table is looked up per language
// first and only then by its bare name:
//     alpine_npc_subtitles_en.toml  ->  alpine_npc_subtitles.toml
// The suffixes match the stock language codes (en/gr/fr). The bare name is what a
// translation for a language the stock game has no code for should ship, and is
// also the natural place for a single-language mod to put its table.
static const char* subtitle_lang_suffix()
{
    switch (rf::get_language()) {
    case LANG_GR: return "gr";
    case LANG_FR: return "fr";
    default:      return "en";
    }
}

// Reads the whole of a packfile entry into content_out.
static bool subtitle_read_file(const char* filename, std::string& content_out)
{
    rf::File file;
    if (file.open(filename) != 0) {
        return false;
    }
    const int file_size = file.size();
    if (file_size < 0) {
        file.close();
        return false;
    }
    // An empty file is still a table, and an empty TOML document parses fine. Treating
    // it as missing would fall through to the next candidate and quietly ignore the
    // localized table the author actually shipped.
    content_out.assign(static_cast<size_t>(file_size), '\0');
    const int bytes_read = file_size == 0 ? 0 : file.read(content_out.data(), file_size);
    file.close();
    if (bytes_read != file_size) {
        return false;
    }
    content_out.resize(static_cast<size_t>(bytes_read));
    return true;
}

// Parses the best available table for base_name. Returns nullopt when neither
// candidate exists, which is the normal "no subtitles installed" case.
static std::optional<toml::table> subtitle_parse_table(const char* base_name, std::string& name_out)
{
    const std::string candidates[] = {
        std::format("{}_{}.toml", base_name, subtitle_lang_suffix()),
        std::format("{}.toml", base_name),
    };
    for (const std::string& filename : candidates) {
        std::string content;
        if (!subtitle_read_file(filename.c_str(), content)) {
            continue;
        }
        try {
            toml::table root = toml::parse(content, filename);
            name_out = filename;
            return root;
        }
        catch (const toml::parse_error& err) {
            xlog::error("Failed to parse {}: {}", filename, err.description());
            return std::nullopt;   // present but broken: do not fall back and hide it
        }
    }
    return std::nullopt;
}

static std::string npc_sub_lower(const char* s)
{
    std::string out;
    for (; s && *s; ++s) {
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(*s)));
    }
    return out;
}

static const std::unordered_map<std::string, std::string>& npc_subtitle_table()
{
    static std::unordered_map<std::string, std::string> table;
    static bool loaded = false;
    if (loaded) {
        return table;
    }
    loaded = true;

    std::string filename;
    auto root = subtitle_parse_table(npc_subtitle_base_name, filename);
    if (!root) {
        // Log it: a silent empty table is indistinguishable from a hook that never ran.
        xlog::info("No {}*.toml found, NPC subtitles are off", npc_subtitle_base_name);
        return table;
    }
    const auto* lines = root->get_as<toml::table>("lines");
    if (!lines) {
        xlog::warn("{} has no [lines] table, NPC subtitles are off", filename);
        return table;
    }
    for (const auto& [key, value] : *lines) {
        const auto* text = value.as_string();
        if (!text || text->get().empty()) {
            continue;
        }
        // foley.tbl spells these "Grd_Alert_01.wav" but the packfile index has them
        // upper case, so the key is always lowered on both sides.
        std::string wav = npc_sub_lower(std::string{key.str()}.c_str());
        if (!wav.empty()) {
            table.emplace(std::move(wav), text->get());
        }
    }
    xlog::info("Loaded {} NPC subtitles from {}", table.size(), filename);
    return table;
}

static int g_npc_subtitle_debug = 30;   // log the first N lookups, then go quiet

// ---- Independent subtitle channel -------------------------------------------
// rf::hud_msg is shared with pickup notices ("Picked up ...") and holds at most 8
// entries, so voice subtitles and item pickups keep pushing each other out. This is
// a separate queue with its own render pass: it cannot be stepped on, and position,
// lifetime and styling are ours to set.
constexpr int npc_subtitle_max_lines = 3;

struct NpcSubtitleLine
{
    std::string text;
    int expire_ms = 0;
};

static NpcSubtitleLine g_npc_subtitle_queue[npc_subtitle_max_lines];

static void npc_subtitle_push(const std::string& text, int duration_ms)
{
    for (int i = 0; i + 1 < npc_subtitle_max_lines; ++i) {
        g_npc_subtitle_queue[i] = std::move(g_npc_subtitle_queue[i + 1]);
    }
    auto& slot = g_npc_subtitle_queue[npc_subtitle_max_lines - 1];
    slot.text = text;
    slot.expire_ms = rf::frametime_total_milliseconds + duration_ms;
}

// Same font the message log body uses (0x006C66F0 is set when the log UI is created).
// Cached rather than loaded per call: the .vf redirect logs a line every time.
static auto& npc_subtitle_msglog_font = addr_as_ref<int>(0x006C66F0);

static int npc_subtitle_font()
{
    if (npc_subtitle_msglog_font) {
        return npc_subtitle_msglog_font;
    }
    static int fallback = rf::gr::load_font("rfpc-medium.vf");
    return fallback;
}

// ---- Cutscene subtitles ------------------------------------------------------
// In-engine cutscenes do not play their dialogue through snd_play/snd_play_3d at
// all: the whole scene is one pre-mixed track (RFCS_*_FinalMix.wav) streamed via
// snd_music_play, so there is no per-line filename to look up. The engine also
// ships no text for these lines -- they were transcribed and translated by hand.
// So this is a plain timed track: remember when playback started, then show
// whichever phrase covers the elapsed time.
constexpr const char* cutscene_subtitle_base_name = "alpine_cutscene_subtitles";

struct CutscenePhrase
{
    int start_ms;
    int end_ms;
    std::string text;
};

static const std::unordered_map<std::string, std::vector<CutscenePhrase>>& cutscene_subtitle_table()
{
    static std::unordered_map<std::string, std::vector<CutscenePhrase>> table;
    static bool loaded = false;
    if (loaded) {
        return table;
    }
    loaded = true;

    std::string filename;
    auto root = subtitle_parse_table(cutscene_subtitle_base_name, filename);
    if (!root) {
        xlog::info("No {}*.toml found, cutscene subtitles are off", cutscene_subtitle_base_name);
        return table;
    }
    const auto* tracks = root->get_as<toml::array>("track");
    if (!tracks) {
        xlog::warn("{} has no [[track]] entries, cutscene subtitles are off", filename);
        return table;
    }

    size_t n_lines = 0;
    for (const auto& track_node : *tracks) {
        const auto* track = track_node.as_table();
        if (!track) {
            continue;
        }
        const auto* name = track->get_as<std::string>("name");
        const auto* lines = track->get_as<toml::array>("lines");
        if (!name || name->get().empty() || !lines) {
            continue;
        }
        std::vector<CutscenePhrase> phrases;
        for (const auto& line_node : *lines) {
            const auto* line = line_node.as_table();
            if (!line) {
                continue;
            }
            const auto start = line->get_as<int64_t>("start");
            const auto end = line->get_as<int64_t>("end");
            const auto* text = line->get_as<std::string>("text");
            if (!start || !end || !text || text->get().empty()) {
                continue;
            }
            // Offsets from the start of the track, so negative is meaningless, and
            // anything past INT_MAX would wrap on the way into CutscenePhrase and
            // produce timings unrelated to what the file says.
            if (start->get() < 0 || end->get() <= start->get()
                || end->get() > std::numeric_limits<int>::max()) {
                continue;
            }
            phrases.push_back(CutscenePhrase{
                static_cast<int>(start->get()),
                static_cast<int>(end->get()),
                text->get(),
            });
        }
        if (phrases.empty()) {
            continue;
        }
        // Consumers walk the phrases in order and stop at the first match.
        std::sort(phrases.begin(), phrases.end(),
                  [](const CutscenePhrase& a, const CutscenePhrase& b) {
                      return a.start_ms < b.start_ms;
                  });
        n_lines += phrases.size();
        table.emplace(npc_sub_lower(name->get().c_str()), std::move(phrases));
    }
    xlog::info("Loaded {} cutscene subtitles across {} tracks from {}",
        n_lines, table.size(), filename);
    return table;
}

static const std::vector<CutscenePhrase>* g_cutscene_phrases = nullptr;
static int g_cutscene_started_ms = 0;

void subtitles_cutscene_begin(const char* track)
{
    g_cutscene_phrases = nullptr;
    if (!track) {
        return;
    }
    const auto& table = cutscene_subtitle_table();
    const auto it = table.find(npc_sub_lower(track));
    if (it == table.end()) {
        return;
    }
    g_cutscene_phrases = &it->second;
    g_cutscene_started_ms = rf::frametime_total_milliseconds;
    xlog::info("Cutscene subtitles armed for '{}' ({} lines)", track, it->second.size());
}

// Returns the phrase covering the current playback position, or nullptr.
static const std::string* cutscene_subtitle_current(int now)
{
    if (!g_cutscene_phrases || !is_cutscene_music_playing()) {
        // The track stops when the player skips the cutscene; these are timed
        // against it, so they stop too.
        return nullptr;
    }
    const int elapsed = now - g_cutscene_started_ms;
    for (const auto& p : *g_cutscene_phrases) {
        if (elapsed < p.start_ms) {
            break;      // sorted, so nothing later can match either
        }
        if (elapsed < p.end_ms) {
            return &p.text;
        }
    }
    return nullptr;
}

void subtitles_render()
{
    const int now = rf::frametime_total_milliseconds;

    // Two paths drive this: the HUD message render hook during normal gameplay, and
    // the cutscene per-frame hook (the HUD is not drawn at all during cutscenes).
    // Both can run in the same frame, and drawing twice would double the drop shadow,
    // so the second call in a frame is a no-op.
    //
    // Keyed on the frame counter rather than the clock: at a high framerate two frames
    // can land in the same millisecond, and a paused frame does not advance the clock
    // at all, either of which would drop the text from a frame that needs it.
    static int last_drawn_frame = -1;
    if (rf::frame_count == last_drawn_frame) {
        return;
    }
    last_drawn_frame = rf::frame_count;

    const int font = npc_subtitle_font();
    const int line_h = rf::gr::get_font_height(font);
    const int clip_w = rf::gr::clip_width();
    const int clip_h = rf::gr::clip_height();
    const int max_w = clip_w * 4 / 5;

    // Cutscene line first, then the ordinary queue. During a cutscene the queue is
    // normally empty anyway, but a sound started just before the cutscene began can
    // still be expiring, and stacking them is better than one hiding the other.
    std::vector<std::string> sources;
    if (const std::string* cut = cutscene_subtitle_current(now)) {
        sources.push_back(*cut);
    }
    for (const auto& slot : g_npc_subtitle_queue) {
        if (!slot.text.empty() && now < slot.expire_ms) {
            sources.push_back(slot.text);
        }
    }

    std::vector<std::string> lines;
    for (const auto& src : sources) {
        // gr_split_str is our CJK aware replacement, so this wraps Chinese correctly
        std::string buf = src;
        int len_array[8] = {};
        int off_array[8] = {};
        const int n = rf::gr::split_str(len_array, off_array, buf.data(), max_w, 8, 0, font);
        if (n <= 0) {
            lines.push_back(buf);
        }
        else {
            for (int i = 0; i < n; ++i) {
                lines.emplace_back(buf, static_cast<size_t>(off_array[i]),
                                   static_cast<size_t>(len_array[i]));
            }
        }
    }
    if (lines.empty()) {
        return;
    }

    // Sit above the engine's own message line so the two never overlap.
    const int center_x = clip_w / 2;
    int y = clip_h * 84 / 100 - static_cast<int>(lines.size()) * line_h;
    for (const auto& line : lines) {
        rf::gr::set_color(0, 0, 0, 170);
        rf::gr::string_aligned(rf::gr::ALIGN_CENTER, center_x + 2, y + 2, line.c_str(), font);
        rf::gr::set_color(255, 255, 255, 255);
        rf::gr::string_aligned(rf::gr::ALIGN_CENTER, center_x, y, line.c_str(), font);
        y += line_h;
    }
}


void subtitles_on_sound_play(int handle, const rf::Vector3* pos)
{
    const auto& table = npc_subtitle_table();
    if (table.empty() || handle < 0 || handle >= 2600) {
        return;
    }
    const rf::Sound& snd = rf::sounds[handle];
    const auto it = table.find(npc_sub_lower(snd.filename));

    // Diagnostics first, before any of the early returns below. Logging after them
    // means a line skipped by the persona check leaves no trace at all, which makes
    // "no subtitle" and "hook never saw it" look identical in the log.
    // Only level dialogue names (L<n>S<n>_...) are logged, so gunfire and footsteps
    // do not burn the budget.
    if (g_npc_subtitle_debug > 0) {
        const char* fn = snd.filename;
        // Level dialogue is named L<level>S<scene>_..., e.g. L6S3_GRYN_01.wav.
        // Checking the first two chars avoids needing <cstring> for strchr.
        const bool is_dialogue = fn && (fn[0] == 'L' || fn[0] == 'l')
            && fn[1] >= '0' && fn[1] <= '9';
        if (is_dialogue || it != table.end()) {
            --g_npc_subtitle_debug;
            const int pidx = rf::hud_persona_current_idx;
            const bool persona_owns = pidx >= 0 && pidx < 10
                && rf::hud_personas_info[pidx].sound_handle == handle;
            xlog::info("[npc-sub] '{}' {} in_table={} persona_owns={} alpha={:.2f}",
                fn ? fn : "(null)", pos ? "3D" : "2D", it != table.end(), persona_owns,
                rf::hud_persona_alpha);
        }
    }

    // Skip only the one line the engine is itself subtitling in the persona box.
    // HudPersonaInfo::sound_handle and hud_persona_current_idx are both set before the
    // sound is started (0x0043957C sets the index, the play call reads +0x20 after it),
    // so this comparison is reliable at the moment we run.
    //
    // Do NOT gate on hud_persona_alpha alone: the box stays up for several seconds and
    // world conversations happen underneath it. In the level 1 opening the box is
    // visible while a guard and a miner argue in front of the player, and gating on
    // visibility would drop exactly the lines that have no captions anywhere else.
    const int persona_idx = rf::hud_persona_current_idx;
    if (persona_idx >= 0 && persona_idx < 10
        && rf::hud_personas_info[persona_idx].sound_handle == handle) {
        return;
    }

    float dist = -1.0f;
    if (pos) {
        const float dx = pos->x - rf::sound_listener_pos.x;
        const float dy = pos->y - rf::sound_listener_pos.y;
        const float dz = pos->z - rf::sound_listener_pos.z;
        dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    if (it == table.end()) {
        return;
    }
    // Out of earshot: the engine plays it inaudibly, no reason to subtitle it.
    // Only trust max_range when it is actually set -- snd_get_handle takes min_range
    // but no max_range, so for dynamically loaded sounds it can be left at zero, and
    // a naive "dist > max_range" then rejects everything.
    if (dist >= 0.0f && snd.max_range > 0.0f && dist > snd.max_range) {
        return;
    }
    // Duration follows the clip: long lines stay up long enough to read.
    const float secs = rf::snd_pc_get_duration(handle);
    int ms = static_cast<int>(secs * 1000.0f) + 900;
    ms = std::clamp(ms, 2500, 9000);
    npc_subtitle_push(it->second, ms);
}
