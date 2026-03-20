#pragma once

#include "bot_internal.h"
#include "../../rf/player/player.h"

void bot_alerts_reset();
void bot_alerts_update_context(const rf::Player& local_player, const rf::Entity& local_entity);
bool bot_alerts_get_contact_awareness(
    int entity_handle,
    float& out_awareness_weight,
    BotAlertType* out_alert_type = nullptr);
