#include <patch_common/FunHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>
#include <patch_common/MemUtils.h>
#include <xlog/xlog.h>
#include <common/utils/string-utils.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include "alpine_color_picker.h"
#include "level.h"
#include "vtypes.h"
#include "mfc_types.h"
#include "resources.h"
#include "mesh.h"
#include "note.h"
#include "corona.h"
#include "bag.h"
#include "weather_region.h"
#include "vehicle_factory.h"
#include "projection_camera.h"
#include "rope_emitter.h"
#include "alpine_obj.h"
#include "alpine_spinner.h"
#include "textures.h"
#include "minimap_bake.h"
#include "terrain.h"
#include "terrain_build.h"
#include "terrain_decorations.h"
#include "dir_light.h"
#include "alpine_lightmaps.h"
#include "overflow_charts.h"
#include "headless_bake.h"
#include "event.h"
#include "bake_progress.h"
#include "face_list_cache.h"

// Forward declarations
int get_level_rfl_version();
void set_initial_level_rfl_version();
void lightmap_reset_level_state();

// Global AlpineLevelProperties — kept separate from CDedLevel allocation to avoid
// stock code overwriting it (the stock rfg group loader writes to CDedLevel fields
// that overlap with the embedded ALP region).
static AlpineLevelProperties g_alpine_level_props;

AlpineLevelProperties& CDedLevel::GetAlpineLevelProperties()
{
    return g_alpine_level_props;
}

void editor_report(EditorReportLevel level, const char* tag, const std::string& msg, bool red_log)
{
    switch (level) {
    case EditorReportLevel::info: xlog::info("[{}] {}", tag, msg); break;
    case EditorReportLevel::warn: xlog::warn("[{}] {}", tag, msg); break;
    case EditorReportLevel::error: xlog::error("[{}] {}", tag, msg); break;
    }
    if (headless_bake_active()) {
        const char* prefix = level == EditorReportLevel::warn    ? "WARNING: "
                             : level == EditorReportLevel::error ? "ERROR: "
                                                                 : "";
        headless_bake_note((prefix + msg).c_str());
        return;
    }
    if (void* log = red_log && GetMainFrame() ? GetLogDlg() : nullptr) {
        LogDlg_Append(log, "%s\n", msg.c_str());
    }
}

void editor_report_blocking(const char* tag, const char* caption, const std::string& msg)
{
    editor_report(EditorReportLevel::error, tag, msg, true);
    if (headless_bake_active()) {
        return;
    }
    // a message box mid-bake would run a modal loop that dispatches every window's messages
    if (bake_progress_active()) {
        bake_progress_defer_message(caption, msg);
    }
    else {
        MessageBoxA(GetMainFrameHandle(), msg.c_str(), caption, MB_OK | MB_ICONWARNING);
    }
}

void editor_address_space_free(std::uint64_t& free_largest, std::uint64_t& free_total)
{
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    free_largest = 0;
    free_total = 0;
    auto addr = reinterpret_cast<std::uintptr_t>(si.lpMinimumApplicationAddress);
    const auto end = reinterpret_cast<std::uintptr_t>(si.lpMaximumApplicationAddress);
    MEMORY_BASIC_INFORMATION mbi{};
    while (addr < end && VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == sizeof(mbi)) {
        if (mbi.State == MEM_FREE) {
            free_largest = std::max<std::uint64_t>(free_largest, mbi.RegionSize);
            free_total += mbi.RegionSize;
        }
        const auto next = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= addr) {
            break;
        }
        addr = next;
    }
}

std::string editor_address_space_shortfall(std::uint64_t largest, std::uint64_t total, const char* advice)
{
    std::uint64_t free_largest = 0;
    std::uint64_t free_total = 0;
    editor_address_space_free(free_largest, free_total);
    constexpr std::uint64_t mb = 1u << 20;
    char msg[512];
    if (free_largest < largest) {
        std::snprintf(msg, sizeof(msg), "RED is low on memory (largest free block %llu MB, needs %llu MB). %s",
                      static_cast<unsigned long long>(free_largest / mb),
                      static_cast<unsigned long long>((largest + mb - 1) / mb), advice);
        return msg;
    }
    if (free_total < total) {
        std::snprintf(msg, sizeof(msg), "RED is low on memory (%llu MB of address space free, needs %llu MB). %s",
                      static_cast<unsigned long long>(free_total / mb),
                      static_cast<unsigned long long>((total + mb - 1) / mb), advice);
        return msg;
    }
    return {};
}

// Initialize on CDedLevel construction
CodeInjection CDedLevel_construct_patch{
    0x004181B8,
    [](auto& regs) {
        set_initial_level_rfl_version();
        g_alpine_level_props.LoadNewLevelDefaults();
    },
};

// Clear alpine properties when the level is reset (File > New, File > Open, document close).
// FUN_00418960 is CDedLevel::DeleteContents (thiscall), reached via vtable[27] (FUN_0041CDF0).
// Alpine objects stay in master_objects through the stock body so undo cleanup (FUN_0043d170) never
// takes them for orphans, and level_destroy_object_hook keeps stock FUN_0041c360 off them. They are
// freed after it, when no undo record can reach them any more.
void __fastcall CDedLevel_DeleteContents_hooked(CDedLevel* level, void* edx_unused);
FunHook<decltype(CDedLevel_DeleteContents_hooked)> CDedLevel_DeleteContents_hook{
    0x00418960,
    CDedLevel_DeleteContents_hooked,
};
void __fastcall CDedLevel_DeleteContents_hooked(CDedLevel* level, void* edx_unused)
{
    auto& props = level->GetAlpineLevelProperties();
    terrain_decorations_level_reset();
    sun_arrow_detach(level);

    // Released while the level is still intact; the stock body only ever sees [obj+0xC] null on Alpine meshes.
    for (auto* m : props.mesh_objects)
        mesh_release_vmesh(m);

    CDedLevel_DeleteContents_hook.call_target(level, edx_unused);

    alpine_graveyard_clear();
    // File > Open applies the existing-level defaults again before it reads the file (0x0042F136)
    props.LoadNewLevelDefaults();
    lightmap_reset_level_state();
    // Also runs as the document closes at exit, when the views may be gone, so nothing is repainted.
    alpine_lm_reset_level_state();
}

// load default AlpineLevelProperties values
CodeInjection CDedLevel_LoadLevel_patch1{
    0x0042F136,
    []() {
        CDedLevel::Get()->GetAlpineLevelProperties().LoadDefaults();
        lightmap_reset_level_state();
        alpine_lm_drop_retained();
    },
};

// Capture another editor's RFL chunk verbatim so it can be re-emitted unchanged on the next save.
static void retained_chunk_deserialize(CDedLevel& level, rf::File& file, uint32_t chunk_id, std::size_t chunk_len, const char* editor)
{
    auto& chunks = level.GetAlpineLevelProperties().retained_chunks;
    std::size_t remaining = chunk_len;

    // Ensures the file advances to the chunk end even on a short read.
    rf::File::ChunkGuard chunk_guard{file, remaining};

    // A legitimate chunk can never be larger than the file itself.
    int file_size = file.get_size();
    if (file_size < 0 || chunk_len > static_cast<std::size_t>(file_size)) {
        xlog::warn("[RetainedChunk] skipping chunk id=0x{:08X} with implausible len={} (file size={})", chunk_id, chunk_len, file_size);
        // Advance to EOF so the section loop terminates cleanly.
        remaining = 0;
        file.seek(0, rf::File::seek_end);
        return;
    }

    RetainedRflChunk chunk;
    chunk.id = chunk_id;

    if (chunk_len > 0) {
        chunk.data.resize(chunk_len);
        int got = file.read(chunk.data.data(), chunk_len);
        // A short read means the source is truncated mid-chunk.
        if (got <= 0 || file.error() || static_cast<std::size_t>(got) < chunk_len) {
            if (got > 0) remaining -= got;
            xlog::warn("[RetainedChunk] failed to fully read chunk id=0x{:08X} (len={}, got={})", chunk_id, chunk_len, got);
            return;
        }
        remaining -= got;
    }

    xlog::debug("[RetainedChunk] retained {} chunk id=0x{:08X} len={}", editor, chunk_id, chunk.data.size());
    chunks.push_back(std::move(chunk));
}

// Re-write all retained foreign-editor chunks verbatim, preserving their original IDs.
static void retained_chunks_serialize(CDedLevel& level, rf::File& file)
{
    auto& chunks = level.GetAlpineLevelProperties().retained_chunks;
    for (const auto& chunk : chunks) {
        auto start_pos = level.BeginRflSection(file, static_cast<int>(chunk.id));
        if (!chunk.data.empty()) {
            file.write(chunk.data.data(), chunk.data.size());
        }
        level.EndRflSection(file, start_pos);
    }
}

// load AlpineLevelProperties chunk from rfl file
CodeInjection CDedLevel_LoadLevel_patch2{
    0x0042F2D4,
    [](auto& regs) {
        auto& file = *static_cast<rf::File*>(regs.esi);

        // Preserve unknown chunks from other editors.
        uint32_t raw_chunk_id = static_cast<uint32_t>(regs.edi);
        if (const char* editor = foreign_chunk_editor(raw_chunk_id)) {
            auto& level = *static_cast<CDedLevel*>(regs.ebp);
            std::size_t chunk_size = regs.ebx;
            retained_chunk_deserialize(level, file, raw_chunk_id, chunk_size, editor);
            regs.eip = 0x0043090C;
            return;
        }

        // Alpine level properties chunk was introduced in rfl v302, no point looking for it before that
        if (file.check_version(302)) {
            auto& level = *static_cast<CDedLevel*>(regs.ebp);
            int chunk_id = regs.edi;
            std::size_t chunk_size = regs.ebx;
            if (chunk_id == alpine_props_chunk_id) {
                auto& alpine_level_props = level.GetAlpineLevelProperties();
                alpine_level_props.Deserialize(file, chunk_size);
                regs.eip = 0x0043090C;
            }
            // Mesh and note chunks were introduced in rfl v304
            if (file.check_version(304)) {
                if (chunk_id == alpine_mesh_chunk_id) {
                    mesh_deserialize_chunk(level, file, chunk_size, file.get_version());
                    regs.eip = 0x0043090C;
                }
                if (chunk_id == alpine_note_chunk_id) {
                    note_deserialize_chunk(level, file, chunk_size);
                    regs.eip = 0x0043090C;
                }
                if (chunk_id == alpine_corona_chunk_id) {
                    corona_deserialize_chunk(level, file, chunk_size);
                    regs.eip = 0x0043090C;
                }
                if (chunk_id == alpine_bag_chunk_id) {
                    bag_deserialize_chunk(level, file, chunk_size);
                    regs.eip = 0x0043090C;
                }
                if (chunk_id == alpine_weather_region_chunk_id) {
                    weather_region_deserialize_chunk(level, file, chunk_size);
                    regs.eip = 0x0043090C;
                }
                if (chunk_id == alpine_projection_camera_chunk_id) {
                    projection_camera_deserialize_chunk(level, file, chunk_size);
                    regs.eip = 0x0043090C;
                }
                if (chunk_id == alpine_rope_emitter_chunk_id) {
                    rope_emitter_deserialize_chunk(level, file, chunk_size);
                    regs.eip = 0x0043090C;
                }
                if (chunk_id == alpine_terrain_chunk_id) {
                    terrain_deserialize_chunk(level, file, chunk_size);
                    regs.eip = 0x0043090C;
                }
                if (chunk_id == alpine_directional_light_chunk_id) {
                    directional_light_deserialize_chunk(level, file, chunk_size, file.get_version());
                    regs.eip = 0x0043090C;
                }
                if (chunk_id == alpine_lightmaps_chunk_id) {
                    alpine_lm_deserialize_chunk(level, file, chunk_size);
                    regs.eip = 0x0043090C;
                }
            }
            // Vehicle factory chunk was introduced in rfl v306
            if (file.check_version(306)) {
                if (chunk_id == alpine_vehicle_factory_chunk_id) {
                    vehicle_factory_deserialize_chunk(level, file, chunk_size);
                    regs.eip = 0x0043090C;
                }
            }
        }
    },
};

// At save time, match geoable brush UIDs to compiled room UIDs via position.
// For each geoable brush, find the detail room whose bbox contains the brush position.
// Find the compiled room that contains a brush's faces by matching face_ids.
// This is the primary method for mapping brushes to rooms — it directly traces
// brush geometry faces through the compiled solid to find which room they ended up in.
// Returns nullptr if no matching face is found (e.g., brush geometry is null).
static GRoom* find_room_by_face_ids(const CDedLevel& level, int32_t brush_uid)
{
    // Find the brush node
    BrushNode* brush_node = nullptr;
    BrushNode* node = level.brush_list;
    if (node) {
        do {
            if (node->uid == brush_uid) {
                brush_node = node;
                break;
            }
            node = node->next;
        } while (node && node != level.brush_list);
    }
    if (!brush_node) return nullptr;

    // Collect face_ids from the brush's source geometry
    auto* brush_geom = static_cast<GSolid*>(brush_node->geometry);
    if (!brush_geom) return nullptr;

    std::unordered_set<int> brush_face_ids;
    for (GFace* face = brush_geom->face_list_head; face; face = face->next_solid) {
        if (face->face_id >= 0) {
            brush_face_ids.insert(face->face_id);
        }
    }
    if (brush_face_ids.empty()) return nullptr;

    // Search the compiled solid's faces for any matching face_id
    for (GFace* face = level.solid->face_list_head; face; face = face->next_solid) {
        if (face->which_room && face->which_room->is_detail &&
            brush_face_ids.count(face->face_id)) {
            return face->which_room;
        }
    }
    return nullptr;
}

// Fallback: find the detail room whose bbox center is closest to the brush position.
// Used when face_id matching fails (e.g., face_ids weren't assigned).
static GRoom* find_room_by_position(const CDedLevel& level, int32_t brush_uid)
{
    BrushNode* node = level.brush_list;
    Vector3 brush_pos;
    bool found = false;
    if (node) {
        do {
            if (node->uid == brush_uid) {
                brush_pos = node->pos;
                found = true;
                break;
            }
            node = node->next;
        } while (node && node != level.brush_list);
    }
    if (!found) return nullptr;

    constexpr float tolerance = 2.0f;
    GRoom* best_room = nullptr;
    float best_dist_sq = FLT_MAX;
    auto& all_rooms = level.solid->all_rooms;
    for (int j = 0; j < all_rooms.get_size(); j++) {
        GRoom* room = all_rooms.data_ptr[j];
        if (!room || !room->is_detail) continue;
        // Skip empty rooms (e.g. secondary rooms emptied by merge_geoable_interior_rooms).
        // Their bbox is stale and would cause false-positive position matches.
        if (!room->face_list_head) continue;
        if (brush_pos.x >= room->bbox_min.x - tolerance && brush_pos.x <= room->bbox_max.x + tolerance &&
            brush_pos.y >= room->bbox_min.y - tolerance && brush_pos.y <= room->bbox_max.y + tolerance &&
            brush_pos.z >= room->bbox_min.z - tolerance && brush_pos.z <= room->bbox_max.z + tolerance) {
            float cx = (room->bbox_min.x + room->bbox_max.x) * 0.5f;
            float cy = (room->bbox_min.y + room->bbox_max.y) * 0.5f;
            float cz = (room->bbox_min.z + room->bbox_max.z) * 0.5f;
            float dx = brush_pos.x - cx, dy = brush_pos.y - cy, dz = brush_pos.z - cz;
            float dist_sq = dx * dx + dy * dy + dz * dz;
            if (dist_sq < best_dist_sq) {
                best_dist_sq = dist_sq;
                best_room = room;
            }
        }
    }

    if (best_room) {
        xlog::debug("[Geoable] brush uid={} matched by position fallback to room uid={}",
            brush_uid, best_room->uid);
    }
    else {
        xlog::debug("[Geoable] brush uid={} at ({:.2f},{:.2f},{:.2f}) has no matching detail room",
            brush_uid, brush_pos.x, brush_pos.y, brush_pos.z);
    }
    return best_room;
}

static void compute_geoable_room_uids(CDedLevel& level, AlpineLevelProperties& props)
{
    // Prune stale UIDs: remove any geoable brush UIDs that no longer exist in the
    // brush list (e.g., the brush was deleted). This must happen before room matching.
    {
        std::unordered_set<int32_t> live_uids;
        BrushNode* node = level.brush_list;
        if (node) {
            do {
                live_uids.insert(node->uid);
                node = node->next;
            } while (node && node != level.brush_list);
        }
        auto& uids = props.geoable_brush_uids;
        uids.erase(std::remove_if(uids.begin(), uids.end(),
            [&live_uids](int32_t uid) { return live_uids.find(uid) == live_uids.end(); }),
            uids.end());
    }

    props.geoable_room_uids.clear();
    props.geoable_room_uids.resize(props.geoable_brush_uids.size(), 0);

    if (!level.solid) {
        xlog::debug("[Geoable] compute_room_uids: no compiled solid");
        return;
    }

    auto& all_rooms = level.solid->all_rooms;

    // The editor's build/compile process creates final rooms as copies that lack UIDs (uid=-1).
    // Final rooms in all_rooms are clones that skip the GRoom constructor and therefore never get
    // a UID. Assign UIDs here so solid_write persists them to the .rfl and our geoable mapping
    // can reference them.
    for (int j = 0; j < all_rooms.get_size(); j++) {
        if (GRoom* room = all_rooms.data_ptr[j]) {
            groom_assign_uid_if_missing(*room);
        }
    }

    for (std::size_t i = 0; i < props.geoable_brush_uids.size(); i++) {
        int32_t brush_uid = props.geoable_brush_uids[i];

        // Primary method: trace brush face_ids through compiled solid to find the exact room.
        // This is reliable because the room isolation code ensures each geoable brush's faces
        // end up in their own dedicated room, and face_ids are preserved during compilation.
        GRoom* room = find_room_by_face_ids(level, brush_uid);
        if (room) {
            props.geoable_room_uids[i] = room->uid;
            xlog::debug("[Geoable] brush uid={} matched by face_id to room uid={} index={}",
                brush_uid, room->uid, room->room_index);
            continue;
        }

        // Fallback: position-based matching (for edge cases where face_ids aren't available)
        room = find_room_by_position(level, brush_uid);
        if (room) {
            props.geoable_room_uids[i] = room->uid;
        }
    }

}

// At save time, match breakable brush UIDs to compiled room UIDs.
// Uses face_id matching (primary) with position-based fallback.
static void compute_breakable_room_uids(CDedLevel& level, AlpineLevelProperties& props)
{
    // Prune rows whose brush no longer qualifies.
    {
        std::unordered_set<int32_t> live_uids;
        BrushNode* node = level.brush_list;
        if (node) {
            do {
                if (node->is_detail && node->life != -1) {
                    live_uids.insert(node->uid);
                }
                node = node->next;
            } while (node && node != level.brush_list);
        }
        for (std::size_t i = 0; i < props.breakable_brush_uids.size(); ) {
            if (live_uids.find(props.breakable_brush_uids[i]) == live_uids.end()) {
                props.breakable_brush_uids.erase(props.breakable_brush_uids.begin() + i);
                props.breakable_room_uids.erase(props.breakable_room_uids.begin() + i);
                props.breakable_materials.erase(props.breakable_materials.begin() + i);
            } else {
                i++;
            }
        }
    }

    // Every destructible detail brush gets an entry, so the game can map its brush UID to the
    // compiled room UID.
    {
        BrushNode* node = level.brush_list;
        if (node) {
            do {
                if (node->is_detail && node->life != -1 &&
                    std::find(props.breakable_brush_uids.begin(), props.breakable_brush_uids.end(),
                              node->uid) == props.breakable_brush_uids.end()) {
                    props.breakable_brush_uids.push_back(node->uid);
                    props.breakable_room_uids.push_back(0);
                    props.breakable_materials.push_back(0);
                }
                node = node->next;
            } while (node && node != level.brush_list);
        }
    }

    // Clear room UIDs — will be recomputed
    for (auto& uid : props.breakable_room_uids) uid = 0;

    if (!level.solid) {
        xlog::debug("[Breakable] compute_room_uids: no compiled solid");
        return;
    }

    for (std::size_t i = 0; i < props.breakable_brush_uids.size(); i++) {
        int32_t brush_uid = props.breakable_brush_uids[i];

        // Primary method: trace brush face_ids through compiled solid to find the exact room.
        GRoom* room = find_room_by_face_ids(level, brush_uid);
        if (room) {
            props.breakable_room_uids[i] = room->uid;
            xlog::debug("[Breakable] brush uid={} matched by face_id to room uid={} index={}",
                brush_uid, room->uid, room->room_index);
            continue;
        }

        // Fallback: position-based matching, but only for rows that carry a real material.
        const uint8_t raw = (i < props.breakable_materials.size()) ? props.breakable_materials[i] : 0;
        if (raw == 0) {
            xlog::debug("[Breakable] brush uid={} mapping-only row unmatched, leaving room uid 0",
                brush_uid);
            continue;
        }

        room = find_room_by_position(level, brush_uid);
        if (room) {
            props.breakable_room_uids[i] = room->uid;
        }
        else {
            xlog::debug("[Breakable] brush uid={} has no matching detail room", brush_uid);
        }
    }
}

// Drop no-shadow-cast entries whose brush no longer exists.
static void prune_no_shadow_cast_brush_uids(CDedLevel& level, AlpineLevelProperties& props)
{
    if (props.no_shadow_cast_brush_uids.empty()) return;

    std::unordered_set<int32_t> keep_uids;
    const std::unordered_set<int32_t> mover_brush_uids = collect_moving_group_brush_uids();
    BrushNode* node = level.brush_list;
    if (node) {
        do {
            if (no_shadow_cast_eligible(*node, mover_brush_uids)) {
                keep_uids.insert(node->uid);
            }
            node = node->next;
        } while (node && node != level.brush_list);
    }
    auto& uids = props.no_shadow_cast_brush_uids;
    uids.erase(std::remove_if(uids.begin(), uids.end(),
        [&keep_uids](int32_t uid) { return keep_uids.find(uid) == keep_uids.end(); }),
        uids.end());
}

// ─── Geoable room isolation ───────────────────────────────────────────────────
// During geometry building, the room builder (FUN_00485990) flood-fills adjacent
// coplanar faces into the same GRoom. This merges geoable and non-geoable detail
// brushes into one room, breaking in-game geomod. Similarly, breakable detail brushes
// must each be in their own room for destruction to work correctly. The hooks below
// ensure that each geoable/breakable brush always gets its own self-contained room:
//   1. adjacency_test_hook prevents merging via the geometric adjacency path
//   2. isolate_marked_rooms post-processes to split any remaining mixed rooms
//      (the flood-fill also merges via edge-based adjacency which bypasses the
//       adjacency test, so post-processing is the primary fix)

// Map from face_id (GFaceAttributes+0x10) to brush UID for brushes that need isolation
// (geoable and breakable detail brushes, terrain chunk brushes). Populated before room building,
// cleared afterwards.
static std::unordered_map<int, int> g_isolated_face_map;

static void populate_isolated_face_map()
{
    g_isolated_face_map.clear();

    auto* level = CDedLevel::Get();
    if (!level) return;
    auto& props = level->GetAlpineLevelProperties();

    std::unordered_set<int32_t> isolated_set;
    terrain_build_isolated_brush_uids(isolated_set);
    if (props.geoable_brush_uids.empty() && props.breakable_brush_uids.empty() && isolated_set.empty()) return;
    isolated_set.insert(props.geoable_brush_uids.begin(), props.geoable_brush_uids.end());
    // Glass entries exist only to carry the brush UID -> room UID mapping for When_Destroyed;
    // they must not join the isolation set or a level would build different room structure than
    // it did before those entries were added.
    for (std::size_t i = 0; i < props.breakable_brush_uids.size(); i++) {
        const uint8_t mat = (i < props.breakable_materials.size()) ? props.breakable_materials[i] : 0;
        if ((mat & 0x7F) != 0) {
            isolated_set.insert(props.breakable_brush_uids[i]);
        }
    }

    BrushNode* head = level->brush_list;
    if (!head) return;
    BrushNode* node = head;
    do {
        if (node->is_detail && isolated_set.count(node->uid)) {
            auto* geom = static_cast<GSolid*>(node->geometry);
            if (geom) {
                for (GFace* face = geom->face_list_head; face; face = face->next_solid) {
                    g_isolated_face_map[face->face_id] = node->uid;
                }
            }
        }
        node = node->next;
    } while (node && node != head);
}

// Split any rooms that contain faces from multiple isolated brushes (geoable or
// breakable) or mixed isolated/non-isolated faces.  Each isolated brush gets its
// own room so in-game geomod and destruction only affect the intended brush.
//
// Called from inside FUN_00485990 (room builder) via CodeInjection at 0x00485e88,
// which is after the detail-marking loop (loop 1) but before the parent-room
// association loop (loop 2).  This ensures:
//   - new rooms are properly associated with parent non-detail rooms (loop 2)
//   - spatial data is rebuilt for all rooms including new ones (final loop)
// Recompute a room's bbox_min/bbox_max from its current face list.
// GRoom::add_face only expands the bbox and removing faces does not shrink it,
// so after splitting faces out we must recompute from scratch.
static void recompute_room_bbox(GRoom* room)
{
    GFace* head = room->face_list_head;
    if (!head) return;

    Vector3 vmin = head->bounding_box_min;
    Vector3 vmax = head->bounding_box_max;

    for (GFace* f = head->next_room; f; f = f->next_room) {
        if (f->bounding_box_min.x < vmin.x) vmin.x = f->bounding_box_min.x;
        if (f->bounding_box_min.y < vmin.y) vmin.y = f->bounding_box_min.y;
        if (f->bounding_box_min.z < vmin.z) vmin.z = f->bounding_box_min.z;
        if (f->bounding_box_max.x > vmax.x) vmax.x = f->bounding_box_max.x;
        if (f->bounding_box_max.y > vmax.y) vmax.y = f->bounding_box_max.y;
        if (f->bounding_box_max.z > vmax.z) vmax.z = f->bounding_box_max.z;
    }

    room->bbox_min = vmin;
    room->bbox_max = vmax;
}

// Leaves the room exactly as add_face's per-face unlinks would, in one pass.
static void unlink_room_faces(GRoom* room, const std::unordered_set<GFace*>& faces)
{
    GFace** link = &room->face_list_head;
    while (GFace* f = *link) {
        if (faces.count(f)) {
            *link = f->next_room;
            f->next_room = nullptr;
            f->which_room = nullptr;
            room->face_list_count--;
        }
        else {
            link = &f->next_room;
        }
    }
    face_list_cache_forget(&room->face_list_head);
}

static void isolate_marked_rooms(GSolid* solid)
{
    // Group faces by room, then by isolated brush UID (-1 = not isolated)
    struct FaceGroup {
        std::unordered_map<int, std::vector<GFace*>> by_brush;
    };
    std::unordered_map<GRoom*, FaceGroup> room_groups;

    for (GFace* face = solid->face_list_head; face; face = face->next_solid) {
        GRoom* room = face->which_room;
        if (!room) continue;
        auto it = g_isolated_face_map.find(face->face_id);
        int uid = (it != g_isolated_face_map.end()) ? it->second : -1;
        room_groups[room].by_brush[uid].push_back(face);
    }

    int rooms_created = 0;
    std::vector<GRoom*> modified_rooms; // original rooms that had faces removed

    for (auto& [room, fg] : room_groups) {
        int isolated_count = 0;
        bool has_unmarked = fg.by_brush.count(-1) > 0;
        for (auto& [uid, faces] : fg.by_brush) {
            if (uid != -1) isolated_count++;
        }

        if (isolated_count == 0) continue;                 // no isolated faces
        if (!has_unmarked && isolated_count <= 1) continue; // single isolated brush, no mixing

        // If room has only isolated faces (no unmarked), keep the first
        // isolated group in the original room to avoid creating an empty room
        int kept_uid = -1;
        if (!has_unmarked) {
            kept_uid = fg.by_brush.begin()->first;
        }

        // add_face's stock unlink walks the room list from its head for every face, which is quadratic
        // when a terrain room is split, so every moving face leaves the room list in one pass first.
        std::unordered_set<GFace*> moving;
        for (auto& [uid, faces] : fg.by_brush) {
            if (uid != -1 && uid != kept_uid) {
                moving.insert(faces.begin(), faces.end());
            }
        }
        unlink_room_faces(room, moving);

        // Room has mixed content — split each isolated brush into its own room
        for (auto& [uid, faces] : fg.by_brush) {
            if (uid == -1 || uid == kept_uid) continue;

            // Use first face's bbox to initialize the new room
            GFace* seed = faces[0];

            GRoom* new_room = GRoom::alloc();
            if (!new_room) {
                for (GFace* f : faces) {
                    room->add_face(f);
                }
                continue;
            }
            new_room->init(solid, &seed->bounding_box_min, &seed->bounding_box_max);

            // Copy life from original room so breakable brushes retain their HP.
            // init() defaults to life=-1.0 (indestructible); without this copy,
            // isolated breakable rooms would never take damage in-game.
            new_room->life = room->life;

            for (GFace* f : faces) {
                new_room->add_face(f);
            }

            new_room->set_detail(solid, 1);

            rooms_created++;

            xlog::debug("[RoomIsolation] isolated brush uid={}: {} faces -> new room",
                       uid, faces.size());
        }

        // Original room had faces removed — needs bbox recomputation
        modified_rooms.push_back(room);
    }

    // Recompute bbox for original rooms that lost faces. add_face only expands
    // the bbox when adding faces; it does not shrink when faces are removed.
    for (GRoom* room : modified_rooms) {
        recompute_room_bbox(room);
    }

    if (rooms_created > 0) {
        xlog::debug("[RoomIsolation] created {} isolated rooms", rooms_created);
    }
}

// Merge all rooms belonging to the same geoable brush into a single room.
// A concave geoable brush (e.g. a hollow box) produces disconnected face groups
// in the flood-fill: exterior faces form one room, interior faces another.
// Rooms are grouped by brush UID via face_id lookup in g_isolated_face_map,
// then faces from secondary rooms are moved into the primary via add_face.
//
// This merge is necessary because compute_geoable_room_uids uses
// find_room_by_face_ids (first-match) — without merging, only the first room
// of a multi-room brush would be flagged geoable in-game.
//
// Emptied secondary rooms are kept as detail in all_rooms (preserving indices
// for serialization). A code injection at 0x485f1a skips empty detail rooms
// in loop 2 to prevent the null face_list crash at 0x485f44. We don't set
// is_detail = false on emptied rooms because that makes their faces behave as
// world geometry and get carved by the boolean engine during adjacent geomods.
static void merge_geoable_interior_rooms(GSolid* solid)
{
    auto* level = CDedLevel::Get();
    if (!level) return;
    auto& props = level->GetAlpineLevelProperties();

    // Terrain chunks merge the same way: holes can split a chunk into several components.
    std::unordered_set<int32_t> geoable_set(
        props.geoable_brush_uids.begin(), props.geoable_brush_uids.end());
    terrain_build_isolated_brush_uids(geoable_set);
    if (geoable_set.empty()) return;

    // Group rooms by geoable brush UID via face_id matching.
    std::unordered_map<int, std::vector<GRoom*>> brush_to_rooms;

    auto& all_rooms = solid->all_rooms;
    for (int i = 0; i < all_rooms.get_size(); i++) {
        GRoom* room = all_rooms.data_ptr[i];
        if (!room || !room->is_detail) continue;

        GFace* head = room->face_list_head;
        if (!head) continue;

        int brush_uid = -1;
        bool conflicting = false;
        for (GFace* f = head; f; f = f->next_room) {
            auto it = g_isolated_face_map.find(f->face_id);
            if (it == g_isolated_face_map.end()) continue;
            if (!geoable_set.count(it->second)) continue;
            if (brush_uid == -1) {
                brush_uid = it->second;
            } else if (it->second != brush_uid) {
                conflicting = true;
                break;
            }
        }

        if (brush_uid != -1 && !conflicting) {
            brush_to_rooms[brush_uid].push_back(room);
        }
    }

    int merges = 0;
    for (auto& [uid, rooms] : brush_to_rooms) {
        if (rooms.size() <= 1) continue;

        // Pick room with most faces as primary
        GRoom* primary = nullptr;
        int max_faces = -1;
        for (GRoom* r : rooms) {
            int count = 0;
            for (GFace* f = r->face_list_head; f; f = f->next_room) count++;
            if (count > max_faces) {
                max_faces = count;
                primary = r;
            }
        }

        for (GRoom* r : rooms) {
            if (r == primary) continue;

            std::vector<GFace*> faces;
            for (GFace* f = r->face_list_head; f; f = f->next_room)
                faces.push_back(f);

            for (GFace* f : faces)
                primary->add_face(f);

            // Leave the emptied room as detail in all_rooms. Don't set
            // is_detail = false — that makes its faces behave as world geometry,
            // causing the boolean engine to carve them during adjacent geomods.
            // The skip_empty_detail_rooms injection prevents the loop 2 crash.
            //
            // Zero out the bbox so future bbox-based spatial queries don't treat
            // this now-empty room as having its old spatial footprint (the stock
            // bbox would still reflect the moved-out faces' positions).
            r->bbox_min = {0.0f, 0.0f, 0.0f};
            r->bbox_max = {0.0f, 0.0f, 0.0f};

            merges++;
        }

        recompute_room_bbox(primary);

        xlog::debug("[RoomMerge] geoable brush uid={}: merged {} rooms into primary",
            uid, static_cast<int>(rooms.size()) - 1);
    }

    if (merges > 0) {
        xlog::debug("[RoomMerge] total: merged {} rooms for geoable brushes", merges);
    }
}

// The GRoom constructor never initialises is_airlock, so a room no airlock Room Effect reaches saved heap garbage.
CodeInjection groom_ctor_clear_airlock_injection{
    0x004855d8,
    [](auto& regs) {
        reinterpret_cast<GRoom*>(static_cast<uintptr_t>(regs.ebp))->is_airlock = false;
    },
};

// Skip empty detail rooms in the room builder's loop 2 (parent association).
// After merging geoable brush rooms, secondary rooms are empty (no faces) but
// remain as detail in all_rooms. Loop 2 at 0x485f44 dereferences the face list
// head to find a vertex for room placement — null face list = crash.
// This injection checks for empty face lists and skips the room.
//
// Context at 0x485f1a: ESI = detail room that passed the is_detail check.
// Skip target: 0x00486009 (next iteration of inner loop).
CodeInjection skip_empty_detail_rooms_in_loop2{
    0x00485f1a,
    [](auto& regs) {
        auto* room = reinterpret_cast<GRoom*>(static_cast<void*>(regs.esi));
        if (!room->face_list_head) {
            regs.eip = 0x00486009;
        }
    },
};

// CodeInjection inside FUN_00485990 at 0x00485e88: after the detail-marking
// loop (loop 1), before the parent-room association loop (loop 2).
// At this address EBP = solid (GSolid* this, set at 0x004859aa).
// The subsequent loops naturally handle parent association, portal creation,
// and spatial data rebuilding for any new rooms we create here.
CodeInjection isolate_rooms_injection{
    0x00485e88,
    [](auto& regs) {
        if (g_isolated_face_map.empty()) return;
        auto* solid = reinterpret_cast<GSolid*>(static_cast<std::byte*>(regs.ebp));
        isolate_marked_rooms(solid);
        merge_geoable_interior_rooms(solid);
    },
};

// Hook FUN_00485990 (room builder, thiscall on GSolid*) to populate/clear
// the face_id → brush UID map around the room builder execution.
void __fastcall build_rooms_hooked(GSolid* solid, void* edx_unused);
FunHook<decltype(build_rooms_hooked)> build_rooms_hook{
    0x00485990,
    build_rooms_hooked,
};
void __fastcall build_rooms_hooked(GSolid* solid, void* edx_unused)
{
    populate_isolated_face_map();
    build_rooms_hook.call_target(solid, edx_unused);
    g_isolated_face_map.clear();
}

// After the room builder, GeoBuild_Driver hands each detail brush to CDedLevel::SyncBrushLife
// (0x0043c2a0), which walks the level solid's face list for the first face with the id of the brush's
// first face and gives that face's room the brush's life. This runs the same loop against an index of
// first faces by id, or the stock loop (from 0x0043a040) if building the index runs out of memory. Replaces
// "MOV EDI,[ESI+0x118]" (6 bytes); 0x0043a065 is the loop exit.
CodeInjection sync_brush_life_injection{
    0x0043a03a,
    [](auto& regs) {
        auto* level = reinterpret_cast<CDedLevel*>(static_cast<uintptr_t>(regs.esi));
        BrushNode* const head = level->brush_list;
        try {
            std::unordered_map<int, GFace*> first_face;
            bool indexed = false;
            BrushNode* brush = head;
            while (brush) {
                auto* geometry = static_cast<GSolid*>(brush->geometry);
                if (brush->is_detail == 1 && geometry && geometry->face_list_head) {
                    if (!indexed) {
                        for (GFace* face = level->solid ? level->solid->face_list_head : nullptr; face;
                             face = face->next_solid) {
                            first_face.try_emplace(face->face_id, face);
                        }
                        indexed = true;
                    }
                    auto it = first_face.find(geometry->face_list_head->face_id);
                    GRoom* room = it != first_face.end() ? it->second->which_room : nullptr;
                    if (room) {
                        room->life = static_cast<float>(brush->life);
                        if (brush->life > 0) {
                            room->is_invincible = false;
                        }
                    }
                }
                brush = brush->next;
                if (brush == head) {
                    break;
                }
            }
            regs.edi = reinterpret_cast<uintptr_t>(brush);
            regs.ebp = 0;
            regs.eip = 0x0043a065;
        }
        catch (const std::bad_alloc&) {
            // only the index allocates, and no room is written before it is complete
            regs.edi = reinterpret_cast<uintptr_t>(head);
            regs.eip = 0x0043a040;
        }
    },
    false, // no trampoline: the injection fully replaces the 6 byte load
};

// FUN_0043a710 starts a Build Geometry (thiscall on CDedLevel*). The minimums cover the first build after
// a load, the largest measured.
constexpr std::uint64_t build_geometry_min_free_block = 100u << 20;
constexpr std::uint64_t build_geometry_min_free_total = 160u << 20;

void __fastcall build_geometry_start_hooked(CDedLevel* level, void* edx_unused);
FunHook<decltype(build_geometry_start_hooked)> build_geometry_start_hook{
    0x0043a710,
    build_geometry_start_hooked,
};
void __fastcall build_geometry_start_hooked(CDedLevel* level, void* edx_unused)
{
    const std::string shortfall =
        editor_address_space_shortfall(build_geometry_min_free_block, build_geometry_min_free_total);
    if (!shortfall.empty()) {
        editor_report_blocking("Build Geometry", "Build Geometry", shortfall);
        return;
    }
    // the build frees the faces the overflow preview is keyed on
    overflow_preview_clear();
    build_geometry_start_hook.call_target(level, edx_unused);
}

// Hook FUN_004861d0 (face adjacency test, cdecl) as secondary defense.
// The primary fix is post-processing in isolate_marked_rooms, but this hook
// also prevents merging via the geometric adjacency path during flood-fill.
bool __cdecl adjacency_test_hooked(GFace* face1, GFace* face2);
FunHook<decltype(adjacency_test_hooked)> adjacency_test_hook{
    0x004861d0,
    adjacency_test_hooked,
};

bool __cdecl adjacency_test_hooked(GFace* face1, GFace* face2)
{
    bool result = adjacency_test_hook.call_target(face1, face2);
    if (!result || g_isolated_face_map.empty()) return result;

    auto it1 = g_isolated_face_map.find(face1->face_id);
    auto it2 = g_isolated_face_map.find(face2->face_id);

    bool iso1 = (it1 != g_isolated_face_map.end());
    bool iso2 = (it2 != g_isolated_face_map.end());

    if (!iso1 && !iso2) return true;   // both unmarked: allow
    if (iso1 != iso2) return false;    // mixed: block
    if (it1->second != it2->second) return false; // different isolated brushes: block
    return true;                        // same isolated brush: allow
}

// Skip "objects outside of level" bounds check for some object types
CodeInjection skip_alpine_objects_bounds_check{
    0x0041d7c0,
    [](auto& regs) {
        auto* obj = reinterpret_cast<DedObject*>(static_cast<uintptr_t>(regs.edx));
        if (obj->type == DedObjectType::DED_MESH ||
            obj->type == DedObjectType::DED_NOTE ||
            obj->type == DedObjectType::DED_CORONA ||
            obj->type == DedObjectType::DED_GAS_REGION ||
            obj->type == DedObjectType::DED_WEATHER_REGION ||
            obj->type == DedObjectType::DED_PROJECTION_CAMERA ||
            obj->type == DedObjectType::DED_ROPE_EMITTER ||
            obj->type == DedObjectType::DED_TERRAIN ||
            obj->type == DedObjectType::DED_DIRECTIONAL_LIGHT) {
            regs.eip = 0x0041dcfa;
        }
    },
};

// The game refuses a level that needs D3D11 on the other renderers; logs every reason it does.
static bool level_needs_d3d11(CDedLevel& level, bool stock_lightmaps_suppressed)
{
    const auto& props = level.GetAlpineLevelProperties();
    std::string reasons;
    auto add_reason = [&](const char* reason) {
        if (!reasons.empty()) {
            reasons += ", ";
        }
        reasons += reason;
    };
    if (props.require_d3d11) {
        add_reason("Require Direct3D 11 setting");
    }
    if (stock_lightmaps_suppressed) {
        add_reason("D3D11-only lightmaps");
    }
    if (std::any_of(props.mesh_objects.begin(), props.mesh_objects.end(),
                    [](const DedMesh* mesh) { return mesh->draw_scale != 1.0f; })) {
        add_reason("mesh draw scale");
    }
    auto& objects = level.master_objects;
    for (int i = 0; i < objects.get_size(); i++) {
        const DedObject* obj = objects.data_ptr[i];
        if (obj && obj->type == DedObjectType::DED_EVENT &&
            static_cast<const DedEvent*>(obj)->event_type == static_cast<int>(AlpineDedEventID::Mesh_Set_Scale)) {
            add_reason("Mesh_Set_Scale event");
            break;
        }
    }
    if (reasons.empty()) {
        return false;
    }
    xlog::info("[Level] Level requires Direct3D 11: {}", reasons);
    return true;
}

// save AlpineLevelProperties when saving rfl file
// At 0x00430CBD, all_objects is already populated but link UIDs haven't been
// converted to indices yet (that happens later in 0x10000/0x20000 chunks).
CodeInjection CDedLevel_SaveLevel_patch{
    0x00430CBD,
    [](auto& regs) {
        auto& level = *static_cast<CDedLevel*>(regs.edi);
        auto& file = *static_cast<rf::File*>(regs.esi);

        // Decides whether the alpine lightmap section is emitted, which the stock lightmaps
        // section write a few instructions later depends on.
        alpine_lm_save_begin(level);

        // A rotation of the sun arrow not yet repainted still belongs in the saved sun direction.
        sun_arrow_sync(&level);

        // Compute room UIDs and scrub stale data before serializing
        auto& alpine_level_props = level.GetAlpineLevelProperties();
        compute_geoable_room_uids(level, alpine_level_props);
        compute_breakable_room_uids(level, alpine_level_props);
        prune_no_shadow_cast_brush_uids(level, alpine_level_props);

        // Scrub hold_open_keyframe_uids: remove entries that don't match any
        // moving group's first keyframe (e.g. deleted movers, UID changes from undo)
        {
            std::unordered_set<int32_t> valid_kf_uids;
            auto& mg = level.moving_groups;
            for (int i = 0; i < mg.size; i++) {
                auto* group = mg[i];
                if (group && group->is_moving_group() && group->keyframes &&
                    group->keyframes->objects.size > 0) {
                    DedObject* first_kf = group->keyframes->objects[0];
                    if (first_kf)
                        valid_kf_uids.insert(first_kf->uid);
                }
            }
            auto& uids = alpine_level_props.hold_open_keyframe_uids;
            uids.erase(std::remove_if(uids.begin(), uids.end(),
                [&valid_kf_uids](int32_t uid) { return valid_kf_uids.find(uid) == valid_kf_uids.end(); }),
                uids.end());
        }

        auto start_pos = level.BeginRflSection(file, alpine_props_chunk_id);
        const bool stock_lightmaps_suppressed = alpine_lm_stock_suppressed();
        alpine_level_props.Serialize(file, stock_lightmaps_suppressed,
                                     level_needs_d3d11(level, stock_lightmaps_suppressed));
        level.EndRflSection(file, start_pos);

        // Write mesh objects chunk
        mesh_serialize_chunk(level, file);

        // Write note objects chunk
        note_serialize_chunk(level, file);

        // Write corona objects chunk
        corona_serialize_chunk(level, file);

        // Write bag objects chunk
        bag_serialize_chunk(level, file);

        // Write weather region objects chunk
        weather_region_serialize_chunk(level, file);

        // Write vehicle factory objects chunk
        vehicle_factory_serialize_chunk(level, file);

        // Write projection camera objects chunk
        projection_camera_serialize_chunk(level, file);

        // Write rope emitter objects chunk
        rope_emitter_serialize_chunk(level, file);

        // Write terrain objects chunk
        terrain_serialize_chunk(level, file);

        // Write directional light objects chunk
        directional_light_serialize_chunk(level, file);

        // Re-write any foreign-editor chunks
        retained_chunks_serialize(level, file);
    },
};

// Stock FlagFaceTextureTraits (0x0041d3c0) only stamps the see-through face flags
// (FACE_SEE_THRU, FACE_HAS_HOLES) on faces carrying FACE_IS_DETAIL, which the geometry
// build sets exclusively on compiled static faces. Mover brushes are saved as raw brush
// geometry, so their faces never get those bits and the game draws their alpha textures
// opaque. Mirror stock's detail-brush rule for the faces of moving group detail brushes;
// the stock pass already cleared the bits, so only OR them back in.
static void flag_mover_face_texture_traits(GSolid* solid)
{
    for (GFace* face = solid->face_list_head; face; face = face->next_solid) {
        if (face->flags & FACE_IS_DETAIL) continue; // stock already handled compiled detail faces
        if (face->bitmap_id == -1 || !bm_has_alpha(face->bitmap_id)) continue;

        face->flags |= FACE_SEE_THRU;

        const char* filename = bm_get_filename(face->bitmap_id);
        if (!filename || !string_istarts_with(filename, "gls_")) {
            face->flags |= FACE_HAS_HOLES;
        }
    }
}

// Hook FUN_0041d330 (FlagFaceTextureTraits_all, cdecl), run on every level save and
// before lightmap UV calculation.
void __cdecl flag_face_texture_traits_all_hooked(CDedLevel* level);
FunHook<decltype(flag_face_texture_traits_all_hooked)> flag_face_texture_traits_all_hook{
    0x0041d330,
    flag_face_texture_traits_all_hooked,
};
void __cdecl flag_face_texture_traits_all_hooked(CDedLevel* level)
{
    flag_face_texture_traits_all_hook.call_target(level);

    // Terrain chunk faces are compiled detail faces, but the terrain shader blends its layers
    // opaquely: an alpha layer texture must not make them see-through or holed.
    const auto& props = level->GetAlpineLevelProperties();
    if (level->solid && !props.terrain_room_uids.empty()) {
        for (GFace* face = level->solid->face_list_head; face; face = face->next_solid) {
            if (face->which_room && props.is_terrain_room(face->which_room->uid)) {
                face->flags &= ~(FACE_SEE_THRU | FACE_HAS_HOLES);
            }
        }
    }

    BrushNode* head = level->brush_list;
    if (!head) return;
    BrushNode* node = head;
    do {
        auto* geom = static_cast<GSolid*>(node->geometry);
        if (geom && node->is_detail && level->brush_in_moving_group(node)) {
            flag_mover_face_texture_traits(geom);
        }
        node = node->next;
    } while (node && node != head);
}

// Fill the sun yaw/pitch edit fields from the 3D viewport camera. The camera is aimed
// ALONG the sun's rays (at the ground), so to-sun is the NEGATED camera forward vector.
static void set_sun_angles_from_camera(HWND hdlg)
{
    auto* viewport = get_active_viewport();
    if (!viewport || !viewport->view_data) {
        return;
    }

    float yaw = 0.0f, pitch = 0.0f;
    if (!alpine_light_dir_to_sun_angles(viewport->view_data->camera_orient.fvec, yaw, pitch)) {
        return;
    }

    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.3f", yaw);
    SetDlgItemTextA(hdlg, IDC_SUN_YAW, buffer);
    std::snprintf(buffer, sizeof(buffer), "%.3f", pitch);
    SetDlgItemTextA(hdlg, IDC_SUN_PITCH, buffer);
}

// Default RGB values for sunlight.
static uint8_t g_sun_color_r = 255;
static uint8_t g_sun_color_g = 255;
static uint8_t g_sun_color_b = 255;

static void update_sun_color_controls(HWND hdlg)
{
    // a null HWND would send InvalidateRect at every window on the desktop
    if (HWND swatch = GetDlgItem(hdlg, IDC_SUN_COLOR_SWATCH)) {
        SendMessageA(swatch, LVM_SETBKCOLOR, 0,
            static_cast<LPARAM>(RGB(g_sun_color_r, g_sun_color_g, g_sun_color_b)));
        InvalidateRect(swatch, nullptr, TRUE);
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "<%d, %d, %d>", g_sun_color_r, g_sun_color_g, g_sun_color_b);
    SetDlgItemTextA(hdlg, IDC_SUN_COLOR_VALUE, buffer);
}

static bool parse_color_text(const char* text, uint8_t& r, uint8_t& g, uint8_t& b)
{
    long values[3] = {};
    const char* p = text;
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    if (*p == '<') {
        ++p;
    }
    for (int i = 0; i < 3; ++i) {
        while (*p == ' ' || *p == '\t' || (i > 0 && *p == ',')) {
            ++p;
        }
        char* end = nullptr;
        long value = std::strtol(p, &end, 10);
        if (end == p || value < 0 || value > 255) {
            return false;
        }
        values[i] = value;
        p = end;
    }
    r = static_cast<uint8_t>(values[0]);
    g = static_cast<uint8_t>(values[1]);
    b = static_cast<uint8_t>(values[2]);
    return true;
}

// Leaves r/g/b untouched unless all three components parse.
static bool read_sun_color_text(HWND hdlg, uint8_t& r, uint8_t& g, uint8_t& b)
{
    char buffer[64] = {};
    GetDlgItemTextA(hdlg, IDC_SUN_COLOR_VALUE, buffer, static_cast<int>(sizeof(buffer)));
    return parse_color_text(buffer, r, g, b);
}

static void pick_sun_color(HWND hdlg)
{
    COLORREF color = RGB(g_sun_color_r, g_sun_color_g, g_sun_color_b);
    if (alpine_pick_color(hdlg, color, alpine_shared_custom_colors())) {
        g_sun_color_r = GetRValue(color);
        g_sun_color_g = GetGValue(color);
        g_sun_color_b = GetBValue(color);
        update_sun_color_controls(hdlg);
    }
}

static bool read_window_float(HWND hwnd, float& out, float min_value, float max_value)
{
    char buffer[64] = {};
    if (hwnd) {
        GetWindowTextA(hwnd, buffer, static_cast<int>(sizeof(buffer)));
    }
    char* end = nullptr;
    float value = std::strtof(buffer, &end);
    if (end == buffer || !std::isfinite(value)) {
        return false;
    }
    out = std::clamp(value, min_value, max_value);
    return true;
}

static bool read_dlg_float(HWND hdlg, int id, float& out, float min_value, float max_value)
{
    return read_window_float(GetDlgItem(hdlg, id), out, min_value, max_value);
}

// ─── Level Properties tabs ───

namespace
{

enum class LevelTab : int
{
    general,
    lighting,
    lightmaps,
    gameplay,
    minimap,
    compatibility,
};

struct LevelTabControl
{
    int id;
    LevelTab tab;
};

} // namespace

constexpr const char* level_tab_names[] = {"General", "Lighting", "Lightmaps", "Gameplay", "Minimap",
                                               "Compatibility"};

// IDC_LEVEL_DIRECTIONAL_LIGHT is left out so it stays hidden on every tab.
constexpr LevelTabControl level_tab_controls[] = {
    {IDC_LEVEL_NAME_LABEL, LevelTab::general},
    {IDC_LEVEL_NAME, LevelTab::general},
    {IDC_LEVEL_AUTHOR_LABEL, LevelTab::general},
    {IDC_LEVEL_AUTHOR, LevelTab::general},
    {IDC_LEVEL_DATE_LABEL, LevelTab::general},
    {IDC_LEVEL_DATE, LevelTab::general},
    {IDC_LEVEL_VERSION_LABEL, LevelTab::general},
    {IDC_LEVEL_VERSION, LevelTab::general},
    {IDC_LEVEL_MULTIPLAYER, LevelTab::general},
    {IDC_LEVEL_FLAGS_GROUP, LevelTab::general},
    {IDC_LEVEL_OUTSIDE, LevelTab::general},
    {IDC_LEVEL_INSIDE, LevelTab::general},
    {IDC_LEVEL_GEOMOD_GROUP, LevelTab::general},
    {IDC_LEVEL_GEOMOD_TEXTURE_LABEL, LevelTab::general},
    {IDC_LEVEL_GEOMOD_TEXTURE, LevelTab::general},
    {IDC_LEVEL_GEOMOD_TEXTURE_BROWSE, LevelTab::general},
    {IDC_LEVEL_HARDNESS_LABEL, LevelTab::general},
    {IDC_LEVEL_HARDNESS, LevelTab::general},
    {IDC_LEVEL_HARDNESS_SPIN, LevelTab::general},
    {IDC_LEVEL_HARDNESS_HINT, LevelTab::general},
    {IDC_RF2_STYLE_GEOMOD, LevelTab::general},
    {IDC_REQUIRE_D3D11, LevelTab::general},

    {IDC_LEVEL_AMBIENT_GROUP, LevelTab::lighting},
    {IDC_LEVEL_AMBIENT_COLOR_LABEL, LevelTab::lighting},
    {IDC_LEVEL_AMBIENT_SWATCH, LevelTab::lighting},
    {IDC_LEVEL_AMBIENT_CHANGE, LevelTab::lighting},
    {IDC_LEVEL_AMBIENT_VALUE, LevelTab::lighting},
    {IDC_LEVEL_AMBIENT_DEFAULT, LevelTab::lighting},
    {IDC_LEVEL_FOG_GROUP, LevelTab::lighting},
    {IDC_LEVEL_FOG_COLOR_LABEL, LevelTab::lighting},
    {IDC_LEVEL_FOG_SWATCH, LevelTab::lighting},
    {IDC_LEVEL_FOG_CHANGE, LevelTab::lighting},
    {IDC_LEVEL_FOG_VALUE, LevelTab::lighting},
    {IDC_LEVEL_FOG_NEAR_CLIP_LABEL, LevelTab::lighting},
    {IDC_LEVEL_FOG_NEAR_CLIP, LevelTab::lighting},
    {IDC_LEVEL_FOG_NEAR_CLIP_HINT, LevelTab::lighting},
    {IDC_LEVEL_FOG_FAR_CLIP_LABEL, LevelTab::lighting},
    {IDC_LEVEL_FOG_FAR_CLIP, LevelTab::lighting},
    {IDC_LEVEL_CAMERA_FAR_CLIP_LABEL, LevelTab::lighting},
    {IDC_LEVEL_CAMERA_FAR_CLIP, LevelTab::lighting},
    {IDC_LEVEL_CAMERA_FAR_CLIP_SPIN, LevelTab::lighting},
    {IDC_LEVEL_CAMERA_FAR_CLIP_HINT, LevelTab::lighting},
    {IDC_LEVEL_CAMERA_FAR_CLIP_WARNING_ICON, LevelTab::lighting},
    {IDC_LEVEL_CAMERA_FAR_CLIP_WARNING, LevelTab::lighting},
    {IDC_MESH_AMBIENT_GROUP, LevelTab::lighting},
    {IDC_OVERRIDE_MESH_AMBIENT_LIGHT_MODIFIER, LevelTab::lighting},
    {IDC_MESH_AMBIENT_LIGHT_MODIFIER_LABEL, LevelTab::lighting},
    {IDC_MESH_AMBIENT_LIGHT_MODIFIER, LevelTab::lighting},
    {IDC_SUN_GROUP, LevelTab::lighting},
    {IDC_SUN_ENABLE, LevelTab::lighting},
    {IDC_SUN_SET_FROM_CAMERA, LevelTab::lighting},
    {IDC_SUN_YAW_LABEL, LevelTab::lighting},
    {IDC_SUN_YAW, LevelTab::lighting},
    {IDC_SUN_PITCH_LABEL, LevelTab::lighting},
    {IDC_SUN_PITCH, LevelTab::lighting},
    {IDC_SUN_INTENSITY_LABEL, LevelTab::lighting},
    {IDC_SUN_INTENSITY, LevelTab::lighting},
    {IDC_SUN_SPREAD_ANGLE_LABEL, LevelTab::lighting},
    {IDC_SUN_SPREAD_ANGLE, LevelTab::lighting},
    {IDC_SUN_COLOR_LABEL, LevelTab::lighting},
    {IDC_SUN_COLOR_SWATCH, LevelTab::lighting},
    {IDC_SUN_COLOR_CHANGE, LevelTab::lighting},
    {IDC_SUN_COLOR_VALUE, LevelTab::lighting},
    {IDC_SUN_CAST_BAKED_SHADOWS, LevelTab::lighting},
    {IDC_SUN_AFFECTS_MESHES, LevelTab::lighting},
    {IDC_SUN_MESH_MODE_SCALE, LevelTab::lighting},
    {IDC_SUN_DRIVES_SHADOWMAP_DIR, LevelTab::lighting},
    {IDC_SUN_LIQUID_OCCLUDES, LevelTab::lighting},

    {IDC_HIGHRES_LIGHTMAPS, LevelTab::lightmaps},
    {IDC_LIGHT_BLOCKING_GROUP, LevelTab::lightmaps},
    {IDC_INVISIBLE_FACES_OCCLUDE, LevelTab::lightmaps},
    {IDC_ALPHA_FACES_OCCLUDE, LevelTab::lightmaps},
    {IDC_MESHES_OCCLUDE, LevelTab::lightmaps},
    {IDC_D3D11_ONLY_LIGHTMAPS, LevelTab::lightmaps},
    {IDC_LIGHTMAP_DENSITY_LABEL, LevelTab::lightmaps},
    {IDC_LIGHTMAP_DENSITY, LevelTab::lightmaps},
    {IDC_LIGHTMAP_COMPRESSION_LABEL, LevelTab::lightmaps},
    {IDC_LIGHTMAP_COMPRESSION, LevelTab::lightmaps},

    {IDC_STARTS_WITH_HEADLAMP, LevelTab::gameplay},
    {IDC_VEHICLE_FLIGHT_CEILING_ENABLE, LevelTab::gameplay},
    {IDC_VEHICLE_FLIGHT_CEILING_LABEL, LevelTab::gameplay},
    {IDC_VEHICLE_FLIGHT_CEILING, LevelTab::gameplay},
    {IDC_LEGACY_CYCLIC_TIMERS, LevelTab::compatibility},
    {IDC_LEGACY_MOVERS, LevelTab::compatibility},
    {IDC_LEGACY_LIGHTING, LevelTab::compatibility},

    {IDC_MINIMAP_ENABLE, LevelTab::minimap},
    {IDC_MINIMAP_BITMAP_LABEL, LevelTab::minimap},
    {IDC_MINIMAP_BITMAP, LevelTab::minimap},
    {IDC_MINIMAP_BITMAP_BROWSE, LevelTab::minimap},
    {IDC_MINIMAP_BOUNDS_GROUP, LevelTab::minimap},
    {IDC_MINIMAP_MIN_X_LABEL, LevelTab::minimap},
    {IDC_MINIMAP_MIN_X, LevelTab::minimap},
    {IDC_MINIMAP_MIN_X_SPIN, LevelTab::minimap},
    {IDC_MINIMAP_MIN_Z_LABEL, LevelTab::minimap},
    {IDC_MINIMAP_MIN_Z, LevelTab::minimap},
    {IDC_MINIMAP_MIN_Z_SPIN, LevelTab::minimap},
    {IDC_MINIMAP_MAX_X_LABEL, LevelTab::minimap},
    {IDC_MINIMAP_MAX_X, LevelTab::minimap},
    {IDC_MINIMAP_MAX_X_SPIN, LevelTab::minimap},
    {IDC_MINIMAP_MAX_Z_LABEL, LevelTab::minimap},
    {IDC_MINIMAP_MAX_Z, LevelTab::minimap},
    {IDC_MINIMAP_MAX_Z_SPIN, LevelTab::minimap},
    {IDC_MINIMAP_CUT_HEIGHT_LABEL, LevelTab::minimap},
    {IDC_MINIMAP_CUT_HEIGHT, LevelTab::minimap},
    {IDC_MINIMAP_CUT_HEIGHT_SPIN, LevelTab::minimap},
    {IDC_MINIMAP_CUT_HEIGHT_HINT, LevelTab::minimap},
    {IDC_MINIMAP_BITMAP_PREVIEW, LevelTab::minimap},
    {IDC_MINIMAP_BAKE_RES_LABEL, LevelTab::minimap},
    {IDC_MINIMAP_BAKE_RES, LevelTab::minimap},
    {IDC_MINIMAP_BAKE, LevelTab::minimap},
    {IDC_MINIMAP_BAKE_USE_BOUNDS, LevelTab::minimap},
};

static const LevelTabControl* level_tab_control_find(int id)
{
    const auto it = std::find_if(std::begin(level_tab_controls), std::end(level_tab_controls),
                                 [id](const LevelTabControl& control) { return control.id == id; });
    return it != std::end(level_tab_controls) ? it : nullptr;
}

static HWND g_level_fog_far_edit = nullptr;

// The stored value while the field still shows it exactly, 0 (automatic) when emptied, else the typed value;
// text that is not a number keeps the stored value, as the other Level Properties fields do.
static float read_camera_far_clip_field(HWND hdlg, float stored)
{
    char text[32] = {};
    char shown[32];
    GetDlgItemTextA(hdlg, IDC_LEVEL_CAMERA_FAR_CLIP, text, static_cast<int>(sizeof(text)));
    alpine_format_float_exact(shown, stored);
    if (std::strcmp(text, shown) == 0) {
        return stored;
    }
    if (text[std::strspn(text, " \t")] == '\0') {
        return 0.0f;
    }
    float value = stored;
    read_dlg_float(hdlg, IDC_LEVEL_CAMERA_FAR_CLIP, value, 0.0f, FLT_MAX);
    return value;
}

// On the Lighting tab, the warning takes the place of the hint while the camera far clip ends before the fog does.
static void level_dialog_update_camera_far_clip_warning(HWND hdlg)
{
    const bool lighting = SendDlgItemMessageA(hdlg, IDC_LEVEL_TABS, TCM_GETCURSEL, 0, 0) ==
                          static_cast<LRESULT>(LevelTab::lighting);
    const float camera_far_clip = alpine_camera_far_clip::sanitize(
        read_camera_far_clip_field(hdlg, CDedLevel::Get()->GetAlpineLevelProperties().camera_far_clip));
    float fog_far_clip = 0.0f;
    const bool warn = read_window_float(g_level_fog_far_edit, fog_far_clip, 0.0f, FLT_MAX) && camera_far_clip > 0.0f &&
                      camera_far_clip < std::min(fog_far_clip, alpine_camera_far_clip::max_value);
    auto show = [hdlg](int id, bool visible) {
        if (HWND control = GetDlgItem(hdlg, id)) {
            ShowWindow(control, visible ? SW_SHOWNA : SW_HIDE);
        }
    };
    show(IDC_LEVEL_CAMERA_FAR_CLIP_HINT, lighting && !warn);
    show(IDC_LEVEL_CAMERA_FAR_CLIP_WARNING_ICON, lighting && warn);
    show(IDC_LEVEL_CAMERA_FAR_CLIP_WARNING, lighting && warn);
}

// RED's fog far clip spinner is a dialog of its own, so its edit's notifications never reach the level dialog.
static LRESULT CALLBACK fog_far_spinner_subclass_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam,
                                                      UINT_PTR subclass_id, DWORD_PTR hdlg)
{
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, fog_far_spinner_subclass_proc, subclass_id);
    }
    const LRESULT result = DefSubclassProc(hwnd, msg, wparam, lparam);
    if (msg == WM_COMMAND && HIWORD(wparam) == EN_CHANGE) {
        level_dialog_update_camera_far_clip_warning(reinterpret_cast<HWND>(hdlg));
    }
    return result;
}

// The up-down in RED's float spinner container (dialog 209), beside its edit 1234
constexpr int red_float_spinner_updown_id = 1228;

// Lays the camera far clip edit and arrows out exactly like the fog clip spinners above them.
static void level_dialog_match_fog_spinner_layout(HWND hdlg, HWND fog_edit)
{
    HWND fog_spin = fog_edit ? GetDlgItem(GetParent(fog_edit), red_float_spinner_updown_id) : nullptr;
    HWND edit = GetDlgItem(hdlg, IDC_LEVEL_CAMERA_FAR_CLIP);
    HWND spin = GetDlgItem(hdlg, IDC_LEVEL_CAMERA_FAR_CLIP_SPIN);
    if (!fog_spin || !edit || !spin) return;
    auto rect_in_dialog = [hdlg](HWND hwnd) {
        RECT r{};
        GetWindowRect(hwnd, &r);
        MapWindowPoints(nullptr, hdlg, reinterpret_cast<POINT*>(&r), 2);
        return r;
    };
    const RECT fog_edit_rect = rect_in_dialog(fog_edit);
    const RECT fog_spin_rect = rect_in_dialog(fog_spin);
    const int top = rect_in_dialog(edit).top;
    constexpr UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
    SetWindowPos(edit, nullptr, fog_edit_rect.left, top, fog_edit_rect.right - fog_edit_rect.left,
                 fog_edit_rect.bottom - fog_edit_rect.top, flags);
    SetWindowPos(spin, nullptr, fog_spin_rect.left, top + (fog_spin_rect.top - fog_edit_rect.top),
                 fog_spin_rect.right - fog_spin_rect.left, fog_spin_rect.bottom - fog_spin_rect.top, flags);
}

static void level_dialog_init_camera_far_clip(HWND hdlg, const CLevelDialog& dlg, float camera_far_clip)
{
    alpine_dlg_set_float_field_exact(hdlg, IDC_LEVEL_CAMERA_FAR_CLIP, camera_far_clip);
    alpine_spinner_init(hdlg, IDC_LEVEL_CAMERA_FAR_CLIP, IDC_LEVEL_CAMERA_FAR_CLIP_SPIN, 1.0f, 0.0f,
                        alpine_camera_far_clip::max_value, 2);
    // LoadIconWithScaleDown would tie the editor to comctl32 6; a shared icon is never destroyed
    const auto warning_icon = static_cast<HICON>(LoadImageA(nullptr, reinterpret_cast<LPCSTR>(IDI_WARNING),
                                                            IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                            GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    SendDlgItemMessageA(hdlg, IDC_LEVEL_CAMERA_FAR_CLIP_WARNING_ICON, STM_SETICON,
                        reinterpret_cast<WPARAM>(warning_icon), 0);
    g_level_fog_far_edit = dlg.fog_far_spinner ? dlg.fog_far_spinner->edit._d.m_hWnd : nullptr;
    level_dialog_match_fog_spinner_layout(hdlg, g_level_fog_far_edit);
    if (HWND container = g_level_fog_far_edit ? GetParent(g_level_fog_far_edit) : nullptr) {
        SetWindowSubclass(container, fog_far_spinner_subclass_proc, 1, reinterpret_cast<DWORD_PTR>(hdlg));
    }
}

// What stock stores for a fog spinner after DoModal (0x004024E5). The spinner parses its text only on
// EN_KILLFOCUS (0x0044B060), which an edit still focused when OK is pressed gets during teardown, so the text is
// what counts: empty keeps the level's value, anything else goes through atof and the spinner's range (0x0044B180).
static float fog_spinner_stored_value(const DedFloatSpinner& spinner, float level_value)
{
    char buffer[64] = {};
    GetWindowTextA(spinner.edit._d.m_hWnd, buffer, static_cast<int>(sizeof(buffer)));
    if (!buffer[0]) {
        return level_value;
    }
    const float value = std::strtof(buffer, nullptr);
    return std::isnan(value) ? spinner.min_value : std::clamp(value, spinner.min_value, spinner.max_value);
}

// The game drops a fog near clip that is not below the far clip, so pull it just below.
static void level_dialog_clamp_fog_near_clip(const CLevelDialog& dlg)
{
    DedFloatSpinner* near_spinner = dlg.fog_near_spinner;
    DedFloatSpinner* far_spinner = dlg.fog_far_spinner;
    const CDedLevel* level = CDedLevel::Get();
    if (!near_spinner || !far_spinner || !level) return;
    const float far_clip = fog_spinner_stored_value(*far_spinner, level->fog_far_clip);
    const float near_clip = fog_spinner_stored_value(*near_spinner, level->fog_near_clip);
    float clamped = near_clip > 0.0f ? near_clip : 0.0f;
    if (far_clip > 0.0f && clamped >= far_clip) {
        clamped = std::max(far_clip - 0.01f, 0.0f);
    }
    if (clamped == near_clip) {
        return;
    }
    // The text is what a later EN_KILLFOCUS parses
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.2f", clamped);
    clamped = std::strtof(buffer, nullptr);
    if (far_clip > 0.0f && clamped >= far_clip) {
        clamped = 0.0f;
        std::snprintf(buffer, sizeof(buffer), "%.2f", clamped);
    }
    SetWindowTextA(near_spinner->edit._d.m_hWnd, buffer);
    near_spinner->value = clamped;
    near_spinner->valid = true;
}

// Stock OnInitDialog replaces the fog clip placeholders with id-less spinner windows inserted after the fog
// Change color button in z-order, so a child missing from the table takes the tab of the listed one before it.
static void level_dialog_show_tab(HWND hdlg, LevelTab tab)
{
    SendDlgItemMessageA(hdlg, IDC_LEVEL_TABS, TCM_SETCURSEL, static_cast<WPARAM>(tab), 0);
    std::optional<LevelTab> owner;
    for (HWND child = GetWindow(hdlg, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        const int id = GetDlgCtrlID(child);
        if (const LevelTabControl* control = level_tab_control_find(id)) {
            owner = control->tab;
        }
        else if (!owner || id == IDOK || id == IDCANCEL || id == IDC_LEVEL_TABS || id == IDC_LEVEL_DIRECTIONAL_LIGHT) {
            continue;
        }
        ShowWindow(child, *owner == tab ? SW_SHOWNA : SW_HIDE);
    }
    level_dialog_update_camera_far_clip_warning(hdlg);
}

static HBRUSH g_level_tab_pane_brush = nullptr;
static COLORREF g_level_tab_pane_color = 0;

// The pages are siblings of the tab control, not children of it, so their statics and buttons would paint the
// dialog's face color over the pane, which is white under visual styles.
static void level_dialog_sample_tab_pane(HWND tabs)
{
    RECT client{};
    GetClientRect(tabs, &client);
    RECT pane = client;
    SendMessageA(tabs, TCM_ADJUSTRECT, FALSE, reinterpret_cast<LPARAM>(&pane));
    COLORREF color = GetSysColor(COLOR_3DFACE);
    if (HDC tabs_dc = GetDC(tabs)) {
        if (HDC mem_dc = CreateCompatibleDC(tabs_dc)) {
            if (HBITMAP bitmap = CreateCompatibleBitmap(tabs_dc, client.right, client.bottom)) {
                HGDIOBJ old_bitmap = SelectObject(mem_dc, bitmap);
                FillRect(mem_dc, &client, GetSysColorBrush(COLOR_3DFACE));
                SendMessageA(tabs, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(mem_dc), PRF_CLIENT | PRF_ERASEBKGND);
                const COLORREF sampled = GetPixel(mem_dc, (pane.left + pane.right) / 2, (pane.top + pane.bottom) / 2);
                if (sampled != CLR_INVALID) {
                    color = sampled;
                }
                SelectObject(mem_dc, old_bitmap);
                DeleteObject(bitmap);
            }
            DeleteDC(mem_dc);
        }
        ReleaseDC(tabs, tabs_dc);
    }
    if (g_level_tab_pane_brush) {
        DeleteObject(g_level_tab_pane_brush);
    }
    g_level_tab_pane_color = color;
    g_level_tab_pane_brush = CreateSolidBrush(color);
}

// Group boxes leave their inside to the parent, and the tab control under them is clipped away.
static void level_dialog_fill_tab_pane(HWND hdlg, HDC dc)
{
    HWND tabs = GetDlgItem(hdlg, IDC_LEVEL_TABS);
    if (!tabs) return;
    RECT pane{};
    GetWindowRect(tabs, &pane);
    MapWindowPoints(nullptr, hdlg, reinterpret_cast<POINT*>(&pane), 2);
    SendMessageA(tabs, TCM_ADJUSTRECT, FALSE, reinterpret_cast<LPARAM>(&pane));
    FillRect(dc, &pane, g_level_tab_pane_brush);
}

static bool level_dialog_paints_on_pane(HWND control)
{
    const int id = GetDlgCtrlID(control);
    if (id == IDOK || id == IDCANCEL) {
        return false;
    }
    char class_name[16] = {};
    GetClassNameA(control, class_name, static_cast<int>(sizeof(class_name)));
    return string_iequals(class_name, "Static") || string_iequals(class_name, "Button");
}

static void level_dialog_init_tabs(HWND hdlg)
{
    HWND tabs = GetDlgItem(hdlg, IDC_LEVEL_TABS);
    if (!tabs) return;
    for (std::size_t i = 0; i < std::size(level_tab_names); ++i) {
        TCITEMA item{};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<char*>(level_tab_names[i]);
        SendMessageA(tabs, TCM_INSERTITEMA, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(&item));
    }
    // The pages overlap the tab control, which paints over any of them it sits above.
    SetWindowPos(tabs, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    level_dialog_sample_tab_pane(tabs);
    level_dialog_show_tab(hdlg, LevelTab::general);
}

static AlpineBitmapPreview g_minimap_preview;

constexpr float minimap_coord_step = 1.0f;
constexpr float minimap_coord_min = -100000.0f;
constexpr float minimap_coord_max = 100000.0f;

static void minimap_update_bitmap_preview(HWND hdlg, bool force)
{
    g_minimap_preview.update(hdlg, IDC_MINIMAP_BITMAP, IDC_MINIMAP_BITMAP_PREVIEW, force);
}

static void minimap_format_coord(float value, char (&buf)[32])
{
    std::snprintf(buf, sizeof(buf), "%.2f", value);
}

static void minimap_set_coord_field(HWND hdlg, int idc_edit, int idc_spin, float value)
{
    char buf[32];
    minimap_format_coord(value, buf);
    SetDlgItemTextA(hdlg, idc_edit, buf);
    alpine_spinner_init(hdlg, idc_edit, idc_spin, minimap_coord_step, minimap_coord_min,
                        minimap_coord_max, 2);
}

// A field still showing the stored value, rounded for display, keeps that value at full precision.
static bool minimap_field_value(HWND hdlg, int idc, float stored, float& out)
{
    char shown[32];
    minimap_format_coord(stored, shown);
    char text[32] = {};
    GetDlgItemTextA(hdlg, idc, text, static_cast<int>(sizeof(text)));
    if (!std::isnan(stored) && std::strcmp(text, shown) == 0) {
        out = std::clamp(stored, minimap_coord_min, minimap_coord_max);
        return true;
    }
    return read_dlg_float(hdlg, idc, out, minimap_coord_min, minimap_coord_max);
}

// The same value minimap_validate checked.
static void minimap_store_coord(HWND hdlg, int idc, float& value)
{
    minimap_field_value(hdlg, idc, value, value);
}

// Shows the Minimap tab first, so the field is in view when it takes the focus.
static void minimap_reject_field(HWND hdlg, int idc, const char* message)
{
    level_dialog_show_tab(hdlg, LevelTab::minimap);
    MessageBoxA(hdlg, message, "Minimap", MB_OK | MB_ICONWARNING);
    if (HWND field = GetDlgItem(hdlg, idc)) {
        SendMessageA(hdlg, WM_NEXTDLGCTL, reinterpret_cast<WPARAM>(field), TRUE);
    }
}

// Leaves `out` untouched and says which field is wrong when the text does not parse.
static bool minimap_read_coord(HWND hdlg, int idc, const char* label, float stored, float& out)
{
    if (minimap_field_value(hdlg, idc, stored, out)) {
        return true;
    }
    char msg[96];
    std::snprintf(msg, sizeof(msg), "%s is not a number.", label);
    minimap_reject_field(hdlg, idc, msg);
    return false;
}

static bool minimap_read_bounds(HWND hdlg, const AlpineLevelProperties& props, Vector3& world_min,
                                Vector3& world_max)
{
    return minimap_read_coord(hdlg, IDC_MINIMAP_MIN_X, "Min X", props.minimap_world_min.x, world_min.x) &&
           minimap_read_coord(hdlg, IDC_MINIMAP_MIN_Z, "Min Z", props.minimap_world_min.z, world_min.z) &&
           minimap_read_coord(hdlg, IDC_MINIMAP_MAX_X, "Max X", props.minimap_world_max.x, world_max.x) &&
           minimap_read_coord(hdlg, IDC_MINIMAP_MAX_Z, "Max Z", props.minimap_world_max.z, world_max.z);
}

// The game disables a minimap narrower than 1 unit on either axis.
static bool minimap_bounds_valid(HWND hdlg, const Vector3& world_min, const Vector3& world_max)
{
    if (world_max.x - world_min.x >= 1.0f && world_max.z - world_min.z >= 1.0f) {
        return true;
    }
    minimap_reject_field(hdlg, world_max.x - world_min.x >= 1.0f ? IDC_MINIMAP_MAX_Z : IDC_MINIMAP_MAX_X,
                         "World bounds need Max X and Max Z at least 1 unit above Min X and Min Z.");
    return false;
}

static std::string minimap_read_bitmap(HWND hdlg)
{
    char buf[256] = {};
    GetDlgItemTextA(hdlg, IDC_MINIMAP_BITMAP, buf, static_cast<int>(sizeof(buf)));
    return buf;
}

// Runs before stock OnOK, so a bad field keeps the dialog open.
static bool minimap_validate(HWND hdlg)
{
    const auto& props = CDedLevel::Get()->GetAlpineLevelProperties();
    Vector3 world_min{};
    Vector3 world_max{};
    float cut_height = 0.0f;
    if (!minimap_read_bounds(hdlg, props, world_min, world_max) ||
        !minimap_read_coord(hdlg, IDC_MINIMAP_CUT_HEIGHT, "Cut height", props.minimap_cut_height, cut_height)) {
        return false;
    }
    const std::string bitmap = minimap_read_bitmap(hdlg);
    if (rfl_name_over_long(bitmap) || bitmap.find_first_of("\\/:") != std::string::npos) {
        minimap_reject_field(hdlg, IDC_MINIMAP_BITMAP,
                             "The minimap bitmap must be a bare file name of at most 31 characters.");
        return false;
    }
    const bool enabled = IsDlgButtonChecked(hdlg, IDC_MINIMAP_ENABLE) == BST_CHECKED;
    if (enabled && bitmap.empty()) {
        minimap_reject_field(hdlg, IDC_MINIMAP_BITMAP,
                             "An enabled minimap needs a bitmap: pick one or bake it from the level.");
        return false;
    }
    return !enabled || minimap_bounds_valid(hdlg, world_min, world_max);
}

constexpr int minimap_bake_sizes[] = {512, 1024, 2048};
static int g_minimap_bake_size_index = 1;
static bool g_minimap_bake_use_bounds = false;

static void minimap_bake_from_dialog(HWND hdlg)
{
    auto* level = CDedLevel::Get();
    if (!level) return;

    const LRESULT sel = SendDlgItemMessageA(hdlg, IDC_MINIMAP_BAKE_RES, CB_GETCURSEL, 0, 0);
    if (sel >= 0 && sel < static_cast<LRESULT>(std::size(minimap_bake_sizes))) {
        g_minimap_bake_size_index = static_cast<int>(sel);
    }
    MinimapBakeParams params;
    params.resolution = minimap_bake_sizes[g_minimap_bake_size_index];
    auto& props = level->GetAlpineLevelProperties();
    if (!minimap_read_coord(hdlg, IDC_MINIMAP_CUT_HEIGHT, "Cut height", props.minimap_cut_height,
                            params.cut_height)) {
        return;
    }
    params.use_bounds = IsDlgButtonChecked(hdlg, IDC_MINIMAP_BAKE_USE_BOUNDS) == BST_CHECKED;
    g_minimap_bake_use_bounds = params.use_bounds;
    if (params.use_bounds && (!minimap_read_bounds(hdlg, props, params.bounds_min, params.bounds_max) ||
                              !minimap_bounds_valid(hdlg, params.bounds_min, params.bounds_max))) {
        return;
    }

    // RED sets it on brush edits and clears it at Build Geometry (0x00439A5F); a cancelled build sets it again.
    if (level->geometry_needs_rebuild &&
        MessageBoxA(hdlg,
                    "The geometry has changed since the last Build Geometry, and the bake uses the last "
                    "built geometry.\n\nBake anyway?",
                    "Bake minimap", MB_YESNO | MB_ICONQUESTION) != IDYES) {
        return;
    }

    std::string bitmap_name;
    std::string path;
    std::string error;
    if (!minimap_bake_target(bitmap_name, path, error)) {
        MessageBoxA(hdlg, error.c_str(), "Bake minimap", MB_OK | MB_ICONWARNING);
        return;
    }
    if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        const std::string prompt = path + " already exists.\n\nReplace it with a new bake?";
        if (MessageBoxA(hdlg, prompt.c_str(), "Bake minimap", MB_YESNO | MB_ICONQUESTION) != IDYES) {
            return;
        }
    }

    MinimapBakeResult result;
    HCURSOR old_cursor = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    const bool ok = minimap_bake(*level, params, result, error);
    SetCursor(old_cursor);
    if (!ok) {
        MessageBoxA(hdlg, error.c_str(), "Bake minimap", MB_OK | MB_ICONWARNING);
        return;
    }

    // The image is already on disk, so its bitmap, bounds and cut height go into the level at once:
    // no Cancel can pair the new image with the old bounds.
    props.minimap_bitmap = result.bitmap_name;
    props.minimap_world_min = result.world_min;
    props.minimap_world_max = result.world_max;
    props.minimap_cut_height = params.cut_height;
    mark_level_modified();

    SetDlgItemTextA(hdlg, IDC_MINIMAP_BITMAP, result.bitmap_name.c_str());
    minimap_set_coord_field(hdlg, IDC_MINIMAP_MIN_X, IDC_MINIMAP_MIN_X_SPIN, result.world_min.x);
    minimap_set_coord_field(hdlg, IDC_MINIMAP_MIN_Z, IDC_MINIMAP_MIN_Z_SPIN, result.world_min.z);
    minimap_set_coord_field(hdlg, IDC_MINIMAP_MAX_X, IDC_MINIMAP_MAX_X_SPIN, result.world_max.x);
    minimap_set_coord_field(hdlg, IDC_MINIMAP_MAX_Z, IDC_MINIMAP_MAX_Z_SPIN, result.world_max.z);
    minimap_update_bitmap_preview(hdlg, true);

    char cut_text[48];
    if (result.cut_applied) {
        std::snprintf(cut_text, sizeof(cut_text), "cut at Y = %.2f", result.cut_height);
    }
    else {
        std::snprintf(cut_text, sizeof(cut_text), "no cut");
    }
    const char* lighting = result.lightmaps_placeholder
                               ? "The level has no usable brush lightmaps (D3D11-only or missing), so brushes are "
                                 "unlit. Build Geometry and Calculate Lighting for lit brushes."
                           : result.faces_lightmapped > 0 ? "Brushes are lit with the level's lightmaps."
                                                          : "No brush lightmaps found: brushes are unlit.";
    char terrain_note[160] = "";
    if (result.terrains_unlit > 0) {
        std::snprintf(terrain_note, sizeof(terrain_note),
                      "\n%d terrain(s) have no baked lighting and use the level ambient and sun.",
                      result.terrains_unlit);
    }
    char stale_note[160] = "";
    if (result.terrains_stale > 0) {
        std::snprintf(stale_note, sizeof(stale_note),
                      "\n%d terrain(s) changed since the last Build Geometry are drawn as plain geometry, as the "
                      "game would. Rebuild, then bake again.",
                      result.terrains_stale);
    }
    char deco_text[96] = "";
    if (result.decorations_drawn + result.decorations_blended > 0) {
        std::snprintf(deco_text, sizeof(deco_text), " %d terrain decorations (%d drawn, %d blended),",
                      result.decorations_drawn + result.decorations_blended, result.decorations_drawn,
                      result.decorations_blended);
    }
    char summary[768];
    std::snprintf(summary, sizeof(summary),
                  "Wrote user_maps\\textures\\%s\n%d x %d, %d faces,%s %s, %.2f s.\n%s%s%s\n"
                  "The bitmap and bounds are applied to the level now; Cancel does not undo them.",
                  result.bitmap_name.c_str(), params.resolution, params.resolution, result.faces_drawn, deco_text,
                  cut_text, result.seconds, lighting, terrain_note, stale_note);
    MessageBoxA(hdlg, summary, "Bake minimap", MB_OK | MB_ICONINFORMATION);
}

static void minimap_init_controls(HWND hdlg, const AlpineLevelProperties& props)
{
    CheckDlgButton(hdlg, IDC_MINIMAP_ENABLE, props.minimap_enabled ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemTextA(hdlg, IDC_MINIMAP_BITMAP, props.minimap_bitmap.c_str());
    minimap_set_coord_field(hdlg, IDC_MINIMAP_MIN_X, IDC_MINIMAP_MIN_X_SPIN, props.minimap_world_min.x);
    minimap_set_coord_field(hdlg, IDC_MINIMAP_MIN_Z, IDC_MINIMAP_MIN_Z_SPIN, props.minimap_world_min.z);
    minimap_set_coord_field(hdlg, IDC_MINIMAP_MAX_X, IDC_MINIMAP_MAX_X_SPIN, props.minimap_world_max.x);
    minimap_set_coord_field(hdlg, IDC_MINIMAP_MAX_Z, IDC_MINIMAP_MAX_Z_SPIN, props.minimap_world_max.z);
    minimap_set_coord_field(hdlg, IDC_MINIMAP_CUT_HEIGHT, IDC_MINIMAP_CUT_HEIGHT_SPIN, props.minimap_cut_height);
    minimap_update_bitmap_preview(hdlg, true);
    for (int size : minimap_bake_sizes) {
        char label[24];
        std::snprintf(label, sizeof(label), "%d x %d", size, size);
        SendDlgItemMessageA(hdlg, IDC_MINIMAP_BAKE_RES, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
    }
    SendDlgItemMessageA(hdlg, IDC_MINIMAP_BAKE_RES, CB_SETCURSEL, g_minimap_bake_size_index, 0);
    CheckDlgButton(hdlg, IDC_MINIMAP_BAKE_USE_BOUNDS, g_minimap_bake_use_bounds ? BST_CHECKED : BST_UNCHECKED);
}

// Both lightmap combos carry their stored property value as item data, so no index-to-value
// table is needed on the way back out.
static void init_lightmap_combos(HWND hdlg, const AlpineLevelProperties& props)
{
    char label[32];

    if (HWND density = GetDlgItem(hdlg, IDC_LIGHTMAP_DENSITY)) {
        SendMessageA(density, CB_RESETCONTENT, 0, 0);
        std::snprintf(label, sizeof(label), "Default (%u)", alpine_lightmap::density_default);
        int sel = alpine_dlg_combo_add(hdlg, IDC_LIGHTMAP_DENSITY, label, 0);
        bool matched = props.lightmap_density == 0;
        const int off = alpine_dlg_combo_add(hdlg, IDC_LIGHTMAP_DENSITY, "Off", alpine_lightmap::density_off);
        if (props.lightmap_density == alpine_lightmap::density_off) {
            sel = off;
            matched = true;
        }
        static constexpr std::uint8_t density_presets[] = {2, 4, 8, 16, 32, 64, 128};
        for (std::uint8_t value : density_presets) {
            std::snprintf(label, sizeof(label), "%u", value);
            const int item = alpine_dlg_combo_add(hdlg, IDC_LIGHTMAP_DENSITY, label, value);
            if (props.lightmap_density == value) {
                sel = item;
                matched = true;
            }
        }
        if (!matched) {
            std::snprintf(label, sizeof(label), "%u (custom)", props.lightmap_density);
            sel = alpine_dlg_combo_add(hdlg, IDC_LIGHTMAP_DENSITY, label, props.lightmap_density);
        }
        SendMessageA(density, CB_SETCURSEL, sel, 0);
    }

    if (HWND compression = GetDlgItem(hdlg, IDC_LIGHTMAP_COMPRESSION)) {
        SendMessageA(compression, CB_RESETCONTENT, 0, 0);
        static const char* const mode_names[] = {"Quality", "Balanced", "Compact"};
        for (int i = 0; i < 3; i++) {
            alpine_dlg_combo_add(hdlg, IDC_LIGHTMAP_COMPRESSION, mode_names[i], i);
        }
        SendMessageA(compression, CB_SETCURSEL, std::min<int>(props.lightmap_compression, 2), 0);
    }
}

static std::uint8_t read_combo_u8(HWND hdlg, int id, std::uint8_t fallback)
{
    const LRESULT data = alpine_dlg_combo_data(hdlg, id, fallback);
    return data < 0 || data > 255 ? fallback : static_cast<std::uint8_t>(data);
}

// D3D11-only lightmaps stands in for the stock section with the surface charts, which legacy lighting
// and an Off density never bake.
static void update_lightmap_controls(HWND hdlg)
{
    const bool legacy = IsDlgButtonChecked(hdlg, IDC_LEGACY_LIGHTING) == BST_CHECKED;
    const bool off = alpine_dlg_combo_data(hdlg, IDC_LIGHTMAP_DENSITY, 0) == alpine_lightmap::density_off;
    if (HWND d3d11_only = GetDlgItem(hdlg, IDC_D3D11_ONLY_LIGHTMAPS)) {
        EnableWindow(d3d11_only, !legacy && !off);
    }
}

static WNDPROC g_level_dlg_orig_wndproc = nullptr;

static LRESULT CALLBACK LevelDialogSubclassProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    WNDPROC orig = g_level_dlg_orig_wndproc;
    if (msg == WM_COMMAND && LOWORD(wparam) == IDOK && HIWORD(wparam) == BN_CLICKED && !minimap_validate(hwnd)) {
        return 0;
    }
    if (msg == WM_NOTIFY) {
        const auto* nm = reinterpret_cast<const NMHDR*>(lparam);
        if (nm && nm->idFrom == IDC_LEVEL_TABS && nm->code == TCN_SELCHANGE) {
            const LRESULT sel = SendMessageA(nm->hwndFrom, TCM_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < static_cast<LRESULT>(std::size(level_tab_names))) {
                level_dialog_show_tab(hwnd, static_cast<LevelTab>(sel));
            }
            return 0;
        }
        if (alpine_spinner_handle_notify(hwnd, lparam)) {
            return TRUE;
        }
    }
    if ((msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLORBTN) && g_level_tab_pane_brush &&
        level_dialog_paints_on_pane(reinterpret_cast<HWND>(lparam))) {
        const auto dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        SetBkColor(dc, g_level_tab_pane_color);
        return reinterpret_cast<LRESULT>(g_level_tab_pane_brush);
    }
    if (msg == WM_ERASEBKGND && g_level_tab_pane_brush) {
        const LRESULT erased = CallWindowProcA(orig, hwnd, msg, wparam, lparam);
        level_dialog_fill_tab_pane(hwnd, reinterpret_cast<HDC>(wparam));
        return erased;
    }
    if (msg == WM_DRAWITEM) {
        const auto* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
        if (dis && dis->CtlID == IDC_MINIMAP_BITMAP_PREVIEW) {
            alpine_dlg_draw_bitmap_preview(dis->hwndItem, dis->rcItem, g_minimap_preview.handle);
            return TRUE;
        }
    }
    if (msg == WM_COMMAND && LOWORD(wparam) == IDC_MINIMAP_BITMAP && HIWORD(wparam) == EN_CHANGE) {
        minimap_update_bitmap_preview(hwnd, false);
    }
    if (msg == WM_COMMAND && LOWORD(wparam) == IDC_LEVEL_CAMERA_FAR_CLIP && HIWORD(wparam) == EN_CHANGE) {
        level_dialog_update_camera_far_clip_warning(hwnd);
    }
    if (msg == WM_COMMAND && LOWORD(wparam) == IDC_MINIMAP_BITMAP_BROWSE && HIWORD(wparam) == BN_CLICKED) {
        if (alpine_dlg_browse_bitmap(hwnd, IDC_MINIMAP_BITMAP, nullptr, g_minimap_preview.handle)) {
            minimap_update_bitmap_preview(hwnd, true);
        }
        return 0;
    }
    if (msg == WM_COMMAND && LOWORD(wparam) == IDC_MINIMAP_BAKE && HIWORD(wparam) == BN_CLICKED) {
        minimap_bake_from_dialog(hwnd);
        return 0;
    }
    if (msg == WM_COMMAND && LOWORD(wparam) == IDC_SUN_SET_FROM_CAMERA && HIWORD(wparam) == BN_CLICKED) {
        set_sun_angles_from_camera(hwnd);
        return 0;
    }
    if (msg == WM_COMMAND && LOWORD(wparam) == IDC_SUN_COLOR_CHANGE && HIWORD(wparam) == BN_CLICKED) {
        pick_sun_color(hwnd);
        return 0;
    }
    if (msg == WM_COMMAND && LOWORD(wparam) == IDC_SUN_COLOR_VALUE && HIWORD(wparam) == EN_KILLFOCUS) {
        read_sun_color_text(hwnd, g_sun_color_r, g_sun_color_g, g_sun_color_b);
        update_sun_color_controls(hwnd);
        return 0;
    }
    if (msg == WM_COMMAND && ((LOWORD(wparam) == IDC_LEGACY_LIGHTING && HIWORD(wparam) == BN_CLICKED) ||
                              (LOWORD(wparam) == IDC_LIGHTMAP_DENSITY && HIWORD(wparam) == CBN_SELCHANGE))) {
        update_lightmap_controls(hwnd);
    }
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(orig));
        g_level_dlg_orig_wndproc = nullptr;
        if (g_level_tab_pane_brush) {
            DeleteObject(g_level_tab_pane_brush);
            g_level_tab_pane_brush = nullptr;
        }
        g_level_fog_far_edit = nullptr;
        return CallWindowProcA(orig, hwnd, msg, wparam, lparam);
    }
    return CallWindowProcA(orig, hwnd, msg, wparam, lparam);
}

// load AlpineLevelProperties settings when opening level properties dialog
CodeInjection CLevelDialog_OnInitDialog_patch{
    0x004676C0,
    [](auto& regs) {
        HWND hdlg = WndToHandle(regs.esi);
        int level_version = get_level_rfl_version();
        std::string version = std::to_string(level_version);
        SetDlgItemTextA(hdlg, IDC_LEVEL_VERSION, version.c_str());

        sun_arrow_sync(CDedLevel::Get());
        auto& alpine_level_props = CDedLevel::Get()->GetAlpineLevelProperties();
        CheckDlgButton(hdlg, IDC_LEGACY_CYCLIC_TIMERS, alpine_level_props.legacy_cyclic_timers ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hdlg, IDC_LEGACY_MOVERS, alpine_level_props.legacy_movers ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hdlg, IDC_STARTS_WITH_HEADLAMP, alpine_level_props.starts_with_headlamp ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hdlg, IDC_OVERRIDE_MESH_AMBIENT_LIGHT_MODIFIER, alpine_level_props.override_static_mesh_ambient_light_modifier ? BST_CHECKED : BST_UNCHECKED);
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.3f", alpine_level_props.static_mesh_ambient_light_modifier);
        SetDlgItemTextA(hdlg, IDC_MESH_AMBIENT_LIGHT_MODIFIER, buffer);
        CheckDlgButton(hdlg, IDC_RF2_STYLE_GEOMOD, alpine_level_props.rf2_style_geomod ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hdlg, IDC_VEHICLE_FLIGHT_CEILING_ENABLE, alpine_level_props.vehicle_flight_ceiling_enabled ? BST_CHECKED : BST_UNCHECKED);
        char ceiling_buffer[32];
        std::snprintf(ceiling_buffer, sizeof(ceiling_buffer), "%.3f", alpine_level_props.vehicle_flight_ceiling);
        SetDlgItemTextA(hdlg, IDC_VEHICLE_FLIGHT_CEILING, ceiling_buffer);
        CheckDlgButton(hdlg, IDC_LEGACY_LIGHTING, alpine_level_props.legacy_lighting ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hdlg, IDC_HIGHRES_LIGHTMAPS, alpine_level_props.highres_lightmaps ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hdlg, IDC_INVISIBLE_FACES_OCCLUDE, alpine_level_props.invisible_faces_occlude ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hdlg, IDC_ALPHA_FACES_OCCLUDE, alpine_level_props.alpha_faces_occlude ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hdlg, IDC_MESHES_OCCLUDE, alpine_level_props.meshes_occlude ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hdlg, IDC_D3D11_ONLY_LIGHTMAPS,
                       alpine_level_props.d3d11_only_lightmaps ? BST_CHECKED : BST_UNCHECKED);
        init_lightmap_combos(hdlg, alpine_level_props);
        update_lightmap_controls(hdlg);
        CheckDlgButton(hdlg, IDC_REQUIRE_D3D11, alpine_level_props.require_d3d11 ? BST_CHECKED : BST_UNCHECKED);
        const CLevelDialog* dlg = regs.esi;
        level_dialog_init_camera_far_clip(hdlg, *dlg, alpine_level_props.camera_far_clip);

        CheckDlgButton(hdlg, IDC_SUN_ENABLE, alpine_level_props.enable_sun ? BST_CHECKED : BST_UNCHECKED);
        std::snprintf(buffer, sizeof(buffer), "%.3f", alpine_level_props.sun_yaw);
        SetDlgItemTextA(hdlg, IDC_SUN_YAW, buffer);
        std::snprintf(buffer, sizeof(buffer), "%.3f", alpine_level_props.sun_pitch);
        SetDlgItemTextA(hdlg, IDC_SUN_PITCH, buffer);
        std::snprintf(buffer, sizeof(buffer), "%.3f", alpine_level_props.sun_intensity);
        SetDlgItemTextA(hdlg, IDC_SUN_INTENSITY, buffer);
        std::snprintf(buffer, sizeof(buffer), "%.3f", alpine_level_props.sun_spread_angle);
        SetDlgItemTextA(hdlg, IDC_SUN_SPREAD_ANGLE, buffer);
        g_sun_color_r = alpine_level_props.sun_color_r;
        g_sun_color_g = alpine_level_props.sun_color_g;
        g_sun_color_b = alpine_level_props.sun_color_b;
        update_sun_color_controls(hdlg);
        CheckDlgButton(hdlg, IDC_SUN_CAST_BAKED_SHADOWS, alpine_level_props.sun_cast_baked_shadows ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hdlg, IDC_SUN_AFFECTS_MESHES, alpine_level_props.sun_affects_meshes ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hdlg, IDC_SUN_MESH_MODE_SCALE, alpine_level_props.sun_mesh_mode == 0 ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hdlg, IDC_SUN_DRIVES_SHADOWMAP_DIR, alpine_level_props.sun_drives_shadowmap_dir ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hdlg, IDC_SUN_LIQUID_OCCLUDES, alpine_level_props.sun_liquid_occludes ? BST_CHECKED : BST_UNCHECKED);

        minimap_init_controls(hdlg, alpine_level_props);
        level_dialog_init_tabs(hdlg);

        if (reinterpret_cast<WNDPROC>(GetWindowLongPtrA(hdlg, GWLP_WNDPROC)) != LevelDialogSubclassProc) {
            g_level_dlg_orig_wndproc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrA(hdlg, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(LevelDialogSubclassProc)));
        }
    },
};

// save AlpineLevelProperties settings when closing level properties dialog
CodeInjection CLevelDialog_OnOK_patch{
    0x00468470,
    [](auto& regs) {
        HWND hdlg = WndToHandle(regs.ecx);
        auto& alpine_level_props = CDedLevel::Get()->GetAlpineLevelProperties();
        alpine_level_props.legacy_cyclic_timers = IsDlgButtonChecked(hdlg, IDC_LEGACY_CYCLIC_TIMERS) == BST_CHECKED;
        alpine_level_props.legacy_movers = IsDlgButtonChecked(hdlg, IDC_LEGACY_MOVERS) == BST_CHECKED;
        alpine_level_props.starts_with_headlamp = IsDlgButtonChecked(hdlg, IDC_STARTS_WITH_HEADLAMP) == BST_CHECKED;
        alpine_level_props.override_static_mesh_ambient_light_modifier = IsDlgButtonChecked(hdlg, IDC_OVERRIDE_MESH_AMBIENT_LIGHT_MODIFIER) == BST_CHECKED;
        char buffer[64] = {};
        GetDlgItemTextA(hdlg, IDC_MESH_AMBIENT_LIGHT_MODIFIER, buffer, static_cast<int>(sizeof(buffer)));
        char* end = nullptr;
        float modifier = std::strtof(buffer, &end);
        if (end != buffer && std::isfinite(modifier)) {
            if (modifier < 0.0f) {
                modifier = 0.0f;
            }
            alpine_level_props.static_mesh_ambient_light_modifier = modifier;
        }
        alpine_level_props.rf2_style_geomod = IsDlgButtonChecked(hdlg, IDC_RF2_STYLE_GEOMOD) == BST_CHECKED;
        alpine_level_props.vehicle_flight_ceiling_enabled = IsDlgButtonChecked(hdlg, IDC_VEHICLE_FLIGHT_CEILING_ENABLE) == BST_CHECKED;
        read_dlg_float(hdlg, IDC_VEHICLE_FLIGHT_CEILING, alpine_level_props.vehicle_flight_ceiling, -FLT_MAX, FLT_MAX);
        alpine_level_props.legacy_lighting = IsDlgButtonChecked(hdlg, IDC_LEGACY_LIGHTING) == BST_CHECKED;
        alpine_level_props.highres_lightmaps = IsDlgButtonChecked(hdlg, IDC_HIGHRES_LIGHTMAPS) == BST_CHECKED;
        alpine_level_props.invisible_faces_occlude = IsDlgButtonChecked(hdlg, IDC_INVISIBLE_FACES_OCCLUDE) == BST_CHECKED;
        alpine_level_props.alpha_faces_occlude = IsDlgButtonChecked(hdlg, IDC_ALPHA_FACES_OCCLUDE) == BST_CHECKED;
        alpine_level_props.meshes_occlude = IsDlgButtonChecked(hdlg, IDC_MESHES_OCCLUDE) == BST_CHECKED;
        alpine_level_props.d3d11_only_lightmaps = IsDlgButtonChecked(hdlg, IDC_D3D11_ONLY_LIGHTMAPS) == BST_CHECKED;
        alpine_level_props.lightmap_density =
            read_combo_u8(hdlg, IDC_LIGHTMAP_DENSITY, alpine_level_props.lightmap_density);
        alpine_level_props.lightmap_compression =
            read_combo_u8(hdlg, IDC_LIGHTMAP_COMPRESSION, alpine_level_props.lightmap_compression);
        alpine_level_props.require_d3d11 = IsDlgButtonChecked(hdlg, IDC_REQUIRE_D3D11) == BST_CHECKED;
        alpine_level_props.camera_far_clip =
            alpine_camera_far_clip::sanitize(read_camera_far_clip_field(hdlg, alpine_level_props.camera_far_clip));
        const CLevelDialog* dlg = regs.ecx;
        level_dialog_clamp_fog_near_clip(*dlg);

        alpine_level_props.enable_sun = IsDlgButtonChecked(hdlg, IDC_SUN_ENABLE) == BST_CHECKED;
        float yaw = alpine_level_props.sun_yaw;
        if (read_dlg_float(hdlg, IDC_SUN_YAW, yaw, -FLT_MAX, FLT_MAX)) {
            yaw = std::fmod(yaw, 360.0f);
            if (yaw < 0.0f) {
                yaw += 360.0f;
            }
            alpine_level_props.sun_yaw = yaw;
        }
        read_dlg_float(hdlg, IDC_SUN_PITCH, alpine_level_props.sun_pitch, 0.0f, 90.0f);
        read_dlg_float(hdlg, IDC_SUN_INTENSITY, alpine_level_props.sun_intensity, 0.0f, 10.0f);
        read_dlg_float(hdlg, IDC_SUN_SPREAD_ANGLE, alpine_level_props.sun_spread_angle, 0.0f, 45.0f);
        read_sun_color_text(hdlg, g_sun_color_r, g_sun_color_g, g_sun_color_b);
        alpine_level_props.sun_color_r = g_sun_color_r;
        alpine_level_props.sun_color_g = g_sun_color_g;
        alpine_level_props.sun_color_b = g_sun_color_b;
        alpine_level_props.sun_cast_baked_shadows = IsDlgButtonChecked(hdlg, IDC_SUN_CAST_BAKED_SHADOWS) == BST_CHECKED;
        alpine_level_props.sun_affects_meshes = IsDlgButtonChecked(hdlg, IDC_SUN_AFFECTS_MESHES) == BST_CHECKED;
        alpine_level_props.sun_mesh_mode = IsDlgButtonChecked(hdlg, IDC_SUN_MESH_MODE_SCALE) == BST_CHECKED ? 0 : 1;
        alpine_level_props.sun_drives_shadowmap_dir = IsDlgButtonChecked(hdlg, IDC_SUN_DRIVES_SHADOWMAP_DIR) == BST_CHECKED;
        alpine_level_props.sun_liquid_occludes = IsDlgButtonChecked(hdlg, IDC_SUN_LIQUID_OCCLUDES) == BST_CHECKED;

        alpine_level_props.minimap_enabled = IsDlgButtonChecked(hdlg, IDC_MINIMAP_ENABLE) == BST_CHECKED;
        alpine_level_props.minimap_bitmap = minimap_read_bitmap(hdlg);
        minimap_store_coord(hdlg, IDC_MINIMAP_MIN_X, alpine_level_props.minimap_world_min.x);
        minimap_store_coord(hdlg, IDC_MINIMAP_MIN_Z, alpine_level_props.minimap_world_min.z);
        minimap_store_coord(hdlg, IDC_MINIMAP_MAX_X, alpine_level_props.minimap_world_max.x);
        minimap_store_coord(hdlg, IDC_MINIMAP_MAX_Z, alpine_level_props.minimap_world_max.z);
        minimap_store_coord(hdlg, IDC_MINIMAP_CUT_HEIGHT, alpine_level_props.minimap_cut_height);

        // Stock OnOK (and the menu path, 0x00402300) never marks the document modified.
        mark_level_modified();
    },
};

static bool is_link_allowed(const DedObject* src, const DedObject* dst)
{
    const auto t0 = src->type;
    const auto t1 = dst->type;

    if (t0 == DedObjectType::DED_SUN_ARROW || t1 == DedObjectType::DED_SUN_ARROW) {
        return false;
    }

    return
        t0 == DedObjectType::DED_TRIGGER ||
        t0 == DedObjectType::DED_EVENT ||
        (t0 == DedObjectType::DED_NAV_POINT && t1 == DedObjectType::DED_EVENT);
}

// Stock RED DoLink (0x00415850) stores links at DedObject+0x7C for all types.
static VArray<int>& get_link_array(DedObject* obj)
{
    return obj->links;  // +0x7C for all object types
}

// Brushes are not DedObjects and never enter level->selection; selection state lives on the
// BrushNode itself, so a brush picked alongside an object is invisible to the selection array.
static std::vector<int> collect_selected_detail_brush_uids(CDedLevel* level)
{
    std::vector<int> uids;

    BrushNode* node = level->brush_list;
    if (!node) {
        return uids;
    }
    do {
        if (node->state == BRUSH_STATE_SELECTED && node->is_detail) {
            uids.push_back(node->uid);
        }
        node = node->next;
    } while (node && node != level->brush_list);

    return uids;
}

// Link an object to selected detail brushes by UID. The object always receives the links
// whichever way the command was invoked, because a brush has no link array of its own.
static void link_object_to_detail_brushes(DedObject* object, const std::vector<int>& brush_uids)
{
    // The source rule is_link_allowed applies, minus its nav point clause: that one needs an
    // event destination, and a brush can only ever be a destination.
    if (object->type != DedObjectType::DED_TRIGGER && object->type != DedObjectType::DED_EVENT) {
        g_main_frame->DedMessageBox(
            "Links to brushes can only be created from Triggers and Events.",
            "Error",
            0
        );
        return;
    }

    auto& links = get_link_array(object);

    for (int brush_uid : brush_uids) {
        const int old_size = links.get_size();
        const int idx = links.add_if_not_exists_int(brush_uid);

        if (idx < 0) {
            xlog::warn("DoLink: Failed to add brush link src_uid={} brush_uid={}", object->uid, brush_uid);
        }
        else if (idx >= old_size) {
            xlog::debug("DoLink: Added new brush link src_uid={} -> brush_uid={}", object->uid, brush_uid);
        }
        else {
            xlog::debug("DoLink: Brush link already existed src_uid={} -> brush_uid={}", object->uid, brush_uid);
        }
    }
}

void DedLevel_DoLinkImpl(CDedLevel* level, bool reverse_link_direction)
{
    auto& sel = level->selection;
    const int count = sel.get_size();
    DedObject* primary = count > 0 ? sel[0] : nullptr;

    // An emitter can't be a link source, so linking one to a Target aims it there instead.
    if (!reverse_link_direction && count == 2 && primary && sel[1]
        && sel[1]->type == DedObjectType::DED_TARGET) {
        if (primary->type == DedObjectType::DED_BOLT_EMITTER) {
            auto* bolt = static_cast<DedBoltEmitter*>(primary);
            bolt->target_uid = sel[1]->uid;
            bolt->sync_preview();
            return;
        }
        if (primary->type == DedObjectType::DED_ROPE_EMITTER) {
            static_cast<DedRopeEmitter*>(primary)->target_uid = sel[1]->uid;
            return;
        }
    }

    // One object plus one or more detail brushes reaches here as a single-object selection,
    // which the object-only path can only report as an error.
    if (count == 1 && primary) {
        const std::vector<int> brush_uids = collect_selected_detail_brush_uids(level);
        if (!brush_uids.empty()) {
            link_object_to_detail_brushes(primary, brush_uids);
            return;
        }
    }

    if (count < 2 || !primary) {
        g_main_frame->DedMessageBox(
            "You must select at least 2 objects to create a link.",
            "Error",
            0
        );
        return;
    }

    int num_success = 0;
    std::vector<int> attempted_uids;

    for (int i = 1; i < count; ++i) {
        DedObject* src = reverse_link_direction ? sel[i] : primary;
        DedObject* dst = reverse_link_direction ? primary : sel[i];
        if (!src || !dst) {
            continue;
        }

        if (!is_link_allowed(src, dst)) {
            xlog::warn(
                "DoLink: disallowed type combination src_type={} dst_type={}",
                static_cast<int>(src->type),
                static_cast<int>(dst->type)
            );
            continue;
        }

        attempted_uids.push_back(reverse_link_direction ? src->uid : dst->uid);

        auto& src_links = get_link_array(src);
        int old_size = src_links.get_size();
        int idx = src_links.add_if_not_exists_int(dst->uid);

        if (idx < 0) {
            xlog::warn("DoLink: Failed to add link src_uid={} dst_uid={}", src->uid, dst->uid);
        }
        else if (idx >= old_size) {
            ++num_success;
            xlog::debug("DoLink: Added new link src_uid={} -> dst_uid={}", src->uid, dst->uid);
        }
        else {
            xlog::debug("DoLink: Link already existed src_uid={} -> dst_uid={}", src->uid, dst->uid);
        }
    }

    if (num_success == 0) {
        std::string uid_list;
        for (size_t i = 0; i < attempted_uids.size(); ++i) {
            if (i > 0) {
                uid_list += ", ";
            }
            uid_list += std::to_string(attempted_uids[i]);
        }

        std::string msg;
        if (!attempted_uids.empty()) {
            if (reverse_link_direction) {
                msg = "All links to selected destination UID " +
                        std::to_string(primary->uid) +
                        " from valid source UID(s) " +
                        uid_list +
                        " already exist.";
            } else {
                msg = "All links from selected source UID " +
                        std::to_string(primary->uid) +
                        " to valid destination UID(s) " +
                        uid_list +
                        " already exist.";
            }
        } else {
            if (reverse_link_direction) {
                msg = "No valid link combinations were found for selected destination UID " +
                    std::to_string(primary->uid) +
                    ".";
            } else {
                msg = "No valid link combinations were found for selected source UID " +
                    std::to_string(primary->uid) +
                    ".";
            }
        }

        g_main_frame->DedMessageBox(msg.c_str(), "Error", 0);
        return;
    }
}

void __fastcall CDedLevel_DoLink_new(CDedLevel* this_);
FunHook<decltype(CDedLevel_DoLink_new)> CDedLevel_DoLink_hook{
    0x00415850,
    CDedLevel_DoLink_new,
};
void __fastcall CDedLevel_DoLink_new(CDedLevel* this_)
{
    DedLevel_DoLinkImpl(this_, false);
}

void DedLevel_DoBackLink()
{
    DedLevel_DoLinkImpl(CDedLevel::Get(), true);
}

void ApplyLevelPatches()
{
    // handle AlpineLevelProperties chunk
    CDedLevel_construct_patch.install();
    CDedLevel_DeleteContents_hook.install();
    CDedLevel_LoadLevel_patch1.install();
    CDedLevel_LoadLevel_patch2.install();
    CDedLevel_SaveLevel_patch.install();
    CLevelDialog_OnInitDialog_patch.install();
    CLevelDialog_OnOK_patch.install();

    // Refuse a Build Geometry that the address space cannot hold
    build_geometry_start_hook.install();

    // Prevent geoable/breakable detail brushes from merging rooms with other brushes
    build_rooms_hook.install();
    adjacency_test_hook.install();
    isolate_rooms_injection.install();
    sync_brush_life_injection.install();
    skip_empty_detail_rooms_in_loop2.install();

    groom_ctor_clear_airlock_injection.install();

    // Ensure the face_id assignment phase (Phase 1 of FUN_004399b0) always runs.
    // Phase 1 assigns unique sequential face_ids to brush geometry faces, which our
    // adjacency test hook uses to identify which brush each compiled face belongs to.
    // The stock code skips Phase 1 on the first build after editor launch (flag at
    // 0x005774a0 starts at 0). Setting it to 1 ensures face_ids are always assigned.
    g_build_first_tick_pending = 1;

    // Avoid clamping lightmaps when loading rfl files
    AsmWriter{0x004A5D6A}.jmp(0x004A5D6E);

    // Fog clip spinners reach the camera far clip maximum instead of 1000 (the push of their max at 0x0040240B, 0x0040242B)
    write_mem<float>(0x0040240B + 1, alpine_camera_far_clip::max_value);
    write_mem<float>(0x0040242B + 1, alpine_camera_far_clip::max_value);

    // Default level fog color to flat black
    constexpr std::uint8_t default_fog = 0;
    write_mem<std::uint8_t>(0x0041CB07 + 1, default_fog);
    write_mem<std::uint8_t>(0x0041CB09 + 1, default_fog);
    write_mem<std::uint8_t>(0x0041CB0B + 1, default_fog);

    // Allow creating multiple links in a single operation
    CDedLevel_DoLink_hook.install();

    // Skip "objects outside of level" bounds check for some object types
    skip_alpine_objects_bounds_check.install();

    // Mark see-through textures on moving group brush faces so alpha renders in game
    flag_face_texture_traits_all_hook.install();
}
