#include <xlog/xlog.h>
#include <patch_common/AsmOpcodes.h>
#include <patch_common/AsmWriter.h>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/FunHook.h>
#include "../os/console.h"
#include "../rf/multi.h"
#include "../rf/level.h"
#include "../rf/file/file.h"
#include "../rf/mover.h"
#include "level.h"
#include "player.h"
#include "../multi/server.h"

CodeInjection level_read_data_check_restore_status_patch{
    0x00461195,
    [](auto& regs) {
        // check if sr_load_level_state is successful
        if (regs.eax != 0)
            return;
        // check if this is auto-load when changing level
        const char* save_filename = regs.edi;
        if (!std::strcmp(save_filename, "auto.svl"))
            return;
        // manual load failed
        xlog::error("Restoring game state failed");
        char* error_info = *reinterpret_cast<char**>(regs.esp + 0x2B0 + 0xC);
        std::strcpy(error_info, "Save file is corrupted");
        // return to level_read_data failure path
        regs.eip = 0x004608CC;
    },
};

CodeInjection level_read_moving_group_travel_time_patch{
    0x00463BF3,
    [](auto& regs) {
        if (AlpineLevelProperties::instance().legacy_movers) {
            rf::MoverCreateInfo* mci = regs.edx;
            // "Force Orient" on translating movers crashes the stock game, so remove it if set just to avoid the crash
            // In Alpine mode, this is fixed so it works properly so the flag shouldn't be removed
            if (!mci->rotate_in_place) {
                mci->force_orient = false;
            }
            return; // legacy behaviour
        }
        else {
            rf::MoverCreateInfo* mci = regs.edx;
            // if "Use Travel Time As Velocity" is used, allow accel/decel values to exceed travel time
            if (!mci->time_is_velocity) {
                for (int i = 0; i < mci->keyframes.size(); ++i) {
                    if (auto kf = mci->keyframes.get(i)) {
                        const float sum_ramp_times = kf->ramp_up_time_seconds + kf->ramp_down_time_seconds;
                        kf->forward_time_seconds = std::max(kf->forward_time_seconds, sum_ramp_times);
                        kf->reverse_time_seconds = std::max(kf->reverse_time_seconds, sum_ramp_times);
                    }
                }
            }
            // "Start Moving Backward" doesn't behave correctly for rotating movers, and it's not necessary
            if (mci->rotate_in_place) {
                mci->is_moving_backwards = false;
            }
        }
    }
};

CodeInjection level_read_moving_group_lift_patch{
    0x00463B35,
    [](auto& regs) {
        if (AlpineLevelProperties::instance().legacy_movers) {
            return; // legacy behaviour
        }
        else {
            regs.eip = 0x00463B3D;
        }
    }
};

CodeInjection level_load_items_crash_fix{
    0x0046519F,
    [](auto& regs) {
        void* item = regs.eax;
        if (item == nullptr) {
            regs.eip = 0x004651C6;
        }
    },
};

CallHook<void(rf::Vector3*, float, float, float, float, bool, int, int)> level_read_geometry_header_light_add_directional_hook{
    0x004619E1,
    [](rf::Vector3 *dir, float intensity, float r, float g, float b, bool is_dynamic, int casts_shadow, int dropoff_type) {
        if (rf::gr::lighting_enabled()) {
            level_read_geometry_header_light_add_directional_hook.call_target(dir, intensity, r, g, b, is_dynamic, casts_shadow, dropoff_type);
        }
    },
};

CodeInjection level_load_init_patch{
    0x00460860,
    []() {
        AlpineLevelProperties::instance() = {};
        DashLevelProps::instance() = {};
        set_headlamp_toggle_enabled(AlpineLevelProperties::instance().starts_with_headlamp);
    },
};

CodeInjection level_load_chunk_patch{
    0x00460912,
    [](auto& regs) {
        int chunk_id = regs.eax;
        rf::File& file = addr_as_ref<rf::File>(regs.esp + 0x2B0 - 0x278);
        auto chunk_len = addr_as_ref<std::size_t>(regs.esp + 0x2B0 - 0x2A0);

        // handling for alpine level props chunk
        if (chunk_id == alpine_props_chunk_id) {
            AlpineLevelProperties::instance().deserialize(file, chunk_len);
            set_headlamp_toggle_enabled(AlpineLevelProperties::instance().starts_with_headlamp);
            regs.eip = 0x004608EF; // loop back to begin next chunk
        }

        // handling for dash faction level props chunk, safe up to v1
        if (chunk_id == dash_level_props_chunk_id) {
            auto version = file.read<std::uint32_t>();
            if (version == 1) {
                DashLevelProps::instance().deserialize(file);
            } else {
                file.seek(chunk_len - 4, rf::File::seek_cur);
            }
            regs.eip = 0x004608EF;
        }
    },
};

FunHook<void(rf::File* file)> level_read_mp_respawns_hook{
    0x00462B20,
    [](rf::File* file) {
        rf::Vector3 pos;
        rf::Matrix3 orient;
        rf::String script_name;
        rf::String tmp_string; // only in rfl version < 67
        int count = file->read_int(0, 0);

        for (int j = 0; j < count; ++j) {
            int uid = file->read_int(0, 0);
            file->read_vector(&pos, 0, &rf::file_default_vector);
            file->read_matrix(&orient, 0, &rf::file_default_matrix);
            file->read_string(&script_name, 0, 0);
            if (file->get_version() < 67)
                file->read_string(&tmp_string, 0, 0); // unused
            bool unk1 = file->read_bool(0, true);
            int team = file->read_int(0, 0);
            bool red = file->read_bool(172, true);
            bool blue = file->read_bool(172, true);
            bool bot = file->read_bool(172, false);

            //xlog::warn("[{}] uid={} name='{}' unk1={} team={} red={} blue={} bot={} pos=({}, {}, {})", j, uid, script_name.c_str(), unk1, team, red, blue, bot, pos.x, pos.y, pos.z);

            multi_create_alpine_respawn_point(uid, script_name.c_str(), pos, orient, red, blue, true);
        }
    },
};

void level_apply_patch()
{
    // Add checking if restoring game state from save file failed during level loading
    level_read_data_check_restore_status_patch.install();

    // Fix impossible mover timing values and allow lift type movers
    level_read_moving_group_travel_time_patch.install();
    level_read_moving_group_lift_patch.install();

    // Fix item_create null result handling in RFL loading (affects multiplayer only)
    level_load_items_crash_fix.install();

    // Fix dedicated server crash when loading level that uses directional light
    level_read_geometry_header_light_add_directional_hook.install();

    // Load new rfl chunks
    level_load_init_patch.install();
    level_load_chunk_patch.install();

    // Load MP respawns
    level_read_mp_respawns_hook.install();
}
