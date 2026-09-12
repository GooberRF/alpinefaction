#include <cstdint>
#include <new>
#include <xlog/xlog.h>
#include <patch_common/FunHook.h>
#include "../rf/object.h"
#include "../os/console.h"
#include "../os/os.h"
#include "obj_collision.h"

constexpr int initial_chunk_nodes = 8192;
constexpr int growth_chunk_nodes = 4096;
constexpr int max_total_nodes = 1 << 20;

static int g_total_nodes = 0;

static bool collision_pairs_grow(int count)
{
    if (g_total_nodes + count > max_total_nodes) {
        static bool warned_cap = false;
        if (!warned_cap) {
            warned_cap = true;
            xlog::warn("Collision pair pool reached cap of {} nodes",
                max_total_nodes);
        }
        return false;
    }

    // never freed - the engine keeps pointers to pair nodes for the process lifetime
    auto* nodes = new (std::nothrow) rf::ObjCollisionPair[count]();
    if (!nodes) {
        static int64_t last_alloc_warn_ms = -5000;
        const int64_t now_ms = timer::get_i64(1000);
        if (now_ms - last_alloc_warn_ms >= 5000) {
            last_alloc_warn_ms = now_ms;
            xlog::warn("Collision pair pool allocation failed at {} nodes",
                g_total_nodes);
        }
        return false;
    }

    for (int i = 0; i < count; ++i) {
        rf::obj_collision_pair_free_list.push(&nodes[i]);
    }

    g_total_nodes += count;
    xlog::info("Collision pair pool grown to {} nodes", g_total_nodes);
    return true;
}

FunHook<void()> obj_collision_pairs_init_hook{
    0x0048C950,
    []() {
        collision_pairs_grow(initial_chunk_nodes);
    },
};

FunHook<bool(rf::Object*, rf::Object*)> obj_collision_pair_create_hook{
    0x0048BD80,
    [](rf::Object* a, rf::Object* b) {
        // grown before the filter runs; at worst one chunk is allocated early
        if (!rf::obj_collision_pair_free_list.head) {
            collision_pairs_grow(growth_chunk_nodes);
        }
        return obj_collision_pair_create_hook.call_target(a, b);
    },
};

ConsoleCommand2 collision_pairs_cmd{
    "dbg_collision_pairs",
    []() {
        rf::console::print("Collision pairs: active {}, free {}, allocated {} (hard cap {})",
            rf::obj_collision_pair_active_list.count, rf::obj_collision_pair_free_list.count, g_total_nodes, max_total_nodes);
    },
    "Prints object collision pair pool statistics",
};

void obj_collision_apply_patch()
{
    // Replace the fixed 8192 node pair pool with a growable one
    obj_collision_pairs_init_hook.install();
    obj_collision_pair_create_hook.install();

    collision_pairs_cmd.register_cmd();
}
