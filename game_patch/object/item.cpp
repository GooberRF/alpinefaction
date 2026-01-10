#include <patch_common/FunHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>
#include <common/utils/string-utils.h>
#include <xlog/xlog.h>
#include <algorithm>
#include <string_view>
#include "../rf/event.h"
#include "../rf/item.h"
#include "../rf/misc.h"
#include "../rf/entity.h"
#include "../rf/multi.h"
#include "../rf/weapon.h"
#include "../rf/player/player.h"
#include "../misc/achievements.h"
#include "../misc/misc.h"
#include "../multi/server.h"

int item_lookup_type(const char* name)
{
    if (!name || rf::num_item_types <= 0)
        return -1;

    auto name_view = std::string_view(name);
    auto it =
        std::find_if(std::begin(rf::item_info), std::begin(rf::item_info) + rf::num_item_types, [name_view](const rf::ItemInfo& item) {
            return string_iequals(name_view, item.cls_name);
        });

    return (it != std::begin(rf::item_info) + rf::num_item_types)
        ? static_cast<int>(std::distance(std::begin(rf::item_info), it))
        : -1;
}

FunHook<int(int, int, int, int)> item_touch_weapon_hook{
    0x0045A6D0,
    [](int entity_handle, int item_handle, int weapon_type, int count) {
        if (server_weapon_items_give_full_ammo() && weapon_type != rf::shoulder_cannon_weapon_type) {
            rf::WeaponInfo& winfo = rf::weapon_types[weapon_type];
            count = winfo.max_ammo + winfo.clip_size;
        }
        return item_touch_weapon_hook.call_target(entity_handle, item_handle, weapon_type, count);
    },
};

CodeInjection item_create_sort_injection{
    0x004593AC,
    [](auto& regs) {
        rf::Item* item = regs.esi;
        rf::VMesh* vmesh = item->vmesh;
        const char* mesh_name = vmesh ? rf::vmesh_get_name(vmesh) : nullptr;
        if (!mesh_name) {
            // Sometimes on level change some objects can stay and have only vmesh destroyed
            return;
        }
        std::string_view mesh_name_sv = mesh_name;

        // HACKFIX: enable alpha sorting for Invulnerability Powerup and Riot Shield
        // Note: material used for alpha-blending is flare_blue1.tga - it uses non-alpha texture
        // so information about alpha-blending cannot be taken from material alone - it must be read from VFX
        static const char* force_alpha_mesh_names[] = {
            "powerup_invuln.vfx",
            "Weapon_RiotShield.V3D",
        };
        for (const char* alpha_mesh_name : force_alpha_mesh_names) {
            if (mesh_name_sv == alpha_mesh_name) {
                item->obj_flags |= rf::OF_HAS_ALPHA;
                break;
            }
        }

        rf::Item* current = rf::item_list.next;
        while (current != &rf::item_list) {
            rf::VMesh* current_anim_mesh = current->vmesh;
            const char* current_mesh_name = current_anim_mesh ? rf::vmesh_get_name(current_anim_mesh) : nullptr;
            if (current_mesh_name && mesh_name_sv == current_mesh_name) {
                break;
            }
            current = current->next;
        }
        item->next = current;
        item->prev = current->prev;
        item->next->prev = item;
        item->prev->next = item;
        // Set up needed registers
        regs.ecx = regs.esp + 0xC0 - 0xB0; // create_info
        regs.eip = 0x004593D1;
    },
};

CodeInjection game_level_init_pre_patch{
    0x00435BDC, [](auto& regs) {

        if (!rf::is_multi) {
            rf::multi_powerup_destroy_all();
        }
    }
};

CodeInjection item_pickup_patch {
    0x004597BA,
    [](auto& regs) {
        if (!rf::is_multi) {
            bool picked_up = regs.eax != -1;

            if (picked_up)
            {
                rf::Item* item = regs.esi;
                rf::Entity* entity = regs.edi;
                if (item && entity && entity == rf::local_player_entity) {
                    //xlog::warn("player {} picked up item {}, uid {}, handle {}", entity->name, item->name, item->uid, item->handle);
                    if (is_achievement_system_initialized()) {
                        achievement_check_item_picked_up(item);
                    }

                    rf::activate_all_events_of_type(rf::EventType::When_Picked_Up, item->handle, entity->handle, true);
                }
            }
        }
    }
};

CodeInjection item_pickup_patch2 {
    0x004597D8,
    [](auto& regs) {
        if (!rf::is_multi) {
            bool picked_up = regs.eax != -1;

            if (picked_up)
            {
                rf::Item* item = regs.esi;
                rf::Entity* entity = regs.edi;
                if (item && entity && entity == rf::local_player_entity) {
                    //xlog::warn("player {} picked up item {}, uid {}, handle {}", entity->name, item->name, item->uid, item->handle);
                    if (is_achievement_system_initialized()) {
                        achievement_check_item_picked_up(item);
                    }

                    rf::activate_all_events_of_type(rf::EventType::When_Picked_Up, item->handle, entity->handle, true);
                }
            }
        }
    }
};

CodeInjection item_touch_amp_multi_check {
    0x0045AAFD,
    [](auto& regs) {
        if (!rf::is_multi && af_rfl_version(rf::level.version)) {
            regs.eip = 0x0045AB11;
        }
    }
};

CodeInjection multi_powerup_add_mp_check {
    0x004800B5,
    [](auto& regs) {
        // eax bool is true if powerup has an associated timer (ie. amp/invuln)
        // for some reason, only for powerups without timer, game checks again here for mp
        if (!static_cast<bool>(regs.eax.value) && !rf::is_multi) {
            regs.eip = 0x00480135;
        }
    }
};

void item_do_patch()
{
    // activate When_Picked_Up events
    item_pickup_patch.install();
    item_pickup_patch2.install();

    // allow use of MP powerups in SP in alpine levels
    item_touch_amp_multi_check.install();   // touch
    multi_powerup_add_mp_check.install();   // apply effect
    game_level_init_pre_patch.install();    // initialize powerup vars

    // Allow overriding weapon items count value
    item_touch_weapon_hook.install();

    // Sort objects by mesh name to improve rendering performance
    item_create_sort_injection.install();
}
