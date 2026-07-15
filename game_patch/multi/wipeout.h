#pragma once

namespace rf
{
    struct Player;
    struct Entity;
}

void wipeout_level_init();
void wipeout_level_init_post();
void wipeout_do_frame();
bool wipeout_can_player_spawn(rf::Player* player);
bool wipeout_is_subsequent_spawn(rf::Player* player);
int wipeout_get_red_team_score();
int wipeout_get_blue_team_score();
void wipeout_set_red_team_score(int v);
void wipeout_set_blue_team_score(int v);
int wipeout_count_team_alive(int team);
int wipeout_count_team_waiting(int team);
