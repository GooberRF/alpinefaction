#include <algorithm>
#include <optional>
#include <type_traits>
#include "bind_context.h"
#include "../hud/multi_spectate.h"
#include "../multi/demo/demo.h"
#include "../multi/vehicles/vehicle.h"
#include "../rf/entity.h"
#include "../rf/gameseq.h"
#include "../rf/input.h"
#include "../rf/player/control_config.h"
#include "../rf/player/player.h"

static_assert(bind_context_max_bindings == std::extent_v<decltype(rf::ControlConfig::bindings)>);

namespace
{

constexpr int ctx_count = static_cast<int>(BindContextId::_count);

std::optional<BindOverride> g_overrides[ctx_count][bind_context_max_bindings];
// Global values displaced by the applied context, so restore() can put them back and the ini
// saver can still see what Global really holds.
BindOverride g_shadow[bind_context_max_bindings];
bool g_shadowed[bind_context_max_bindings];
BindContextId g_active = BindContextId::Global;
int g_suspend_depth = 0;

void read_item(const rf::ControlConfigItem& item, BindOverride& out)
{
    out.scan_codes[0] = item.scan_codes[0];
    out.scan_codes[1] = item.scan_codes[1];
    out.mouse_btn_id = item.mouse_btn_id;
}

void write_item(rf::ControlConfigItem& item, const BindOverride& value)
{
    item.scan_codes[0] = value.scan_codes[0];
    item.scan_codes[1] = value.scan_codes[1];
    item.mouse_btn_id = value.mouse_btn_id;
}

// Writes only the fields the context owns; the item already holds Global, so an inherited field
// is simply left alone. Only valid on a restored table.
void compose_item(rf::ControlConfigItem& item, const BindOverride& value)
{
    for (int j = 0; j < 2; ++j) {
        if (value.scan_codes[j] != bind_inherit) {
            item.scan_codes[j] = value.scan_codes[j];
        }
    }
    if (value.mouse_btn_id != bind_inherit) {
        item.mouse_btn_id = value.mouse_btn_id;
    }
}

// A key that no live binding reads keeps counting presses until something consumes them, so
// one pressed before a switch would fire its new action once the switch binds it.
void drain_key_presses(const rf::ControlConfigItem& item)
{
    for (const int16_t scan : item.scan_codes) {
        if (scan >= 0) {
            rf::key_get_and_reset_down_counter(static_cast<rf::Key>(scan));
        }
    }
}

bool all_inherit(const BindOverride& value)
{
    return value.scan_codes[0] == bind_inherit && value.scan_codes[1] == bind_inherit
        && value.mouse_btn_id == bind_inherit;
}

void restore()
{
    rf::ControlConfig* cc = bind_context_controls();
    for (int i = 0; i < bind_context_max_bindings; ++i) {
        if (!g_shadowed[i]) {
            continue;
        }
        if (!cc) {
            // No table to write back into: keep the shadow, and keep the context marked active
            // so the next do_frame tries again rather than losing the Global values.
            return;
        }
        write_item(cc->bindings[i], g_shadow[i]);
        drain_key_presses(cc->bindings[i]);
        g_shadowed[i] = false;
    }
    g_active = BindContextId::Global;
}

void apply(BindContextId ctx)
{
    rf::ControlConfig* cc = bind_context_controls();
    const int index = static_cast<int>(ctx);
    if (!cc || index <= 0 || index >= ctx_count) {
        g_active = BindContextId::Global;
        return;
    }
    const int total = bind_context_binding_count();
    for (int i = 0; i < total; ++i) {
        const std::optional<BindOverride>& value = g_overrides[index][i];
        if (!value) {
            continue;
        }
        if (!g_shadowed[i]) {
            read_item(cc->bindings[i], g_shadow[i]);
            g_shadowed[i] = true;
        }
        compose_item(cc->bindings[i], *value);
        drain_key_presses(cc->bindings[i]);
    }
    g_active = ctx;
}

// Most specific wins; a context with no override for an action simply leaves Global in place.
BindContextId detect()
{
    // Menus, limbo and level download use Global, whatever state the level was left in.
    if (!rf::gameseq_in_gameplay()) {
        return BindContextId::Global;
    }
    // Any seat: driver, gunner, passenger or turret.
    if (vehicle_ridden_hull(rf::local_player_entity)) {
        return BindContextId::Vehicles;
    }
    if (demo_playback_active()) {
        return BindContextId::Demos;
    }
    if (multi_spectate_is_spectating()) {
        return BindContextId::Spectator;
    }
    return BindContextId::Global;
}

bool valid_override_slot(BindContextId ctx, int index)
{
    const int c = static_cast<int>(ctx);
    return c > 0 && c < ctx_count && index >= 0 && index < bind_context_max_bindings;
}

} // namespace

rf::ControlConfig* bind_context_controls()
{
    return rf::local_player ? &rf::local_player->settings.controls : nullptr;
}

int bind_context_binding_count()
{
    const rf::ControlConfig* cc = bind_context_controls();
    return cc ? std::clamp(cc->num_bindings, 0, bind_context_max_bindings) : 0;
}

const char* bind_context_ini_name(BindContextId ctx)
{
    switch (ctx) {
        case BindContextId::Vehicles: return "vehicles";
        case BindContextId::Spectator: return "spectator";
        case BindContextId::Demos: return "demos";
        default: return "global";
    }
}

const char* bind_context_label(BindContextId ctx)
{
    switch (ctx) {
        case BindContextId::Vehicles: return "VEHICLES";
        case BindContextId::Spectator: return "SPECTATOR";
        case BindContextId::Demos: return "DEMOS";
        default: return "GLOBAL";
    }
}

void bind_context_do_frame()
{
    if (g_suspend_depth > 0) {
        return;
    }
    const BindContextId want = detect();
    if (want == g_active) {
        return;
    }
    restore();
    if (want != BindContextId::Global) {
        apply(want);
    }
}

void bind_context_suspend()
{
    if (g_suspend_depth++ == 0) {
        restore();
    }
}

void bind_context_resume()
{
    if (g_suspend_depth > 0 && --g_suspend_depth == 0) {
        bind_context_do_frame();
    }
}

BindOverride bind_context_global_binding(int index)
{
    BindOverride out{{-1, -1}, -1};
    if (index < 0 || index >= bind_context_max_bindings) {
        return out;
    }
    if (g_shadowed[index]) {
        return g_shadow[index];
    }
    const rf::ControlConfig* cc = bind_context_controls();
    if (cc && index < bind_context_binding_count()) {
        read_item(cc->bindings[index], out);
    }
    return out;
}

const BindOverride* bind_context_override(BindContextId ctx, int index)
{
    if (!valid_override_slot(ctx, index)) {
        return nullptr;
    }
    const std::optional<BindOverride>& value = g_overrides[static_cast<int>(ctx)][index];
    return value ? &*value : nullptr;
}

BindOverride bind_context_effective(BindContextId ctx, int index)
{
    BindOverride out = bind_context_global_binding(index);
    if (const BindOverride* value = bind_context_override(ctx, index)) {
        for (int j = 0; j < 2; ++j) {
            if (value->scan_codes[j] != bind_inherit) {
                out.scan_codes[j] = value->scan_codes[j];
            }
        }
        if (value->mouse_btn_id != bind_inherit) {
            out.mouse_btn_id = value->mouse_btn_id;
        }
    }
    return out;
}

void bind_context_set_override(BindContextId ctx, int index, BindOverride value)
{
    if (!valid_override_slot(ctx, index)) {
        return;
    }
    if (all_inherit(value)) {
        bind_context_clear_override(ctx, index);
        return;
    }
    const bool live = g_active == ctx;
    if (live) {
        restore();
    }
    g_overrides[static_cast<int>(ctx)][index] = value;
    if (live) {
        apply(ctx);
    }
}

void bind_context_clear_override(BindContextId ctx, int index)
{
    if (!valid_override_slot(ctx, index)) {
        return;
    }
    const bool live = g_active == ctx;
    if (live) {
        restore();
    }
    g_overrides[static_cast<int>(ctx)][index].reset();
    if (live) {
        apply(ctx);
    }
}

void bind_context_clear_all(BindContextId ctx)
{
    const int c = static_cast<int>(ctx);
    if (c <= 0 || c >= ctx_count) {
        return;
    }
    const bool live = g_active == ctx;
    if (live) {
        restore();
    }
    for (int i = 0; i < bind_context_max_bindings; ++i) {
        g_overrides[c][i].reset();
    }
    if (live) {
        apply(ctx);
    }
}

bool bind_context_dedupe_against_global()
{
    if (!bind_context_controls()) {
        return false;
    }
    const BindContextId live = g_active;
    if (live != BindContextId::Global) {
        restore();
    }
    const int total = bind_context_binding_count();
    bool changed = false;
    for (int c = 1; c < ctx_count; ++c) {
        const auto ctx = static_cast<BindContextId>(c);
        for (int i = 0; i < total; ++i) {
            std::optional<BindOverride>& value = g_overrides[c][i];
            if (!value) {
                continue;
            }
            const auto taken_by_other = [&](int16_t code, bool mouse) {
                for (int k = 0; k < total; ++k) {
                    if (k == i) {
                        continue;
                    }
                    const BindOverride other = bind_context_effective(ctx, k);
                    if (mouse ? other.mouse_btn_id == code
                              : (other.scan_codes[0] == code || other.scan_codes[1] == code)) {
                        return true;
                    }
                }
                return false;
            };
            for (int j = 0; j < 2; ++j) {
                const int16_t code = value->scan_codes[j];
                if (code == bind_inherit || code < 0 || !taken_by_other(code, false)) {
                    continue;
                }
                value->scan_codes[j] = -1;
                changed = true;
            }
            if (value->mouse_btn_id != bind_inherit && value->mouse_btn_id >= 0
                && taken_by_other(value->mouse_btn_id, true)) {
                value->mouse_btn_id = -1;
                changed = true;
            }
        }
    }
    if (live != BindContextId::Global) {
        apply(live);
    }
    return changed;
}

void bind_context_reset_all()
{
    restore();
    for (int c = 0; c < ctx_count; ++c) {
        for (int i = 0; i < bind_context_max_bindings; ++i) {
            g_overrides[c][i].reset();
        }
    }
}
