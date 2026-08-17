#include <algorithm>
#include <cmath>
#include <format>
#include <patch_common/FunHook.h>
#include "demo.h"
#include "demo_internal.h"
#include "../gametype.h"
#include "../../hud/hud.h"
#include "../../hud/hud_internal.h"
#include "../../hud/remote_server_cfg_ui.h"
#include "../../misc/alpine_settings.h"
#include "../../misc/vote_panel.h"
#include "../../os/console.h"
#include "../../os/os.h"
#include "../../rf/bmpman.h"
#include "../../rf/gameseq.h"
#include "../../rf/gr/gr.h"
#include "../../rf/gr/gr_font.h"
#include "../../rf/item.h"
#include "../../rf/multi.h"
#include "../../rf/object.h"
#include "../../rf/os/console.h"

// Powerup respawn timers for demo playback. Clients never run item_check_for_respawns
// and the server's item_update visibility bitmap (250 ms cadence) hides/unhides items
// without setting respawn_next, so playback infers each timer from the visible->hidden
// edge and counts down respawn_time_ms against the demo clock.

namespace
{
    constexpr int max_slots_per_type = 2;

    enum class SlotState
    {
        visible,       // item is spawned - icon only, no timer
        taken_known,   // hidden with an observed pickup time - countdown
        taken_unknown, // hidden since first observation (demo started mid-cycle) or non-respawning
    };

    struct TrackedItem
    {
        int handle = -1;
        SlotState state = SlotState::visible;
        double hidden_at_ms = 0.0;
        int respawn_time_ms = 0;
    };

    struct TrackedType
    {
        const char* cls_name;
        const char* bitmap_filename;
        int info_index = -2; // -2 = unresolved, -1 = unknown class
        int bmh = -2;        // -2 = not loaded yet; survives resets
        int num_slots = 0;
        TrackedItem slots[max_slots_per_type] = {};
    };

    TrackedType g_tracked_types[] = {
        {.cls_name = "Multi Damage Amplifier", .bitmap_filename = "hud_pow_damage.tga"},
        {.cls_name = "Multi Invulnerability", .bitmap_filename = "hud_pow_invuln.tga"},
    };

    // Adopts untracked items of this type - first max_slots_per_type in list order
    // (deterministic); extras are ignored. An item first seen hidden has no known
    // pickup time.
    void adopt_untracked_items(TrackedType& type)
    {
        for (rf::Item* item = rf::item_list.next; item != &rf::item_list && type.num_slots < max_slots_per_type;
             item = item->next) {
            if (item->info_index != type.info_index) {
                continue;
            }
            auto* const tracked_end = type.slots + type.num_slots;
            const bool tracked = std::any_of(type.slots, tracked_end,
                [item](const TrackedItem& slot) { return slot.handle == item->handle; });
            if (tracked) {
                continue;
            }
            TrackedItem& slot = type.slots[type.num_slots++];
            slot.handle = item->handle;
            slot.state = (item->obj_flags & rf::OF_HIDDEN) != 0 ? SlotState::taken_unknown : SlotState::visible;
            slot.respawn_time_ms = item->respawn_time_ms;
        }
    }

    void update_visibility_edges(TrackedType& type, double now_ms)
    {
        for (int i = 0; i < type.num_slots;) {
            TrackedItem& slot = type.slots[i];
            rf::Object* obj = rf::obj_from_handle(slot.handle);
            if (!obj || obj->type != rf::OT_ITEM) {
                slot = type.slots[--type.num_slots]; // item object is gone - drop the slot
                continue;
            }
            const bool hidden = (obj->obj_flags & rf::OF_HIDDEN) != 0;
            if (slot.state == SlotState::visible && hidden) {
                auto* item = static_cast<rf::Item*>(obj);
                slot.respawn_time_ms = item->respawn_time_ms;
                if (item->respawn_time_ms > 0) {
                    slot.state = SlotState::taken_known;
                    slot.hidden_at_ms = now_ms;
                }
                else {
                    slot.state = SlotState::taken_unknown;
                }
            }
            else if (slot.state != SlotState::visible && !hidden) {
                slot.state = SlotState::visible;
            }
            ++i;
        }
    }

    void update_tracker()
    {
        const double now_ms = demo_playback_clock_ms();
        for (TrackedType& type : g_tracked_types) {
            if (type.info_index == -2) {
                type.info_index = rf::item_lookup_type(type.cls_name);
            }
            if (type.info_index < 0) {
                continue;
            }
            adopt_untracked_items(type);
            update_visibility_edges(type, now_ms);
        }
    }

    using MultiIoPacketHandler = void(char* data, const rf::NetAddr& addr);

    // The item_update bitmap is the only channel through which a spectator learns about
    // level-item pickups/respawns, so its handler is the natural edge-detection point.
    // Correct through seek fast-forward bursts too: the pump advances the demo clock
    // per-record before feeding. Pure passthrough outside demo playback.
    FunHook<MultiIoPacketHandler> process_item_update_packet_hook{
        0x0047A220,
        [](char* data, const rf::NetAddr& addr) {
            process_item_update_packet_hook.call_target(data, addr);
            if (demo_playback_active() && !gt_is_bagman_any()) {
                update_tracker();
            }
        },
    };

    ConsoleCommand2 spectate_powerups_cmd{
        "spectate_powerups",
        []() {
            g_alpine_game_config.demo_powerup_timers = !g_alpine_game_config.demo_powerup_timers;
            rf::console::print("Demo playback powerup respawn timers are {}",
                               g_alpine_game_config.demo_powerup_timers ? "enabled" : "disabled");
        },
        "Toggle powerup respawn timers during demo playback",
        "spectate_powerups",
    };
}

void demo_powerup_timers_reset()
{
    for (TrackedType& type : g_tracked_types) {
        type.info_index = -2;
        type.num_slots = 0;
    }
}

void demo_powerup_timers_render()
{
    if (!demo_playback_active() || rf::is_dedicated_server) {
        return;
    }
    if (!g_alpine_game_config.demo_powerup_timers) {
        return;
    }
    // Bagman uses a damage amp item as the bag object - no powerup tracking there
    if (gt_is_bagman_any()) {
        return;
    }
    if (!rf::gameseq_in_gameplay() || demo_playback_is_seeking()) {
        return;
    }
    if (is_hud_effectively_hidden() || g_alpine_game_config.spectate_mode_minimal_ui) {
        return;
    }
    if (g_remote_server_cfg_popup.is_active() || vote_panel_is_gameplay_overlay_active()) {
        return;
    }

    const float scale = g_alpine_game_config.big_hud ? 1.875f : 1.0f;

    // Anchor below the FPS/Speed/Ping counter stack, which itself sits below the ammo
    // cluster (top-right)
    int row_y = frametime_hud_counter_stack_bottom_y() + static_cast<int>(12 * scale);
    const int margin_x = std::max(8, static_cast<int>(12 * scale));
    const int right_x = rf::gr::screen_width() - margin_x;
    const int gap = static_cast<int>(4 * scale);
    const int font = hud_get_default_font();
    const int font_h = rf::gr::get_font_height(font);
    const double now_ms = demo_playback_clock_ms();

    for (TrackedType& type : g_tracked_types) {
        if (type.bmh == -2) {
            type.bmh = rf::bm::load(type.bitmap_filename, -1, true);
        }
        if (type.bmh < 0) {
            continue;
        }
        int bm_w = 0, bm_h = 0;
        rf::bm::get_dimensions(type.bmh, &bm_w, &bm_h);
        const int icon_w = static_cast<int>(bm_w * scale);
        const int icon_h = static_cast<int>(bm_h * scale);

        for (int i = 0; i < type.num_slots; ++i) {
            const TrackedItem& slot = type.slots[i];
            if (slot.state == SlotState::visible) {
                rf::gr::set_color(255, 255, 255, 255);
            }
            else {
                rf::gr::set_color(255, 255, 255, 96);
            }
            hud_scaled_bitmap(type.bmh, right_x - icon_w, row_y, scale);

            if (slot.state == SlotState::taken_known) {
                const double remaining_ms = std::max(slot.respawn_time_ms - (now_ms - slot.hidden_at_ms), 0.0);
                auto seconds = static_cast<int>(std::ceil(remaining_ms / 1000.0));
                std::string text = std::format("{}", seconds);
                const int text_y = row_y + ((icon_h - font_h) / 2);
                rf::gr::set_color(255, 255, 255, 225);
                rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, right_x - icon_w - gap, text_y, text.c_str(), font);
            }
            row_y += icon_h + gap;
        }
    }
}

void demo_powerup_timers_do_patch()
{
    process_item_update_packet_hook.install();
    spectate_powerups_cmd.register_cmd();
}
