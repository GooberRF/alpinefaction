#pragma once

#include <cstdint>

namespace rf
{
    struct ControlConfig;
}

// Context specific bindings. A context owns a sparse set of overrides keyed by ControlConfig
// binding index; while that context is live its overrides are written into the engine's own
// ControlConfigItem fields (which every poll reads afresh) and the displaced Global values are
// held in a shadow until the context ends.

enum class BindContextId : uint8_t
{
    Global,
    Vehicles,
    Spectator,
    Demos,
    _count,
};

inline constexpr int bind_context_max_bindings = 128;

// Per field sentinel: this context says nothing about the field, so Global shows through. -1 is
// a value of its own there, "explicitly unbound in this context".
inline constexpr int16_t bind_inherit = INT16_MIN;

struct BindOverride
{
    int16_t scan_codes[2];
    int16_t mouse_btn_id;
};

// The local player's live binding table, or null before the player exists.
rf::ControlConfig* bind_context_controls();
// Its binding count, capped to bind_context_max_bindings.
int bind_context_binding_count();

const char* bind_context_ini_name(BindContextId ctx);
const char* bind_context_label(BindContextId ctx);

// Evaluated once per frame from rf_do_frame_hook, in every game state.
void bind_context_do_frame();
// Forces Global and blocks do_frame until the matching resume. Nests.
void bind_context_suspend();
void bind_context_resume();

// The true Global fields for a binding: the shadow while a context is applied over it, else the
// live item. The ini saver must use this, never the raw live item.
BindOverride bind_context_global_binding(int index);

const BindOverride* bind_context_override(BindContextId ctx, int index);
// The triple a context resolves an action to: its override fields, Global for the inherited ones.
BindOverride bind_context_effective(BindContextId ctx, int index);
// An all-inherit value drops the override instead of storing it.
void bind_context_set_override(BindContextId ctx, int index, BindOverride value);
void bind_context_clear_override(BindContextId ctx, int index);
void bind_context_clear_all(BindContextId ctx);
// After a Global edit: unbinds any override field that now duplicates another action's effective
// binding, so the newest Global assignment stays exclusive inside every context. True if it
// changed anything.
bool bind_context_dedupe_against_global();
// Drops every override in every context; the settings loader calls this before parsing.
void bind_context_reset_all();
