# Console Commands

Open the console with `~`. Press Tab to complete a command name; with a name and a space typed, Tab completes level names (`level`, `map`, `levelm`, `mapm`, `sv_gametype`, `gt`), player names (`kick`, `ban`), demo files (`demo_play`, `demo_info`) and command names (`rcon`, `help`), or prints the command's usage. `help <command>` prints usage, and `. <text>` lists every command whose name or description contains the text. Most settings changed here are saved to `alpine_settings.ini` (`alpine_settings_<mod>.ini` when a TC mod is loaded, or the file given with `-afs`); commands marked "not saved" only last for the session.

This page lists the commands Alpine Faction adds, including those inherited from Dash Faction and stock Red Faction commands that the retail PC build leaves unregistered (marked "Stock RF command"; their arguments are listed where known). Other stock commands are not listed.

Argument notation: `<x>` is required, `[x]` is optional. A `[bool]` argument accepts `true`, `on` or a non-zero number to enable and `false`, `off` or `0` to disable; leaving it out toggles. Value commands print the current value when run without an argument. Defaults are the values used when a setting is not in the settings file.

## General

| Command | Arguments | Description |
| --- | --- | --- |
| `.` | `<query>` | Lists commands whose name or description contains the query. |
| `af_get` | `<key>` | Prints a setting's current value and whether it differs from the default. `key` is the setting's ID or its `alpine_settings.ini` key (case-insensitive); an unknown key lists up to five settings whose name contains it. |
| `af_set` | `<key> <value>` | Sets any setting the settings menu can change, with `key` as for `af_get`. Out-of-range numbers are clamped, choice values snap to the nearest option, and text values keep their case. Keybinds, list orderings and launcher settings cannot be set here. Without a value, prints the accepted values and the current one. |
| `afstats_status` | | Shows the status of this session's FactionFiles player stats link (stats-enabled server, session state, signed-in account). |
| `autodl_blur_background` | | Toggles the blurred background on the level download screen. Default: on. |
| `autodl_download_awps` | | Toggles downloading AWP waypoint files along with levels from the autodownloader. Default: off. |
| `cl_discord_join_button` | `[bool]` | Offers a join link to public, password-free servers in your Discord activity. Default: on. |
| `cl_discord_presence` | `[bool]` | Shows what you are playing (server, map and player count in multiplayer) as your Discord activity. Default: on. |
| `download_awp_force` | `<rfl_name>` | Downloads the AWP waypoint file for an installed level from the autodownloader, ignoring revision and settings. `.rfl` is added if there is no extension. |
| `download_level` | `<rfl_name>` | Downloads a level from FactionFiles.com if it is not installed. Allowed via rcon. |
| `download_level_force` | `<rfl_name>` | Downloads a level from FactionFiles.com, overwriting your local copy if one exists. |
| `fflink_status` | | Shows whether this client is linked to a FactionFiles account. |
| `findlevel` (`findmap`) | `<query>` | Lists installed `.rfl` files whose name contains the query. |
| `key_quick_exit` | | Toggles Shift+Esc quitting the game. Default: off. |
| `level_info` (`map_info`) | | Shows information about the current level: file, name, author, RFL version, Alpine and Dash level properties, `_info.tbl` overrides and mesh replacements. |
| `levelm` (`mapm`) | `<filename>` | Starts a new local multiplayer game on the level. Not available on dedicated servers. |
| `map` | `<filename>` | Alias of the stock `level` command, which Alpine also allows outside multiplayer. Allowed via rcon. |
| `maxfps` | `[limit]` | Sets the frame rate limit (10-100000, default 240). In multiplayer it is capped at 240 unless you are hosting or the server allows unlimited FPS. On a dedicated server it sets the server limit (default 80). Allowed via rcon. |
| `save_commands` | | Stock RF command. Prints all console commands to `console_commands.txt`. |
| `screenshot` | | Saves a screenshot and prints its file name. |
| `script` | `<scriptfilename>` | Stock RF command. Runs a console script file from the game directory. |
| `ver` | | Shows the Alpine Faction version and build date. |
| `which_packfile` (`where`) | `<filename>` | Prints the path of the packfile that contains the file. |

## Graphics and rendering

Commands marked "D3D11 only" only have an effect with the Direct3D 11 renderer.

| Command | Arguments | Description |
| --- | --- | --- |
| `cl_ignore_tbl_lightmap_clamping` | | Toggles ignoring per-level lightmap clamping overrides from `<level>_info.tbl`. Applies on the next level load. Default: off. |
| `cl_ignore_tbl_pixel_light_overbright` | | Toggles ignoring per-level pixel light overbright overrides from `<level>_info.tbl`. Default: off. |
| `cl_ignore_tbl_vertex_lighting` | | Toggles ignoring per-level vertex lighting overrides from `<level>_info.tbl`. Default: off. |
| `d_fullscreen` | | Switches to fullscreen mode. |
| `d_windowed` | | Switches to windowed mode. |
| `max_decals` | `[count]` | Sets the decal limit (128-512, default 512). Not saved. |
| `r_antialiasing` | | Toggles anti-aliasing off and back to the last MSAA level. Set a level with `r_antialiasing_mode` first. |
| `r_antialiasing_mode` | `[none\|2\|4\|8\|msaax{2,4,8}]` | Sets the MSAA level, or `none`. Default: none. |
| `r_clampofficiallightmaps` | | Toggles forced lightmap clamping in official levels when full range lighting is on. Applies on the next level load. Default: off. |
| `r_colorblind` | `[off\|protanopia\|deuteranopia\|tritanopia]` | Sets the colorblind filter; `0`-`3` also work. D3D11 only. Default: off. |
| `r_detailtextures` | `[0\|1\|2]` | Detail textures: `0` off, `1` only those an ATX file names, `2` also automatic ones by surface material. D3D11 only. Default: 1. |
| `r_dynamiclightndotl` | `[0.0-1.0]` | Sets the N-L blend for dynamic lights on level geometry (0 = stock, 1 = full). D3D11 only. Default: 1.0. |
| `r_fastanim` | | Toggles fast character animations (less smooth at far distances). |
| `r_fov` | `[degrees]` | Sets horizontal FOV (75-160), or `0` for automatic scaling. Also prints the server's FOV limit, if any. Default: 0. |
| `r_fpgunfov` | `[scale]` | Sets the FOV scale for first person weapon models (0.1-1.5). Default: 1.0. |
| `r_fullbright` | | Toggles fullbright character meshes. In multiplayer, only applies if the server allows it. Default: off. |
| `r_fullrangelighting` | | Toggles full range lighting. Applies on the next level load. Default: on. |
| `r_gamma` | `[value]` | Sets gamma (0.7-2.2, the stock slider range). Default: 1.0. |
| `r_lightmaps` | | Toggles rendering only lightmaps (no textures) on level geometry. In multiplayer, only applies if the server allows it. Default: off. |
| `r_lodscale` | `[scale]` | Sets the LOD distance scale (0.1-1000). Default: 10. |
| `r_meshlighting` | `[0-2]` | Mesh lighting: `0` ambient only, `1` vertex (legacy), `2` pixel (D3D11). A level's `_info.tbl` can force vertex lighting. Default: 2. |
| `r_monitorscale` | `[scale]` | Sets the resolution scale of security monitors and mirrors (1-8). Reload the level to apply. Default: 2. |
| `r_muzzleflash` | | Toggles muzzle flash lights. In multiplayer, turning them off only applies if the server allows it. Default: on. |
| `r_nearest` | | Toggles nearest (unfiltered) texture filtering. Default: off. |
| `r_outlines` | | Toggles player outlines during gameplay. D3D11 only. Default: off. |
| `r_outlines_color` | `[RRGGBB\|RRGGBBAA\|clear]` | Sets the outline color for non-team game modes. D3D11 only. Default: `FF3232FF`. |
| `r_outlines_color_enemy` | `[RRGGBB\|RRGGBBAA\|clear]` | Overrides the outline color for enemies; `clear` removes the override. D3D11 only. |
| `r_outlines_color_team` | `[RRGGBB\|RRGGBBAA\|clear]` | Overrides the outline color for teammates; `clear` removes the override. D3D11 only. |
| `r_outlines_color_team_b` | `[RRGGBB\|RRGGBBAA\|clear]` | Sets the outline color for blue team players. D3D11 only. Default: `0096FFFF`. |
| `r_outlines_color_team_r` | `[RRGGBB\|RRGGBBAA\|clear]` | Sets the outline color for red team players. D3D11 only. Default: `FF3232FF`. |
| `r_outlines_spectator` | | Toggles player outlines while spectating. D3D11 only. Default: off. |
| `r_outlines_team_xray` | | Toggles showing teammate outlines through walls. D3D11 only. Default: on. |
| `r_picmip` | `[divisor]` | Sets the global texture resolution divisor (1-256, 1 = full resolution). D3D11 only. Default: 1. |
| `r_pixellightoverbright` | `[0.0-3.0]` | Sets the overbright range for pixel lighting. A level's `_info.tbl` can override it. D3D11 only. Default: 0.5. |
| `r_precache_rooms` | | Toggles room precaching (longer level loads, fewer hitches during play). D3D11 only. Default: on. |
| `r_scopeglass` | `[0-2]` | Scope lens distortion, chromatic aberration and vignetting: `0` off, `1` light, `2` heavy. D3D11 only. Default: 2. |
| `r_shadowcorpses` | | Toggles corpse shadows. Default: on. |
| `r_shadowdistance` | `[0-5]` | Sets the shadow distance preset: Lowest, Low, Medium, High, Very High, Maximum. Default: 3 (High). |
| `r_shadowitems` | | Toggles item shadows. Default: on. |
| `r_shadowquality` | `[0-5]` | Sets the shadow quality preset: `0` blob shadows, `1`-`5` shadow maps (Low to Maximum). Default: 3 (High). |
| `r_shadowupdateinterval` | `[1-30]` | Sets how often shadow maps refresh, in frames (1 = every frame). Default: 1. |
| `r_trilinearfiltering` | | Stock RF command. Toggles trilinear filtering. |
| `r_underwater` | `[0-3]` | Underwater effects: `0` stock, `1` caustics, `2` adds fog, tint and vignette, `3` adds distortion. D3D11 only. Default: 3. |
| `r_weather` | | Toggles rendering of level weather regions. Default: on. |
| `vli` | | Toggles volumetric lighting (glares). Default: on. |

## Effects

| Command | Arguments | Description |
| --- | --- | --- |
| `cl_burningentityglow` | | Toggles dynamic light glow from burning entities. D3D11 only. Default: on. |
| `cl_damageflash` | `[0-2]` | Damage feedback: `0` off, `1` screen flash, `2` edge vignette (D3D11 only). Default: 1. |
| `cl_explosionflashenv` | | Toggles explosion flash lights from environmental sources. D3D11 only. Default: on. |
| `cl_explosionflashweapons` | | Toggles explosion flash lights from weapons. D3D11 only. Default: on. |
| `cl_gibchunks` | `[count]` | Sets the number of gib chunks (7-100). Default: 14. |
| `cl_gibflames` | | Toggles flames on gib chunks. Default: on. |
| `cl_giblifetimems` | `[milliseconds]` | Sets gib lifetime (1000-15000 ms). Default: 7000. |
| `cl_gibvelocityscale` | `[scale]` | Sets gib velocity scale (3-100). Default: 15. |
| `cl_gorelevel` | `[level]` | Sets gore level, `0` (minimal) to `2` (maximum). |
| `cl_legacy_bob` | | Toggles the original weapon bob, which stutters while running. Default: off. |
| `cl_magazines` | `[bool]` | Toggles magazines ejected when you reload. Default: on. |
| `cl_particlecollidelod` | | Toggles testing cosmetic particle collision less often with camera distance. Default: on. |
| `cl_particlecull` | | Toggles skipping simulation of level particle emitters that are not visible. Default: on. |
| `cl_particlefixedstep` | | Toggles stepping cosmetic particles at a fixed rate with render interpolation. Default: on. |
| `cl_particlepoolsize` | `[n]` | Sets the capacity of each particle pool (1100-16384, or `0` for the stock 500 and 1100). Applies on the next level load. Default: 8192. |
| `cl_particlerate` | `[hz]` | Sets the fixed particle simulation rate (30-240 Hz). Default: 60. |
| `cl_ragdolls` | `[bool]` | Toggles client-side ragdoll deaths. Default: off. |
| `cl_ragdolls_max` | `[4-128]` | Sets how many ragdolls stay in the world before the oldest is removed; capped to 32 in single player. Default: 16. |
| `cl_railbeamfix` | | Toggles the Rail Driver beam following the shot's path through pierced surfaces. Default: on. |
| `cl_shellcasings` | `[bool]` | Toggles shell casings ejected by firing weapons. Default: on. |
| `cl_shellcollide` | `[bool]` | Toggles shell casings bouncing off the level instead of falling through it. Default: on. |
| `cl_shelllifetime` | `[seconds]` | Sets shell casing lifetime (1-10 s). Default: 2. |
| `cl_showenemybullets` | | Toggles enemy bullet impact effects. Default: on. |
| `cl_weaponsway` | | Toggles first person weapon aim sway. Default: off. |
| `cl_weapshake` | | Toggles disabling camera shake from weapon fire. In multiplayer, only applies if the server allows it. Default: off (shake enabled). |
| `mp_ricochet` | | Toggles bullet ricochets in multiplayer (visual only, no damage). Default: off. |

## Audio

| Command | Arguments | Description |
| --- | --- | --- |
| `cl_footsteps` | | Toggles third-person footstep sounds for non-pistol weapons. Default: on. |
| `cl_hitsounds` | | Toggles a sound when you hit players in multiplayer, if the Alpine Faction server enables it. Default: on. |
| `cl_hitsounds_min_interval` | `[ms]` | Sets the minimum interval between hit sounds (0-1000 ms). Default: 20. |
| `cl_hitsounds_streakpitch` | | Toggles hit sounds rising in pitch with consecutive hits. Default: on. |
| `cl_painsounds` | | Toggles pain sounds. Default: on. |
| `levelsounds` | `[volume]` | Sets the volume scale of level sounds (0.0-1.0). Default: 1.0. |
| `mp_join_beep` | | Toggles a beep when a player joins while the game window is out of focus. Default: off. |
| `mp_taunts` | | Toggles voice lines for taunts used by players in multiplayer. Default: on. |
| `sound` | `[bool]` | Stock RF command. Toggles sound. |

## Input and mouse

| Command | Arguments | Description |
| --- | --- | --- |
| `cl_autoswitchempty` | | Toggles switching away from a weapon with no ammo even when autoswitch is off. Default: on. |
| `cl_autoswitchfirewait` | `[ms]` | Sets the time after firing during which autoswitch is suppressed (0-10000 ms). Default: 0. |
| `cl_linearpitch` | | Toggles linear mouse pitch. Default: on. |
| `cl_scannersens` | `[value]` | Sets the mouse sensitivity modifier in a scanner (0.01-10). Default: 0.25. |
| `cl_scopesens` | `[value]` | Sets the mouse sensitivity modifier in a scope (0.01-10). Default: 0.25. |
| `cl_staticscopesens` | | Toggles static scope sensitivity instead of scaling with zoom. Default: off (dynamic). |
| `cl_swaparcontrols` | | Toggles swapping the Assault Rifle fire controls. Default: off. |
| `cl_swapgrenadecontrols` | | Toggles swapping the grenade fire controls. Default: off. |
| `cl_swapsgcontrols` | | Toggles swapping the shotgun fire controls. Default: off. |
| `cl_togglecrouch` | | Stock RF command. Toggles crouch mode. |
| `inputmode` | | Toggles DirectInput mouse mode. Not available in headless bot mode. Default: on. |
| `ms` | `[value]` | Sets mouse sensitivity. |
| `ms_scale` | `[0\|1\|2]` | Mouse scale mode: `0` Classic (RF native), `1` Raw (pure degrees), `2` Modern (id Tech/Source style). Default: 0. |

## HUD

| Command | Arguments | Description |
| --- | --- | --- |
| `bighud` | | Toggles the big HUD. Refused if the screen resolution is too low. Default: off. |
| `bighud_ammo` | `[bool]` | Toggles the big HUD for ammo (only when the big HUD is on). Default: on. |
| `bighud_health` | `[bool]` | Toggles the big HUD for health (only when the big HUD is on). Default: on. |
| `cl_awards` | | Toggles showing awards you earn in multiplayer. Default: on. |
| `cl_chat` | | Toggles hiding the multiplayer chat. Default: chat shown. |
| `cl_columnheightscale` | `[scale]` | Sets the control point light column height scale (0-1000). Default: 1.0. |
| `cl_columnsegments` | `[segments]` | Sets the number of segments of control point light columns (3-256). Default: 8. |
| `cl_dominationmsgs` | | Toggles dominating and revenge server messages in chat. Default: on. |
| `cl_hud` | `[bool]` | Shows or hides the HUD. Not saved. |
| `cl_locationpings` | | Toggles location pinging (team game types and RUN only). Default: on. |
| `cl_minimap` | `[bool]` | Shows the multiplayer minimap on levels that provide one. Default: on. |
| `cl_minimap_labels` | `[bool]` | Shows control point names on the minimap. Default: on. |
| `cl_minimap_rotate` | `[bool]` | Rotates the minimap with your heading instead of keeping north up. Default: off. |
| `cl_minimap_size` | `[80-240]` | Sets the minimap size in small-HUD pixels (scaled with the big HUD). Default: 140. |
| `cl_minimap_zoom` | `[0.1-1.0]` | Sets how much of the level the minimap shows, as a share of its larger extent. Default: 0.35. |
| `cl_outlineheightscale` | `[scale]` | Sets the control point outline height scale (0-1000). Default: 5.0. |
| `cl_outlinesegments` | `[segments]` | Sets the number of segments of control point outline rings (3-256). Default: 32. |
| `cl_wh_altdmgindicators` | | Toggles individual instead of cumulative world HUD damage numbers. Default: off. |
| `cl_wh_cpoverdraw` | | Toggles drawing control point world HUD sprites on top of everything. Default: on. |
| `cl_wh_ctf` | | Toggles world HUD indicators for CTF flags. Default: on. |
| `cl_wh_flagoverdraw` | | Toggles drawing CTF flag world HUD sprites on top of everything. Default: on. |
| `cl_wh_hitnumbers` | | Toggles damage numbers when you hit players in multiplayer, if the Alpine Faction server enables them. Default: on. |
| `cl_wh_teamplayerlabels` | | Toggles name labels over teammates. Default: off. |
| `kill_messages` | | Toggles kill messages in the chat box and console. Default: on. Not saved. |
| `mp_crit_flash` | | Toggles the reticle flash when you land a critical hit. Default: on. |
| `mp_deathbars` | | Toggles red bars at the top and bottom of the screen while dead. Default: on. |
| `mp_join_flash` | | Toggles flashing the game window when a player joins while it is out of focus. Default: on. |
| `ui_always_show_specators` | | Toggles always showing spectators on your HUD. Default: off. |
| `ui_assist_highlight` | | Toggles highlighting kill messages for kills you assisted. Default: on. |
| `ui_assist_names` | | Toggles including assists in kill messages. Default: on. |
| `ui_fpsavg` | `[milliseconds]` | Sets the FPS counter averaging window (0-60000 ms, 0 = update every frame). Default: 100. |
| `ui_gamefeed` | | Toggles showing game event messages in a separate feed instead of chat. Default: off. |
| `ui_gametype_help` | | Toggles the game type help notification shown when spawning on a server. Default: on. |
| `ui_minisb_dm` | | Toggles the mini scoreboard in Deathmatch. Default: on. |
| `ui_playernames` | | Toggles names of targeted players. Default: on. |
| `ui_realarmor` | | Toggles showing armor as real values (1:1 with effective health) instead of classic (2:1). Default: off. |
| `ui_runtimer` | | Toggles the RUN game type timer. Default: on. |
| `ui_scoreboard_simplesplit` | | Toggles whether each scoreboard group is shown individually. Default: on. |
| `ui_scoreboard_split_bots` | | Toggles grouping bots separately on the scoreboard. Default: off. |
| `ui_scoreboard_split_browsers` | | Toggles grouping browsers separately on the scoreboard. Default: on. |
| `ui_scoreboard_split_idle` | | Toggles grouping idle players separately on the scoreboard. Default: off. |
| `ui_scoreboard_split_spectators` | | Toggles grouping spectators separately on the scoreboard. Default: on. |
| `ui_show_fps` | | Toggles the FPS counter. Default: on. |
| `ui_show_ping` | | Toggles the ping counter. Default: on. |
| `ui_show_speed` | | Toggles the speed meter. Default: off. |
| `ui_simple_server_chat_messages` | | Toggles simple server chat messages. Default: on. |
| `ui_verbosetimer` | | Toggles the "Time Left:" text on the in-game timer. Default: on. |

## HUD colors and scale

Colors are hex `RRGGBB` or `RRGGBBAA`, or comma-separated `R,G,B[,A]` values in quotes. `clear` (also `default` or `-1`) removes an override, or resets a color that always has a value to its default. Scales accept `clear` the same way.

| Command | Arguments | Description |
| --- | --- | --- |
| `ui_color_ar_ammo` | `[RRGGBB\|RRGGBBAA\|clear]` | Sets the assault rifle ammo digit color override. Ignored if a client-side mod sets it. |
| `ui_color_console` | `[RRGGBB\|RRGGBBAA\|clear]` | Sets the console background color. Default: `274E69C0`. |
| `ui_color_damage_notify` | `[RRGGBB\|RRGGBBAA\|clear]` | Sets the damage notification color override. |
| `ui_color_location_ping` | `[RRGGBB\|RRGGBBAA\|clear]` | Sets the location ping color override. |
| `ui_color_multi_timer` | `[RRGGBB\|RRGGBBAA\|clear]` | Sets the multiplayer timer color override. Ignored if a client-side mod sets it. |
| `ui_color_precision_scope` | `[RRGGBB\|RRGGBBAA\|clear]` | Sets the precision rifle scope color override. Ignored if a client-side mod sets it. |
| `ui_color_rail_scope` | `[RRGGBB\|RRGGBBAA\|clear]` | Sets the rail driver scope color override. Ignored if a client-side mod sets it. |
| `ui_color_reticle` | `[RRGGBB\|RRGGBBAA\|clear]` | Sets the reticle color override. |
| `ui_color_reticle_locked` | `[RRGGBB\|RRGGBBAA\|clear]` | Sets the locked reticle color override (single player). |
| `ui_color_sniper_scope` | `[RRGGBB\|RRGGBBAA\|clear]` | Sets the sniper scope color override. Ignored if a client-side mod sets it. |
| `ui_color_team_label` | `[RRGGBB\|RRGGBBAA\|clear]` | Sets the teammate world label color override. |
| `ui_color_thermal` | `[RRGGBB\|RRGGBBAA\|clear]` | Sets the thermal (IR) entity color override for the Rail Driver scanner. D3D11 only. |
| `ui_colorize_custom_reticles` | | Toggles applying reticle color overrides to custom reticles (looks wrong on non-greyscale reticle textures). Default: off. |
| `ui_scale_damage_notify` | `[scale\|clear]` | Scales damage notification text (0.5-3). Default: 1.0. |
| `ui_scale_ping_label` | `[scale\|clear]` | Scales location ping label text (0.5-3). Default: 1.0. |
| `ui_scale_player_label` | `[scale\|clear]` | Scales player label text (0.5-3). Default: 1.0. |
| `ui_scale_reticle` (`ui_reticlescale`) | `[scale\|clear]` | Scales the reticle (0-100). Default: 1.0. |

## Multiplayer

| Command | Arguments | Description |
| --- | --- | --- |
| `cl_sprays` | | Toggles showing players' sprays. Default: on. |
| `disconnect` | | Disconnects from the current server. |
| `mp_character` | `[index]` | Prints or sets your multiplayer character by index. |
| `mp_handicap` | `[percent]` | Sets your damage reduction handicap (0-99) and sends it to the server. Only applied on servers that support it. Default: 0. |
| `mp_playercount` | | Prints the number of players. Multiplayer only. |
| `mute_all_players` | | Toggles muting all players in chat. Not saved. |
| `mute_player` | `<player_name>` | Toggles muting one player in chat. Not saved. |
| `spray` | `[id]` | Selects the spray to use; without an id, also lists the available sprays. Default: 0. |

## Vehicles

| Command | Arguments | Description |
| --- | --- | --- |
| `cl_vehiclecam_bounce` | `[0.0-1.0]` | Sets how much the vehicle camera smooths out vertical bouncing, in seconds. Default: 0.15. |
| `cl_vehiclecam_follow` | `[0.0-2.0]` | Sets how long the third-person vehicle camera takes to swing round behind the vehicle, in seconds (0 = rigid). Default: 0.3. |
| `cl_vehiclecam_lag` | `[0.0-0.5]` | Sets how much the vehicle camera smooths out sideways and fore-aft jolts, in seconds. Default: 0.04. |
| `cl_vehiclecam_lookahead` | `[0.0-1.0]` | Sets how far the vehicle camera looks into a turn, in seconds of turn rate (0 = off). Default: 0.2. |
| `cl_vehiclecam_lookahead_max` | `[0-30]` | Sets the most the vehicle camera looks into a turn, in degrees. Default: 12. |
| `cl_vehiclecam_min_speed` | `[0.0-10.0]` | Sets the speed (m/s) below which, or while reversing, the vehicle camera holds its heading. Default: 1.5. |
| `cl_vehiclecam_pitch_follow` | `[0.0-1.0]` | Sets how much of the vehicle's pitch the camera tilts with. Default: 0.35. |
| `cl_vehiclecam_pitch_smooth` | `[0.0-3.0]` | Sets how slowly the vehicle camera follows the vehicle's pitch, in seconds. Default: 0.9. |
| `cl_vehiclecam_recenter` | `[bool]` | Eases an idle vehicle camera back behind the vehicle while it drives forward. Default: on. |
| `cl_vehiclecam_velocity` | `[0.0-1.0]` | Sets how far the vehicle camera turns toward a land vehicle's direction of travel at speed. Default: 1.0. |
| `cl_vehiclefx` | `[bool]` | Shows dust, sparks, spray, smoke and fire effects, with their sounds and lights, for vehicles in multiplayer. Default: on. |
| `cl_vehiclehealthbars` | `[bool]` | Shows health bars over damaged vehicles and turrets. Default: on. |
| `cl_vehiclehints` | `[bool]` | Shows the list of vehicle controls while riding a vehicle or turret. Default: on. |
| `cl_vehiclehorns` | `[bool]` | Plays other players' jeep horns in multiplayer; your own always plays. Default: on. |
| `cl_vehiclemarkers` | `[bool]` | Shows vehicle factory respawn markers and their spawn cues. Default: on. |
| `cl_vehicletracers` | `[bool]` | Shows tracers for vehicle guns in multiplayer. Default: on. |
| `cl_vehicletracers_color` | `[RRGGBB\|RRGGBBAA\|default]` | Sets the vehicle tracer color; team games tint it toward the firing team's color. Default: `FFB43CFF`. |
| `cl_vehicletracers_frequency` | `[1-10]` | Shows a tracer on every Nth round of each vehicle gun. Default: 1. |
| `cl_vehicletracers_length` | `[0.5-10.0]` | Sets the vehicle tracer length in meters. Default: 3.0. |

## Spectate

| Command | Arguments | Description |
| --- | --- | --- |
| `cl_damageflash_spectator` | | Toggles the damage screen flash while spectating. Default: on. |
| `cl_freelookslide` | `[0.0-1.0]` | Scales the acceleration and deceleration slide of the free look camera (0 = none). Default: 1.0. |
| `spectate` | `[player_name]` | In multiplayer, spectates the player in first person, or without a name toggles free look spectate. Disabled by the Force Respawn server option. In single player, enters free look. |
| `spectate_cameras` | | Toggles camera meshes at static camera locations in free look spectate. Default: on. |
| `spectate_cinematic_mode` | | Toggles hiding the HUD in free look spectate. Default: off. |
| `spectate_followkiller` | | Toggles automatically spectating the killer when the spectated player dies. Default: off. Not saved. |
| `spectate_minui` | | Toggles the minimal spectate UI. Default: off. |
| `spectate_playerlabels` | | Toggles player name labels in spectate mode. Default: off. |
| `spectate_povcomp` | `[on\|off\|<delay ms>]` | Ping compensation while spectating a player. `on` uses an automatic delay; a number (0-450) sets it. Default: on. |

## Demos

| Command | Arguments | Description |
| --- | --- | --- |
| `demo_info` | `<filename>` | Prints information about a demo file: format, recording version, server, level, mod, duration. |
| `demo_pause` | | Pauses or resumes demo playback. |
| `demo_play` | `<filename>` | Plays a demo file from the `demos` folder. Not available while in a multiplayer game. |
| `demo_povcomp` | `[on\|off\|<delay ms>]` | Ping compensation while following a player in demo playback: delays other players to match what the followed player saw. A number (0-450) sets the delay. Default: on. Not saved. |
| `demo_seek` | `<mm:ss \| seconds \| +/-seconds>` | Seeks within the playing demo. Seeking backward reloads the level. |
| `demo_stop` | | Stops demo playback. |
| `demo_timescale` | `[0.05-10]` | Sets the demo playback speed multiplier. Default: 1.0. |
| `spectate_playerinfo` | | Toggles player health and armor bars during demo playback. Default: off. |
| `spectate_powerups` | | Toggles powerup respawn timers during demo playback. Default: on. |
| `spectate_spawns` | | Toggles respawn point indicators during demo playback. Default: off. |

## Server and administration

"Server only" commands refuse to run unless you are hosting; "dedicated only" ones need a dedicated server; "ADS only" ones need a dedicated server started with `-ads`.

| Command | Arguments | Description |
| --- | --- | --- |
| `fflink_demo_status` | | Shows the FactionFiles demo upload queue status. |
| `map_ext` | `[minutes]` | Extends the round time (default 5 minutes). Server only, not between rounds. Allowed via rcon. |
| `map_next` | | Loads the next level. Server only, not between rounds. Allowed via rcon. |
| `map_prev` | | Loads the previous level. Server only, not between rounds. Allowed via rcon. |
| `map_rand` | | Loads a random level from the rotation. Server only, not between rounds. Allowed via rcon. |
| `map_rest` | | Restarts the current level. Server only, not between rounds. Allowed via rcon. |
| `sv_afstats_events_reset` | | Clears a stuck send and resumes the FactionFiles stats event stream without discarding queued events. Allowed via rcon. |
| `sv_afstats_events_status` | | Shows the status of the FactionFiles stats event stream. Allowed via rcon. |
| `sv_afstats_trace` | `[0\|1]` | Logs the full JSON of every built stats batch. Allowed via rcon. |
| `sv_caplimit` | `<captures>` | Stock RF command. Sets the capture limit. Server only. Allowed via rcon. |
| `sv_checkmaps` | | Checks whether any level in the rotation or vote-allowed list is unavailable for autodownload from FactionFiles. Dedicated only. |
| `sv_countclients` | | Stock RF command. Shows the number of connected clients. |
| `sv_fflink_resync` | | Forces a new FactionFiles session key exchange. Allowed via rcon. |
| `sv_fflink_status` | | Shows the FactionFiles server session link status. Allowed via rcon. |
| `sv_fraglimit` | `<kills>` | Stock RF command. Sets the kill limit. Server only. Allowed via rcon. |
| `sv_gametype` (`gt`) | `[type] [level]` | Changes the game type: `dm`, `tdm`, `ctf`, `koth`, `dc`, `rev`, `run`, `esc`, `bag`, `tbag`, `pit`, `wo`, `gg`, `sal` or `scav`. See the note below. Allowed via rcon. |
| `sv_geolimit` | `<geomods>` | Stock RF command. Sets the geomod limit. Server only. Allowed via rcon. |
| `sv_kickall` | | Stock RF command. Kicks all clients. Server only. |
| `sv_loadconfig` | `[config]` | Loads a server config, or reloads the launch config, and applies it to the current level. Not between levels. ADS only. Allowed via rcon for full-admin rcon profiles only. |
| `sv_loadpackfiles` | | Loads packfiles added to `user_maps\` since the server started. Dedicated only. |
| `sv_netfps` | `[rate]` | Sets how many updates per second the server sends to clients (12-300). Default: 40. Allowed via rcon. |
| `sv_pass` | `[password]` | Sets the server password, or removes it when run without one. Server only. Allowed via rcon. |
| `sv_printconfig` | | Prints the current server configuration. ADS only. |
| `sv_printrules` | `[filename]` | Prints the rules for a level in the rotation, or the active rules for the current level. ADS only. |
| `sv_rconpass` | `[password]` | Sets the rcon password (up to 16 characters), or removes it when run without one. Dedicated only. |
| `sv_record` | | Starts or stops server-side demo recording for the current level. Server only. |
| `sv_restrict_status` | | Shows the current Alpine restriction checks and expected join verdicts. Server only. |
| `sv_timelimit` | `<minutes>` | Stock RF command. Sets the time limit. Server only. Allowed via rcon. |
| `unban_last` | | Unbans the last banned player. Server only. Allowed via rcon. |

`sv_gametype`: on an ADS dedicated server, `sv_gametype <type> <level>` loads the level on that game type, `sv_gametype <type>` restarts the current level on it, and `sv_gametype` alone prints the current game type. It cannot run between levels. On a client that is not connected (or is playing a demo), both arguments are required and it starts a local game like `levelm`. While connected to a server, use `rcon sv_gametype` instead. On other dedicated servers it is refused.

Rcon: on Alpine Faction servers, rcon only runs commands from a fixed list, further limited by the rcon profile of the player who logged in. Besides the commands marked above, the list contains the stock `info`, `say`, `kick`, `ban`, `ban_ip` and `level` commands.

## Bots and waypoints

| Command | Arguments | Description |
| --- | --- | --- |
| `bot_shared_secret` | `[secret]` | Sets a shared secret that signals your client as a bot to the server (0 = disabled). Not available on dedicated servers. Not saved. |
| `sv_bot_add` | `[profile_index] [red\|blue]` | Adds a server bot (profile index -1 = automatic). Server only. Allowed via rcon. |
| `sv_bot_list` | | Lists server bots. Server only. Allowed via rcon. |
| `sv_bot_remove` | `<name\|all>` | Removes a server bot by name, or all server bots. Server only. Allowed via rcon. |
| `waypoints_clean` | | Removes invalid waypoints. |
| `waypoints_debug` | `[0\|1\|2\|3]` | Sets waypoint debug drawing: `0` off, `1` links, zone bounds and target boxes, `2` adds spheres and arrows, `3` adds labels. Without an argument, cycles. |
| `waypoints_delete` | `<w\|z\|t\|l> ...` | Deletes a waypoint, zone, target or link. See the note below. |
| `waypoints_drop` | `[bool]` | Toggles automatic waypoint dropping. |
| `waypoints_edit` | `[bool]` | Toggles waypoint editing mode, which the other `waypoints_*` commands need. Not available as a multiplayer client or in `-bot` mode. Not saved. |
| `waypoints_generate` | `[default\|coarse\|fine]` | Generates a walk-probe waypoint grid from respawn and item waypoints. `default` uses a coarse lattice over open Alpine terrain and probes the rest finely, `coarse` also probes other open ground at a wider spacing, `fine` probes everything finely. Without a mode, uses the loaded graph's mode. |
| `waypoints_load` | | Loads the waypoint graph from the level's `.awp` file. |
| `waypoints_load_all` | | Loads the waypoint graph from `.awp`, including `std_new` waypoints. |
| `waypoints_reset` | | Resets waypoints to the default map grid. |
| `waypoints_save` | | Saves the current waypoint graph to `.awp`. |
| `waypoints_target_add` | `<explosion\|shatter\|jump>` | Adds a waypoint target at the position you are looking at. `shatter` needs breakable glass. |
| `waypoints_target_list` | | Lists waypoint targets. |
| `waypoints_zone_add` | `<zone_type> <trigger\|room\|box> ...` | Adds a waypoint zone. See the note below. |
| `waypoints_zone_list` | | Lists waypoint zones. |

`waypoints_zone_add` forms:

- `waypoints_zone_add <zone_type> trigger <trigger_uid>`
- `waypoints_zone_add <bridge_zone_type> trigger <trigger_uid> [bridge_waypoint_uid ...]`
- `waypoints_zone_add <zone_type> room <room_uid>`
- `waypoints_zone_add <zone_type> box <min_x> <min_y> <min_z> <max_x> <max_y> <max_z>`

Zone types (or their numeric id): `control_point` (`cp`), `damaging_liquid_room` (`liquid`, `liquid_area`), `damage_zone` (`damage`), `instant_death_zone` (`instant_death`, `death`), `bridge_use` (`mover_bridge_use`), `bridge_prox` (`mover_bridge_prox`, `mover_bridge_proximity`), `high_power_zone` (`high_power`, `power_zone`). The bridge types only accept a trigger.

`waypoints_delete` forms: `w <waypoint_uid>`, `z <zone_uid|all>`, `t <target_uid|all>`, `l <from_waypoint_uid> <to_waypoint_uid|all> [both]`. The long names `waypoint`, `zone`, `target` and `link` also work.

## Single player

| Command | Arguments | Description |
| --- | --- | --- |
| `camera4` | | Freezes the camera in place while you keep controlling your character. Not in multiplayer. |
| `camera5` | | Freezes the camera in place, turning to follow your character (tripod). Not in multiplayer. |
| `drop_clutter` | `<class_name>` | Stock RF command. Spawns a clutter object by class name. Not in multiplayer. |
| `drop_entity` | `<class_name>` | Stock RF command. Spawns an entity by class name. Not in multiplayer. |
| `drop_item` | `<class_name>` | Stock RF command. Spawns an item by class name. Not in multiplayer. |
| `jetpack` | | Toggles a jetpack for yourself. Not in multiplayer. |
| `pcollide` | | Toggles your collision with the world. Not in multiplayer; reset to on when a multiplayer game starts. |
| `pogo` | | Toggles pogo: holding jump keeps hopping without losing speed. Not in multiplayer. |
| `skifree` | | Toggles skiing and dodging. Not in multiplayer. |
| `sp_autosave` | | Toggles autosave. Default: on. |
| `sp_camerashake` | | Toggles forcing off all camera shake in single player. Default: off. |
| `sp_climbfix` | | Toggles the fix for getting stuck climbing after leaving a climbing region while crouched. Default: on. |
| `sp_difficulty` | `<easy\|medium\|hard\|impossible>` | Stock RF command. Sets the game difficulty. |
| `sp_exposuredamage` | | Toggles exposure damage when outside without armor in single player. In multiplayer the server's `exposure_damage` rule applies. Default: on. |
| `sp_geochunkphysics` | | Toggles isolated geomod chunks falling as physics objects instead of disappearing. Default: on. |
| `sp_levelhardness` | `[0-100]` | Stock RF command. Sets the default hardness for geomods. Not in multiplayer. |
| `sp_spreadmode` | | Toggles bullet spread between the gaussian method and the legacy uniform method. Default: legacy (uniform). |
| `sp_staticbombcode` | | Toggles the bomb code between randomized and static. Default: randomized. |
| `sp_unlimitedsemiauto` | | Toggles removing the fire rate cooldown of semi-automatic weapons in single player. Default: off. |
| `teleport` | `<x> <y> <z>` | Stock RF command. Teleports you to the coordinates. Not in multiplayer. |

## Debug and diagnostics

| Command | Arguments | Description |
| --- | --- | --- |
| `d_perf_dump` | | Prints performance aggregator call counts and durations. |
| `d_profiler` | | Toggles the profiler overlay. Not in multiplayer (release builds). |
| `d_profiler_log` | | Toggles the profiler log. |
| `d_profiler_print` | | Prints average times of each profiler. |
| `dbg_achievement` | `<uid>` | Looks up an achievement by UID. |
| `dbg_berserk` | | Makes all entities go berserk. Single player only. |
| `dbg_bot` | `[0\|1]` | Toggles bot debug console logging. Default: off. |
| `dbg_collision_pairs` | | Prints object collision pair pool statistics. |
| `dbg_consolehistory` | | Toggles keeping console history between game launches. |
| `dbg_events` | | Toggles printing event debug information. Default: off. |
| `dbg_framegraph` | | Toggles the frame time graph. |
| `dbg_frameratetest` | | Stock RF command. Starts a frame rate test. |
| `dbg_goal` | `<name>` | Prints the initial and current value of a named goal (normal and persistent). |
| `dbg_loadsettings` | | Reads and applies player settings from the settings file. |
| `dbg_mapver` | `<filename>` | Prints the RFL version of a level file and whether this build can load it. |
| `dbg_numgeos` | | Prints the number of geomod craters in the current level. Single player or server only. |
| `dbg_particlegpu` | | Toggles GPU rendering of particles, plankton and weather (off = stock CPU path). D3D11 only. Default: on. Not saved. |
| `dbg_particles` | | Toggles the particle performance overlay. |
| `dbg_particles_log` | `[seconds]` | Logs particle performance counters for a number of seconds (1-600, default 10). |
| `dbg_particlewarmup` | | Toggles ramping a frozen level particle emitter back up when it becomes visible. Default: on. Not saved. |
| `dbg_pow2tex` | `[0\|1\|-1]` | Overrides power-of-2 texture enforcement (`0` off, `1` on, `-1` automatic). Applies on the next level load. |
| `dbg_resetachievements` | | Resets achievement notifications. |
| `dbg_room_clip_wnd` | | Toggles showing room clip windows. |
| `dbg_savesettings` | | Writes the current player settings and system config files. |
| `dbg_shadows` | | Toggles the shadow map debug overlay. Single player or host only. |
| `dbg_systeminfo` | | Stock RF command. Shows system information. |
| `dbg_tbl_baseline` | | Reports `weapons.tbl` and `entity.tbl` deviations from stock as sent to FactionFiles. Dedicated only. |
| `dbg_terrain_batches` | `[0\|1]` | Tints terrain faces by the batch that draws them. D3D11 only. Default: off. |
| `dbg_terrain_decorations` | `[0\|1]` | Toggles terrain mesh decorations and prints their draw statistics. Default: on. |
| `dbg_timedemo` | | Stock RF command. Starts a timedemo. |
| `dbg_togglerendering` | | Toggles all game rendering. Default: on. |
| `dbg_togglesound` | | Toggles all game sound. Default: on. |
| `dbg_vehicle_spawn` | `[class] [player]` | Spawns a vehicle or turret entity (default `Jeep01`) in front of a player. Multiplayer server only. |
| `dbg_vehnav` | `[0\|1] \| list [first_node] \| path <from_node> <to_node> [size]` | Draws and lists the vehicle navigation layer of the AWP. `path` sizes: `0` jeep, `1` APC, `2` driller. |
| `dbg_verifylevel` | | Stock RF command. Verifies the current level. |
| `dbg_vfxgpu` | | Toggles GPU rendering of `.vfx` meshes (off = stock CPU path). D3D11 only. Default: on. Not saved. |
| `dbg_vphys` | `[0\|1]` | Draws vehicle physics collision primitives in the world. |
| `dbg_wh_mpspawns` | | Toggles world HUD indicators for multiplayer respawn points (single player or host only). |
| `debug` | `<flag>` | Toggles a stock debug flag. See the note below. |

`debug` flags: `thruster`, `light`, `light2`, `push_climb_reg`, `geo_reg`, `glass`, `mover`, `ignite`, `movemode`, `perf`, `perfbar`, `waypoint`, `particle_emitter`, `network`, `particlestats`, `weapon`, `event`, `trigger`, `objrender`, `roomstats`, `trans`, `room`, `portal`, `lightmap`, `nolightmap`, `show_invisible_faces`. Only `network` works in multiplayer.

## Debug builds only

These commands are only registered in debug builds of Alpine Faction.

| Command | Arguments | Description |
| --- | --- | --- |
| `d_ai_pause` | | Toggles pausing AI. |
| `d_entity_action` | `[action]` | Plays an action animation on the target entity; without an argument, cycles. |
| `d_entity_state` | `[state]` | Forces a state animation on the target entity; without an argument, cycles. |
| `d_hang` | | Freezes the game for 6 seconds. |
| `d_hud_coords` | `<index> [x] [y]` | Prints or sets the coordinates of a HUD element. |
| `d_mem_stats` | | Prints heap allocation statistics. |
| `d_playing_sounds` | | Lists the playing DirectSound channels. |
| `d_reload_fpgun` | | Reloads first person weapon meshes. |
| `d_spin` | `<speed>` | Spins the target object at the given yaw rate. |
| `d_target_closest` | | Targets the closest object. |
| `d_target_reticle` | | Targets the object under the reticle. |
| `d_target_uid` | `[uid]` | Targets the object with the UID, or yourself without one. |
| `detect_ftol_issues` | `[fps]` | Toggles detection of float-to-int conversion issues at a frame rate (default 240). |
| `profile_frame` | | Profiles Direct3D calls for the next frame. Legacy Direct3D renderer only. |
| `sound_stress_test` | `<num>` | Plays the given number of random 3D sounds per second around you (0 stops). |

Debug builds also register these stock RF developer commands: `body`, `drop_fixed_cam`, `endgame`, `fall_factor`, `fogme`, `freelook`, `glares_toggle`, `hide_target`, `invulnerable`, `irc`, `irc_close`, `irc_far`, `jump_height`, `loadgame`, `make_myself`, `max_plankton`, `mouse_cursor`, `mouse_look`, `orbit_cam`, `play_bik`, `player_step`, `savegame`, `screen_res`, `set_ammo`, `set_armor`, `set_georegion_hardness`, `set_life`, `set_mass`, `set_primary`, `set_secondary`, `set_skin`, `set_vehicle_bounce`, `set_view`, `set_weapon`, `show_obj_times`, `splitscreen`, `splitscreen_bot_test`, `splitscreen_swap`, `wfar`.
