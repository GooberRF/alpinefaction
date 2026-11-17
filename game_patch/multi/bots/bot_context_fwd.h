#pragma once

struct BotContext;

BotContext& client_bot_context();
void bot_context_activate(BotContext& ctx);
void bot_context_deactivate(BotContext& ctx);
