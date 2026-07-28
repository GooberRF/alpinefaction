#pragma once

namespace rf
{
    struct Player;
}

void gungame_level_init();
void gungame_level_init_post();
void gungame_do_frame();
void gungame_on_player_kill(rf::Player* killer, rf::Player* killed);
void gungame_on_player_spawn(rf::Player* player);
void gungame_on_player_disconnect(rf::Player* player);
int gungame_spawn_weapon_for(rf::Player* player);
