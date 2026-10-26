#pragma once

#include "bot_chat_manager.h"
#include "bot_context_fwd.h"
#include "bot_internal.h"
#include "bot_personality.h"
#include "../../rf/entity.h"
#include "../../rf/player/player.h"
#include <cassert>
#include <string>
#include <string_view>
#include <vector>

struct BotInfo
{
    bool spawn_allowed_by_server = true;
    bool can_spawn = false;
};

struct BotContext
{
    rf::Player* player = nullptr;
    bool is_server_bot = false;
    BotState state{};
    BotInfo info{};
    BotChatRuntimeState chat{};
    BotPersonality personality{};
    BotSkillProfile skill{};
    bool profiles_initialized = false;
    std::string personality_preset = "balanced";
    std::string skill_preset = "average";
    // Applied on top of the presets each time the profile is initialised.
    std::vector<BotConfigOverride> personality_overrides;
    std::vector<BotConfigOverride> skill_overrides;
};

// Set only within a BotActiveScope.
extern BotContext* g_active_bot;

class BotActiveScope
{
public:
    explicit BotActiveScope(BotContext& ctx) : prev_{g_active_bot}
    {
        g_active_bot = &ctx;
    }

    ~BotActiveScope()
    {
        g_active_bot = prev_;
    }

    BotActiveScope(const BotActiveScope&) = delete;
    BotActiveScope& operator=(const BotActiveScope&) = delete;

private:
    BotContext* prev_;
};

inline BotContext& bot_ctx()
{
    assert(g_active_bot);
    return *g_active_bot;
}

inline BotState& bot_state()
{
    return bot_ctx().state;
}

inline rf::Player* bot_player()
{
    return bot_ctx().player;
}

inline rf::Entity* bot_entity()
{
    rf::Player* const player = bot_player();
    return player ? rf::entity_from_handle(player->entity_handle) : nullptr;
}

void bot_run_frame(BotContext& ctx);
void bot_context_reset_idle(BotContext& ctx);

bool bot_backend_weapon_busy(rf::Player& pp, rf::Entity& ep);
void bot_backend_jump(rf::Player& pp);
void bot_backend_crouch(rf::Player& pp);
void bot_backend_use(rf::Player& pp);
void bot_backend_reload(rf::Player& pp);
void bot_backend_switch_weapon(rf::Player& pp, rf::Entity& ep, int weapon_type);
void bot_backend_request_respawn(rf::Player& pp);
bool bot_backend_say(const BotContext& ctx, std::string_view message, bool is_team_msg);
bool bot_backend_taunt(const BotContext& ctx, std::string_view taunt_message);
