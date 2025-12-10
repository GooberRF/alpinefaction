#include <cassert>
#include <xlog/xlog.h>
#include <patch_common/FunHook.h>
#include <patch_common/CallHook.h>
#include <patch_common/AsmWriter.h>
#include <patch_common/CodeInjection.h>
#include <common/utils/list-utils.h>
#include "../misc/misc.h"
#include "../misc/level.h"
#include "../misc/alpine_settings.h"
#include "../rf/gr/gr_light.h"
#include "../rf/object.h"
#include "../rf/event.h"
#include "../rf/mover.h"
#include "../rf/item.h"
#include "../rf/level.h"
#include "../rf/clutter.h"
#include "../rf/gr/gr.h"
#include "../graphics/d3d11/gr_d3d11_mesh.h"
#include "../rf/multi.h"
#include "../rf/crt.h"
#include "../os/console.h"
#include "../main/main.h"
#include "../multi/multi.h"

void gr_light_use_static(bool use_static);

bool server_side_restrict_disable_muzzle_flash = false;

// support fullbright character meshes
static float g_character_ambient_light_r = 1.0f;
static float g_character_ambient_light_g = 1.0f;
static float g_character_ambient_light_b = 1.0f;
bool g_character_meshes_are_fullbright = false;

void obj_mesh_lighting_alloc_one(rf::Object* objp)
{
    if ((objp->type != rf::OT_ITEM && objp->type != rf::OT_CLUTTER) ||
        !objp->vmesh ||
        (objp->obj_flags & rf::OF_DELAYED_DELETE) ||
        rf::vmesh_get_type(objp->vmesh) != rf::MESH_TYPE_STATIC) {
        return;
    }
    assert(objp->mesh_lighting_data == nullptr);
    auto size = rf::vmesh_calc_lighting_data_size(objp->vmesh);
    objp->mesh_lighting_data = rf::rf_malloc(size);
}

void obj_mesh_lighting_free_one(rf::Object* objp)
{
    if (objp->mesh_lighting_data) {
        rf::rf_free(objp->mesh_lighting_data);
        objp->mesh_lighting_data = nullptr;
    }
}

void obj_mesh_lighting_update_one(rf::Object* objp)
{
    if (!objp->mesh_lighting_data)
        return;

    if (g_alpine_game_config.mesh_static_lighting) {
        gr_light_use_static(true);
    }

    rf::vmesh_update_lighting_data(objp->vmesh, objp->room, objp->pos, objp->orient, objp->mesh_lighting_data);

    if (g_alpine_game_config.mesh_static_lighting)
        gr_light_use_static(false);
}

void obj_mesh_lighting_maybe_update(rf::Object* objp)
{
    if (!objp->mesh_lighting_data) {
        obj_mesh_lighting_alloc_one(objp);
        obj_mesh_lighting_update_one(objp);
    }
}

void recalc_mesh_static_lighting()
{
    rf::obj_light_free();
    rf::obj_light_alloc();
    rf::obj_light_calculate();
}

void evaluate_fullbright_meshes()
{
    if (!rf::LEVEL_LOADED)
        return;

    bool previous_fullbright_state = g_character_meshes_are_fullbright;
    g_character_meshes_are_fullbright = false; // reset

    if (g_alpine_game_config.try_fullbright_characters) {
        bool server_side_restrict_fb_mesh =
            rf::is_multi && !rf::is_server && get_af_server_info() && !get_af_server_info()->allow_fb_mesh;

        if (server_side_restrict_fb_mesh) {
            rf::console::print("This server does not allow you to force fullbright meshes!");
        }
        else {
            g_character_meshes_are_fullbright = true;
        }
    }

    if (g_game_config.renderer == GameConfig::Renderer::d3d11) {
        if (g_character_meshes_are_fullbright != previous_fullbright_state) {
            df::gr::d3d11::on_character_fullbright_state_changed();
        }
    }
    else { // d3d9
        // Use fullbright (1.0) for each channel if selected and allowed, otherwise use level ambient light
        if (g_character_meshes_are_fullbright) {
            g_character_ambient_light_r = 1.0f;
            g_character_ambient_light_g = 1.0f;
            g_character_ambient_light_b = 1.0f;
        }
        else {
            // sets all 3 g_character_ambient_light floats
            std::memcpy(&g_character_ambient_light_r, reinterpret_cast<const void*>(0x005A38D4), sizeof(float) * 3);
        }
    }
}

CodeInjection vmesh_update_lighting_scale_patch{
    0x00504290,
    [](auto& regs) {
        const auto& level_props = AlpineLevelProperties::instance();
        if (!level_props.override_static_mesh_ambient_light_modifier) {
            return;
        }

        const float scale = static_cast<float>(level_props.static_mesh_ambient_light_modifier);
        auto esp_value = static_cast<std::uintptr_t>(regs.esp);
        auto* scale_ptr = reinterpret_cast<float*>(esp_value);
        *scale_ptr = scale;
    }
};

FunHook<void()> obj_light_calculate_hook{
    0x0048B0E0,
    []() {
        xlog::trace("update_mesh_lighting_hook");
        // Init transform for lighting calculation
        rf::gr::view_matrix.make_identity();
        rf::gr::view_pos.zero();
        rf::gr::light_matrix.make_identity();
        rf::gr::light_base.zero();

        for (auto& it : DoublyLinkedList{rf::item_list}) {
            obj_mesh_lighting_update_one(&it);
        }

        for (auto& cl : DoublyLinkedList{rf::clutter_list}) {
            obj_mesh_lighting_update_one(&cl);
        }
    },
};

FunHook<void()> obj_light_alloc_hook{
    0x0048B1D0,
    []() {
        for (auto& it : DoublyLinkedList{rf::item_list}) {
            obj_mesh_lighting_alloc_one(&it);
        }

        for (auto& cl : DoublyLinkedList{rf::clutter_list}) {
            obj_mesh_lighting_alloc_one(&cl);
        }
    },
};

FunHook<void()> obj_light_free_hook{
    0x0048B370,
    []() {
        for (auto& it : DoublyLinkedList{rf::item_list}) {
            obj_mesh_lighting_free_one(&it);
        }

        for (auto& cl : DoublyLinkedList{rf::clutter_list}) {
            obj_mesh_lighting_free_one(&cl);
        }
    },
};

ConsoleCommand2 mesh_static_lighting_cmd{
    "r_meshlighting",
    []() {
        g_alpine_game_config.mesh_static_lighting = !g_alpine_game_config.mesh_static_lighting;
        recalc_mesh_static_lighting();
        rf::console::print("Mesh static lighting is {}", g_alpine_game_config.mesh_static_lighting ? "enabled" : "disabled");
    },
    "Toggle mesh static lighting calculation",
};

CallHook<void(rf::Entity&)> entity_update_muzzle_flash_light_hook{
    0x0041E814,
    [](rf::Entity& ep) {
        if (g_alpine_game_config.try_disable_muzzle_flash_lights && !server_side_restrict_disable_muzzle_flash) {
            return;
        }
        entity_update_muzzle_flash_light_hook.call_target(ep);
    },
};

void evaluate_restrict_disable_muzzle_flash()
{
    server_side_restrict_disable_muzzle_flash =
        rf::is_multi && !rf::is_server && get_af_server_info() && !get_af_server_info()->allow_no_mf;

    if (server_side_restrict_disable_muzzle_flash) {
        if (g_alpine_game_config.try_disable_muzzle_flash_lights) {
            rf::console::print("This server does not allow you to disable muzzle flash lights!");
        }
    }
}

ConsoleCommand2 muzzle_flash_cmd{
    "r_muzzleflash",
    []() {
        g_alpine_game_config.try_disable_muzzle_flash_lights = !g_alpine_game_config.try_disable_muzzle_flash_lights;

        evaluate_restrict_disable_muzzle_flash();

        rf::console::print("Muzzle flash lights are {}",
                           g_alpine_game_config.try_disable_muzzle_flash_lights
                               ? "disabled. In multiplayer, this will only apply if the server allows it."
                               : "enabled.");
    },
    "Toggle muzzle flash lights. In multiplayer, this is only applied if the server allows it.",
};

ConsoleCommand2 fullbright_models_cmd{
    "r_fullbright",
    []() {
        g_alpine_game_config.try_fullbright_characters = !g_alpine_game_config.try_fullbright_characters;

        evaluate_fullbright_meshes();    
                
        rf::console::print("Fullbright character meshes are {}", g_alpine_game_config.try_fullbright_characters ?
			"enabled. In multiplayer, this will only apply if the server allows it." : "disabled.");
    },
    "Toggle fullbright character meshes. In multiplayer, this is only available if the server allows it.",
};

CodeInjection dynamic_light_load_patch{
    0x0045F500,
    [](auto& regs) {
        // Note will crash dedicated servers
        if (!rf::is_dedicated_server && af_rfl_version(rf::level.version)) {
            regs.eip = 0x0045F507;
        }
    }
};

void obj_light_apply_patch()
{
    // Support fullbright character meshes
    AsmWriter{0x0052DB3E}.fld<float>(AsmRegMem(&g_character_ambient_light_r));
    AsmWriter{0x0052DB50}.fld<float>(AsmRegMem(&g_character_ambient_light_g));
    AsmWriter{0x0052DB62}.fld<float>(AsmRegMem(&g_character_ambient_light_b));

    // Allow dynamic lights in levels
    dynamic_light_load_patch.install(); // in LevelLight__load

    // Allow level to override static mesh ambient light scale
    vmesh_update_lighting_scale_patch.install();

    // Fix/improve items and clutters static lighting calculation: fix matrices being zero and use static lights
    obj_light_calculate_hook.install();
    obj_light_alloc_hook.install();
    obj_light_free_hook.install();

    // Fix invalid vertex offset in mesh lighting calculation
    write_mem<int8_t>(0x005042F0 + 2, sizeof(rf::Vector3));

    // Don't create muzzle flash lights
    entity_update_muzzle_flash_light_hook.install();

    // Commands
    mesh_static_lighting_cmd.register_cmd();
    muzzle_flash_cmd.register_cmd();
    fullbright_models_cmd.register_cmd();
}
