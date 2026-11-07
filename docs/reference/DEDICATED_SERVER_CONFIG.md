# Dedicated Server Configuration

Alpine Faction dedicated servers are configured with a single TOML file, called the ADS (Alpine Dedicated Server) config. It holds the server's core settings, the rules every level is played with, per-level rule overrides and the level rotation.

While this reference document covers all of the available settings, the easiest way to configure a dedicated server is to use the web-based [Alpine Faction Server Configuration Utility](https://dedi.alpinefaction.com/).

## Starting a server

Start a dedicated server by passing the config file to the launcher with `-ads`:

```
AlpineFactionLauncher.exe -ads myserver.toml
```

`-ads` implies a game launch and a dedicated server, so no other switch is needed. A relative path is resolved against the Red Faction game directory (the folder holding `RF.exe`). The stock `-dedicated` switch and `dedicated_server.txt` are no longer supported: launching with `-dedicated` prints a deprecation notice and exits.

Other switches that affect an ADS server:

| Switch | Effect |
| --- | --- |
| `-ads <file>` | Run a dedicated server from this config file. |
| `-min` | Print only the core settings at startup instead of the full config. |
| `-log` | Also write the full server console output to `logs\AlpineFaction-console-<date>-<time>.log`. |
| `-nodl` | Do not download rotation or vote levels that are not installed. By default missing levels are fetched from FactionFiles at startup, and levels that cannot be fetched are dropped. |
| `-nogsk` | Ignore `fflink_gsk` from the config. |
| `-lanonly` | Stock switch: do not register with the public server tracker. |
| `-port <port>` | Stock switch: UDP port to host on (7755 when not given). |
| `-mod <name>` | Stock switch: run a TC mod. See `require_client_mod`. |

The server log is written to `logs\AlpineFaction-dedicated-<config name>.log`. Target FPS and network FPS are not part of this file: they come from `ServerMaxFPS` and `ServerNetFPS` in `alpine_settings.ini`.

## Console commands

| Command | Effect |
| --- | --- |
| `sv_loadconfig [file]` | Reload the launch config, or load `file` instead. Only works during gameplay, not between levels. Levels that are not installed are skipped (there is no download on reload). The current level's rules are re-applied at once, and the level restarts if its game type changed. All rcon sessions are cleared, so rcon holders must log in again. If the file fails to parse, the error is printed and the previous config stays in force. Over rcon it needs a `full_admin` profile. |
| `sv_printconfig` | Print the full config in force, including any session overrides from votes. |
| `sv_printrules [filename]` | Print the rules for a rotation level, or for the current level when no filename is given. |

## Structure and rule layering

The file has four kinds of content:

1. **Top-level server settings**: plain keys before the first `[table]` header (server name, passwords, demo recording, client permissions) and server-wide tables such as `[inactivity]`, `[[rcon_profiles]]` and the `[vote_*]` tables. They apply to the whole server and cannot be set per level.
2. **Base rules** in `[base]`: game type, limits, spawn settings, mutators and so on. These are the default for every level.
3. **Per-level overrides** in the `rules` table of each `[[levels]]` entry, written as `[levels.rules]` under that entry.
4. **The level rotation**: the `[[levels]]` entries themselves, played in file order (shuffled when `dynamic_rotation` is on). A level may appear more than once. With no usable levels the server runs `glass_house.rfl`.

TOML puts every key that follows a `[table]` header into that table. To add top-level keys after a table, open a `[root]` table: its keys are read exactly as if they were at the top of the file.

The file is read in two passes:

1. **Core pass.** Every top-level key and table except `[[levels]]` is applied in file order, including `[base]`.
2. **Levels pass.** Each `[[levels]]` entry gets its own complete rule set, built in this order:
   1. The game type is taken from `game_type` in the entry's `rules` table (or the entry itself), else from `[base]`.
   2. That game type's defaults are applied (see [Game types](#game-types)).
   3. The base tables are replayed: `[base]`, then `[base.rules]` if present. Base settings therefore apply on top of every game type's defaults, including levels that switch game type.
   4. The entry's own tables are applied: keys written directly in the `[[levels]]` entry, then `[levels.rules]`.

Inside each table, its `mutators` are applied first and its other keys after them, so an explicit key always wins over a value a mutator set. The full order is: game type defaults, `[base]` mutators, `[base]` keys, `[base.rules]` mutators, `[base.rules]` keys, level mutators, level keys.

At runtime:

- A mutator set chosen by a vote replaces every base and level mutator and is applied last.
- A level that is not in the rotation but is loaded anyway (with `level`/`map` or a level vote) gets the base rules over the defaults of its own game type. That game type is RUN for a known Run map, else the base game type if the filename suits it, else the game type of the filename prefix, else the base game type.
- If a level starts under a different game type than its rules name (for example after `sv_gametype`), the rules are rebuilt for that game type rather than carried over.

How inherited values combine:

- Most rule sub-tables (`spawn_life`, `gibbing`, `spawn_selection`, `kill_rewards` and the rest) start from the inherited values, so a level only needs to name the keys it changes. **`overtime` is the exception**: an `overtime` table replaces the inherited one entirely, and keys it leaves out return to their defaults.
- Keys inside a sub-table marked "read only when `enabled`" are ignored unless `enabled` is true after inheritance.
- `spawn_loadout`, `spawn_loadout_blue`, `weapon_stay_exemptions`, `item_replacements` and `item_respawn_time_overrides` merge by name: a narrower scope adds entries or changes existing ones. `delayed_items` can only add items. `gg_tiers` replaces the inherited list, and so does `spawn_selection`'s `dynamic_respawn_items` (see that table).

Warnings:

- Unknown keys at the top level, in `[base]` or in a rules table are silently ignored.
- Unknown keys directly inside a `[[levels]]` entry print a warning. They are still applied as rule keys, but rule keys belong in `[levels.rules]`.
- A key written after a `[[base.mutators]]` or `[[levels.rules.mutators]]` header becomes part of that mutator entry. The server warns about keys a mutator does not know.

## Top-level settings

In the tables below, a Number key takes an integer or a decimal value, and an Integer key should be written without a decimal point. Values outside the stated range are clamped.

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `ads_version` | Integer | `2` | Config format version. Read, but not used for anything. |
| `server_name` | String | `"Alpine Faction Server"` | Name shown in server browsers. |
| `max_players` | Integer | `8` | Player slots, clamped to 1-32. |
| `password` | String | `""` | Join password. Empty means no password. Truncated to 16 characters. |
| `rcon_password` | String | `""` | Legacy rcon password, truncated to 15 characters. Unless an `[[rcon_profiles]]` entry uses the same password, it creates a profile named `legacy` limited to `gt`, `kick`, `level`, `sv_pass`, `map`, `ban`, `ban_ip`, `map_ext`, `map_rest`, `map_next`, `map_rand`, `map_prev`, `sv_caplimit`, `sv_fraglimit`, `sv_gametype`, `sv_geolimit`, `sv_timelimit` and `unban_last`. |
| `bot_shared_secret` | Integer | `0` | Secret (0-4294967295) that bot clients must present to be recognized as bots. `0` disables bot client authentication. |
| `server_bots` | Integer | `0` | Number of server-side bots (run inside the server process), clamped to 0-31. They use the `bot_profiles` profiles and follow `ideal_player_count`. |
| `bot_profiles` | Array of strings | `[]` | Bot profile files, relative to this config file's folder. Each joining bot takes the profile used by the fewest current bots (the earliest one on a tie), so profiles are shared out evenly. See [Bot profile files](#bot-profile-files). |
| `fflink_gsk` | String | `""` | FactionFiles game server key, used for stats tracking and demo uploads. If FactionFiles rejects the key at startup, the server refuses to start. Ignored with `-nogsk`. |
| `upnp` | Boolean | `false` | Try to forward the server port with UPnP. |
| `demo_auto_record` | Boolean | `false` | Record every level to an `.afd` demo in the `demos` folder. Recordings with no player connected are discarded. With match mode, only live matches are recorded. |
| `demo_chat_record` | Boolean | `true` | Include chat in recorded demos. |
| `fflink_demo_upload` | Boolean | `true` | Upload auto-recorded demos to FactionFiles. Needs a working `fflink_gsk`. |
| `fflink_demo_max_mb` | Integer | `100` | Largest demo, in MiB, that will be uploaded. FactionFiles caps this at 100 MiB; values below 1 count as 1. |
| `fflink_demo_queue_max` | Integer | `32` | Most demos waiting to upload. When full, the oldest queued demo is dropped from the queue. Values below 1 count as 1. |
| `fflink_demo_delete_after_send` | Boolean | `false` | Delete a demo after it uploads. When false, uploaded demos are moved to `demos\uploaded`. |
| `dynamic_rotation` | Boolean | `false` | Shuffle the rotation at load and each time its last level is reached. A random-level vote then just loads the next level. |
| `require_client_mod` | Boolean | `true` | When the server runs a TC mod (`-mod`), clients must run the same mod. |
| `gaussian_spread` | Boolean | `true` | Use gaussian (center-weighted) bullet spread. |
| `send_stats_message` | Boolean | `true` | Send players their stats message at the end of each level. |
| `allow_fullbright_meshes` | Boolean | `false` | Let clients render character meshes fullbright. |
| `allow_lightmap_mode` | Boolean | `false` | Let clients use lightmap-only rendering. |
| `allow_disable_screenshake` | Boolean | `true` | Let clients turn off weapon screenshake. |
| `allow_disable_muzzle_flash` | Boolean | `true` | Let clients turn off muzzle flash lights. |
| `allow_unlimited_fps` | Boolean | `false` | Let clients lift the 240 FPS cap. |
| `allow_footsteps` | Boolean | `true` | Let clients hear other players' footsteps. |
| `use_sp_damage_calculation` | Boolean | `false` | Use single-player damage calculation, where armor does not fully protect health. |
| `allow_outlines` | Boolean | `false` | Let clients draw player outlines. |
| `allow_outlines_xray` | Boolean | `true` | Let outlines show through walls (only matters when `allow_outlines` is on). |

## `[[rcon_profiles]]`

Each entry is one rcon login. Logging in with a profile's password grants that profile's commands.

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `name` | String | required | Profile name. Entries without one are skipped. |
| `password` | String | required | Profile password, truncated to 15 characters. Entries without one are skipped. |
| `full_admin` | Boolean | `false` | Allow every rcon-capable command. |
| `allow_multiple` | Boolean | `false` | Let several players hold this profile at once. When false, a new login removes the previous holder. |
| `allowed_commands` | Array of strings | `[]` | Commands this profile may run (ignored for `full_admin`). Only these are accepted: `info`, `say`, `kick`, `ban`, `ban_ip`, `unban_last`, `level`, `map`, `map_ext`, `map_rest`, `map_next`, `map_rand`, `map_prev`, `maxfps`, `sv_caplimit`, `sv_fraglimit`, `sv_gametype`, `sv_netfps`, `gt`, `sv_geolimit`, `sv_pass`, `sv_timelimit`, `download_level`, `sv_afstats_events_status`, `sv_afstats_events_reset`, `sv_afstats_trace`, `sv_fflink_status`, `sv_fflink_resync`, `sv_bot_add`, `sv_bot_remove`, `sv_bot_list`. `sv_loadconfig` is accepted in the list but only ever runs for `full_admin` profiles. |

## `[inactivity]`

Marks players who stop playing as idle and can kick them.

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `enabled` | Boolean | `true` | Track inactive players. The other keys are read only when this is true. |
| `new_player_grace` | Number | `120` | Seconds a newly joined player is exempt (minimum 1). |
| `allowed_inactive` | Number | `60` | Seconds of inactivity before a player is marked idle (minimum 1). |
| `kick_after_warning` | Boolean | `false` | Warn idle players, then kick them if they do not respawn in time. |
| `warning_duration` | Number | `10` | Seconds between the warning and the kick (minimum 1). Read only when `kick_after_warning` is true. |
| `kick_message` | String | `"You have been marked as idle due to inactivity! You will be kicked from the game unless you respawn in the next 10 seconds."` | Warning text. Read only when `kick_after_warning` is true. |

## `[damage_notifications]`

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `enabled` | Boolean | `true` | Notify attackers (and players spectating them) of the damage they deal. |
| `support_legacy` | Boolean | `true` | Send a plain hit sound to clients that cannot receive damage notifications (anything other than Alpine Faction 1.1 or newer). Read only when `enabled` is true. |

## `[sprays]`

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `enabled` | Boolean | `true` | Allow players to use sprays. |
| `cooldown_ms` | Integer | `500` | Milliseconds between one player's sprays, clamped to 0-600000. Read only when `enabled` is true. |

## `[alpine_restrict]`

Restricts which clients may play. Restricted clients can still join and spectate, but cannot spawn, unless they are rejected outright. Some rules (game types, mutators, and levels needing a newer client) make the server require Alpine Faction automatically, whatever these keys say. `sv_restrict_status` shows what is in force.

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `advertise_alpine` | Boolean | `true` | Send joining players on other clients a chat message recommending Alpine Faction, and tell players on an older Alpine Faction release that a new version is available. |
| `only_welcome_alpine` | Boolean | `false` | Send the welcome message only to Alpine Faction players. |
| `clients_require_alpine` | Boolean | `false` | Only Alpine Faction clients may spawn. The three keys below are read only when this is true. |
| `reject_non_alpine_clients` | Boolean | `false` | Refuse the connection instead of letting restricted clients spectate. |
| `alpine_require_release_build` | Boolean | `false` | Also restrict Alpine Faction clients that are not a stable release (for example dev builds). |
| `require_d3d11` | Boolean | `false` | Also restrict Alpine Faction clients that are not using the Direct3D 11 renderer. |

## `[click_limiter]`

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `enabled` | Boolean | `true` | Limit how fast semi-automatic weapons can fire. |
| `cooldown` | Integer | `90` | Minimum milliseconds between semi-automatic shots, clamped to 0-5000. Read only when `enabled` is true. |

## Vote tables

Each vote type has its own table. All of them take these keys:

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `enabled` | Boolean | `false` | Allow players to call this vote. The other keys are read only when this is true. |
| `ignore_nonvoters` | Boolean | `false` | When the vote times out, decide it from the votes cast. When false, players who did not vote count as voting no. Always treated as false during a match. |
| `time` | Number | `60` | Seconds the vote stays open, clamped to 1-65535. |

| Table | Vote |
| --- | --- |
| `[vote_match]` | Start a ready-up match. Offered only when the base game type is a team game type. |
| `[vote_kick]` | Kick a player. |
| `[vote_level]` | Load a chosen level, optionally with a game type and mutators. Takes the extra keys below. |
| `[vote_extend]` | Extend the time limit. |
| `[vote_restart]` | Restart the current level. |
| `[vote_next]` | Load the next rotation level. |
| `[vote_rand]` | Load a random rotation level (the next one when `dynamic_rotation` is on). |
| `[vote_previous]` | Load the previous rotation level. |

### `[vote_level]` extra keys

These are read only when `enabled` is true.

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `allowed_levels` | Array of strings | `[]` | Levels players may vote for. `.rfl` is added when missing. Missing levels are downloaded at startup unless `-nodl` is used. |
| `add_rotation_to_allowed_levels` | Boolean | `true` | Also allow every rotation level. |
| `add_installed_to_allowed_levels` | Boolean | `false` | Also allow every installed level whose filename starts with a game type prefix, including levels installed while the server runs. |
| `only_allow_gametype_prefix` | Boolean | `false` | Accept a level for a voted game type only if the filename has that game type's prefix (or the game type is the level's own default). |

If neither the rotation nor `allowed_levels` names a level, `glass_house.rfl` is the only votable level.

## `[base]` rules

`[base]` holds the rules for every level. The same keys can go in a nested `[base.rules]` table, which is applied after `[base]`'s own keys. Every key in this section can also appear in a `[levels.rules]` table to override it for one level. Defaults marked "game type" depend on the game type; see [Game types](#game-types).

### Game type and mutators

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `game_type` | String | `"DM"` | Game type identifier from [Game types](#game-types), case-insensitive. An unrecognized name falls back to `DM`. Only read in `[base]`, `[base.rules]`, a `[[levels]]` entry or `[levels.rules]` (not at the top level). |
| `mutators` | Array of tables | none | Mutators for this scope, written as `[[base.mutators]]` or `[[levels.rules.mutators]]`. See [Mutators](#mutators). |

### Limits

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `time_limit` | Number | `600` | Level time limit in seconds (minimum 10). |
| `individual_kill_limit` | Integer | `30` | DM frag limit, 1-32767. |
| `team_kill_limit` | Integer | `100` | TDM team score limit, 1-65535. |
| `cap_limit` | Integer | `5` | CTF capture limit, 1-65535. |
| `koth_score_limit` | Integer | `100` | KOTH team score limit, 1-65535. |
| `dc_score_limit` | Integer | `300` | DC team score limit, 1-65535. |
| `pit_score_limit` | Integer | `10` | PIT player score limit, 1-32767. |
| `gg_score_limit` | Integer | `30` | GG player score limit, 1-32767. |
| `bag_score_limit` | Integer | `150` | BAG player score limit, 1-32767. |
| `tbag_score_limit` | Integer | `300` | TBAG team score limit, 1-65535. |
| `sal_cap_limit` | Integer | `5` | SAL capture limit, 1-65535. |
| `scav_cap_limit` | Integer | `15` | SCAV capture limit, 1-65535. |
| `geo_limit` | Integer | `64` | Geomod crater limit, 0-128. |
| `rf2_geo_limit` | Integer | `-1` | RF2-style geomod crater limit. `-1` is unlimited, `0` disables RF2-style geomod, higher values set a limit. |

### Objective timing

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `flag_return_time` | Number | `25` | Seconds before a dropped CTF flag returns (minimum 1). |
| `bag_return_time` | Number | `7.5` | Seconds before a dropped BAG/TBAG bag returns, 1-60. |
| `bag_spawn_delay` | Number | `7.5` | Seconds before the BAG/TBAG bag spawns, 0-60. |
| `sal_flag_spawn_delay` | Number | `15` | Seconds before the SAL flag spawns, 0-60. |
| `sal_flag_capture_respawn_delay` | Number | `5` | Seconds before the SAL flag respawns after a capture, 0-60. |
| `sal_flag_return_time` | Number | `15` | Seconds before a dropped SAL flag returns, 1-60. |
| `scav_cartridge_spawn_delay` | Number | `10` | Seconds before the SCAV cartridge spawns, 0-60. |
| `scav_cartridge_capture_respawn_delay` | Number | `5` | Seconds before the SCAV cartridge respawns after a capture, 0-60. |
| `scav_cartridge_return_time` | Number | `30` | Seconds before a dropped SCAV cartridge returns, 1-60. |
| `scav_target_timeout` | Number | `120` | Seconds before the SCAV target moves, 10-600. |
| `scav_target_radius` | Number | `1.5` | Radius of the SCAV target in meters, 1-10. |

### Gameplay flags

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `team_damage` | Boolean | `false` | Players can damage teammates. |
| `fall_damage` | Boolean | `false` | Players take fall damage. |
| `exposure_damage` | Boolean | `true` | Players take exposure damage when outside without armor, as in single player. |
| `weapons_stay` | Boolean | `false` | Weapon pickups stay after being picked up (see `weapon_stay_exemptions`). |
| `force_respawn` | Boolean | `false` | Respawn dead players automatically. |
| `balance_teams` | Boolean | `false` | Balance the teams when a team level starts. Teams are always balanced when the server moves from a non-team game type to a team game type. |
| `auto_team_balance` | Boolean | `false` | During play, move players when the team sizes differ by 2 or more on three checks in a row (checked every 15 seconds). Not used during matches. |
| `ideal_player_count` | Integer | `32` | Target player count that bots fill up to, 1-32. At 32, bots are never removed to make room. |
| `saving_enabled` | Boolean | game type | Let players save and load their position. On by default only in RUN. |
| `flag_dropping` | Boolean | `true` | Let CTF flag carriers drop the flag. |
| `flag_captures_while_stolen` | Boolean | `false` | Let a team capture while its own CTF flag is stolen. |
| `pvp_damage_modifier` | Number | game type | Multiplier for damage between players, 0-100. `1.0` everywhere except RUN (`0.0`). |
| `drop_amps` | Boolean | `false` | Players drop damage amplifiers when they die. |
| `drop_weapons` | Boolean | `true` | Players drop their weapon when they die. Off in PIT, GG, WO and SCAV. |
| `no_player_collide` | Boolean | game type | Players pass through each other. On by default only in RUN. |
| `location_pinging` | Boolean | game type | Allow location pings. On by default in KOTH, DC, REV, RUN, ESC, TBAG, SAL, WO and SCAV. |
| `geo_chunk_physics` | Boolean | `true` | Geomod chunks that are cut loose fall down. |
| `clear_stale_movement_input` | Boolean | `false` | Clients clear their movement and look input whenever they leave gameplay (for example by opening a menu), so a held key cannot keep moving them. |
| `weapon_pickups_give_full_ammo` | Boolean | `false` | Weapon pickups fill the weapon's ammo. |
| `infinite_reloads` | Boolean | game type | Reloading does not use reserve ammo (except the Fusion Rocket Launcher). On by default in PIT, WO and SCAV. |
| `force_rail_reload` | Boolean | `true` | Players must finish reloading the rail driver before switching weapons. |

### Gun Game

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `gg_rampage_rewards` | Boolean | `true` | Reward players who advance through several weapons in one life. |
| `gg_tiers` | Array of arrays of strings | built-in tiers | Weapon progression. Each inner array is one tier of weapons.tbl names; tiers are played in order, and each player gets each tier in a random order. Unknown names are skipped and empty tiers dropped. If nothing resolves, the built-in tiers are used. |
| `gg_final_weapon` | String | `"Riot Stick"` | Weapon for the final level. An unknown name falls back to the Riot Stick. |
| `gg_end_with_fusion` | Boolean | `true` | After the final weapon, add a one-kill Fusion Rocket Launcher level. |

### Spawn loadout lists

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `spawn_loadout` | Array of tables | game type | Weapons every player spawns with. Entries take the keys below. |
| `spawn_loadout_blue` | Array of tables | none | When given, blue players spawn with this list instead of `spawn_loadout`. It does not inherit the game type's loadout, so list every weapon. Same entry keys. |

Loadout entry keys:

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `weapon_name` | String | required | weapons.tbl name, for example `"Shotgun"`. An unknown name prints a warning and the entry is ignored. |
| `ammo` | Integer | keep inherited, else `0` | Reserve ammo. When left out, an entry inherited from a broader scope keeps its reserve. |
| `include` | Boolean | `true` | `false` removes this weapon from an inherited loadout. |

### Other lists

| Key | Type | Entry keys | Description |
| --- | --- | --- | --- |
| `weapon_stay_exemptions` | Array of tables | `weapon_name` (String, weapons.tbl name), `exempt` (Boolean, default `true`) | Weapons whose pickups disappear when picked up even when `weapons_stay` is on. `shoulder_cannon` (the Fusion Rocket Launcher) is exempt by default, as in the stock game. Every rules table resets that default before reading its own list, so `exempt = false` for `shoulder_cannon` only holds when it is in the last table read for a level: for a rotation level, put it in that level's `[levels.rules]`, because a value set only in `[base]` is reset. |
| `item_replacements` | Array of tables | `original` (String), `replacement` (String) | Replace one items.tbl item with another when the level loads. An empty `replacement` removes the item. Entries with an unknown `original` are ignored. |
| `item_respawn_time_overrides` | Array of tables | `item_name` (String), `respawn_ms` (Integer, default `0`) | Respawn time in milliseconds for an items.tbl item. Unknown items are ignored. |
| `delayed_items` | Array of tables | `item_name` (String) | Items that start hidden when the level loads and first appear once their respawn time has passed. Unknown items are ignored. A narrower scope can add items but not remove them. |

### `overtime`

`[base.overtime]` or `[levels.rules.overtime]`. This table does not inherit: it replaces the broader scope's overtime settings, and keys left out return to these defaults.

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `enabled` | Boolean | `false` | Add overtime when the time limit runs out on a tie. The other keys are read only when this is true. |
| `time` | Integer | `5` | Overtime length in minutes, 0-60. |
| `tie_when_flag_stolen` | Boolean | `false` | CTF: treat the game as tied while a flag is stolen. |
| `tie_when_hill_contested` | Boolean | `true` | KOTH: treat the game as tied while a hill is contested. |

### `rounds`

Only used by round-based game types (PIT and WO); ignored elsewhere.

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `max_rounds` | Integer | `5` (WO: `7`) | Rounds per level, 1-999. In WO this is a best-of-N, so a team needs `max_rounds / 2 + 1` round wins. |
| `round_time` | Integer | `90` (WO: untimed) | Seconds per round, 10-3600. WO rounds are untimed by default and only end on a team wipe; setting this gives them a time limit, and there is no value that makes them untimed again. |
| `post_round_time` | Integer | `3` | Seconds of celebration after a round ends, 0-10. |
| `intermission_time` | Integer | `3` (WO: `5`) | Seconds of countdown between rounds, 0-10. |

### `spawn_weapon`

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `weapon_name` | String | game type | weapons.tbl name of the weapon players spawn holding. It is added to the loadout, replacing the previous spawn weapon unless `spawn_loadout` lists that one. An unknown name is ignored. |
| `clips` | Integer | `3` | Reserve ammo for the spawn weapon, in multiplayer clips. |

### `spawn_life` and `spawn_armor`

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `enabled` | Boolean | `false` (PIT, WO, SCAV: `true`) | Use a custom spawn health (`spawn_life`) or armor (`spawn_armor`). |
| `value` | Number | `100` (PIT, WO, SCAV: `200`) | Spawn health or armor, 1-255. Read only when `enabled` is true. |

### `spawn_delay`

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `enabled` | Boolean | game type | Make players wait before respawning. |
| `base_seconds` | Number | `5` (game type) | Respawn delay in seconds, 0.25-60. Read only when `enabled` is true. In WO the delay is multiplied by the player's deaths this round. |

### `spawn_protection`

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `enabled` | Boolean | `false` | Protect newly spawned players. When false, players get no spawn protection at all. |
| `duration` | Number | `1.5` | Protection length in seconds, 0-3600. Read only when `enabled` is true. |
| `use_powerup` | Boolean | `false` | Grant an invulnerability powerup for the duration instead of plain spawn protection. Read only when `enabled` is true. |

### `spawn_selection`

How the server picks a respawn point. The defaults match the stock game.

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `respect_team_spawns` | Boolean | `true` | In team game types, use only the player's team's respawn points. |
| `try_avoid_players` | Boolean | `true` | Prefer points far from other players. |
| `always_avoid_last` | Boolean | `false` | Never reuse the player's previous respawn point. |
| `always_use_furthest` | Boolean | `false` | Always pick the point furthest from other players. |
| `only_avoid_enemies` | Boolean | `false` | Ignore teammates when measuring distance to other players. |
| `dynamic_respawns` | Boolean | `false` | Create extra respawn points at the positions of chosen items. Dedicated servers only. |
| `dynamic_respawn_items` | Array of tables | `[]` | Items that create respawn points. Whenever a `spawn_selection` table is read while `dynamic_respawns` is true, the list is rebuilt from that table alone, so a level that sets any `spawn_selection` key must repeat this list. Entry keys: `item_name` (String, items.tbl name) and `min_respawn_points` (Integer, default `8`): points are created only while the level has fewer respawn points than this; `0` means always. |

### `kill_rewards`

Health and armor granted to a player for each kill.

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `health` | Number | `0` | Health added per kill. |
| `armor` | Number | `0` | Armor added per kill. |
| `effective_health` | Number | `0` (GG: `50`) | Points that fill health first; the remainder becomes armor at half rate (2 points per armor point). |
| `health_is_super` | Boolean | `false` | Let rewards raise health up to 200 instead of the normal maximum. |
| `armor_is_super` | Boolean | `false` | Let rewards raise armor up to 200 instead of the normal maximum. |

### `gibbing`

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `enabled` | Boolean | `false` | Overkill deaths explode into gibs. |
| `damage_threshold` | Number | `100` | Damage a killing hit must exceed to gib, 1-6000. Read only when `enabled` is true. |
| `all_damage_types` | Boolean | `false` | Any damage type can gib, not only explosions. Read only when `enabled` is true. |

### `vehicles`

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `crash_damage` | Boolean | `true` | Vehicles take damage from hard impacts, hard landings and scraping. |
| `bots_use_vehicles` | Boolean | `true` | Bots may board vehicles. |
| `bot_max_drivers` | Integer | `4` | Most vehicles bots may drive at once, 0-32. |

### `welcome_message`

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `enabled` | Boolean | `false` | Send a message to each player who joins. |
| `text` | String | `""` | Message text, up to 240 characters. `$PLAYER` is replaced with the player's name. Read only when `enabled` is true. |

### `level_message`

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `enabled` | Boolean | `false` | Send a message to all players when a level loads. |
| `text` | String | `""` | Message text, up to 240 characters. `$LEVEL` is replaced with the level's name. Read only when `enabled` is true. |

### `force_character`

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `enabled` | Boolean | `false` | Make every player use one character. |
| `character` | String | `"enviro_parker"` | Multiplayer character name. An unknown name selects the first character (`enviro_parker`). Read only when `enabled` is true. |

## `[[levels]]`

Each `[[levels]]` entry adds one level to the rotation, in file order. `.rfl` is added to the filename when missing. A level that is not installed is downloaded from FactionFiles at startup (unless `-nodl` is used) and dropped from the rotation if that fails.

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `filename` | String | required | Level filename. |
| `rules` | Table | none | Rule overrides for this level, written as `[levels.rules]` after the entry. Takes every key from [`[base]` rules](#base-rules), including `game_type`, `mutators` (`[[levels.rules.mutators]]`) and sub-tables such as `[levels.rules.overtime]`. |
| `rules_presets` | any | none | Removed. Still accepted without a warning, but ignored. |

## Mutators

Mutators are declared in a `mutators` array in any rules scope:

```toml
[[base.mutators]]
name = "oneweapon"
featured_weapon = "Shotgun"
```

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `name` | String | required | Mutator identifier from the table below, case-insensitive. Unknown names print a warning. |
| `enabled` | Boolean | `true` | `false` removes a mutator that a broader scope declared (for example, a level turning off a base mutator). |
| *options* | varies | varies | The mutator's own options, listed below. |

A mutator declared more than once in the same table uses its last declaration. A level can redeclare a base mutator with different options. Active mutators are applied in a fixed order, so a later one wins where they overlap. The order is `joinbots`, `pogo`, `dodging`, `skiing`, `lowgravity`, `weirdgungame`, `humansvsbots`, `jetpacks`, `gibbing`, `flamingenemies`, `bigcraters`, `superrail`, `delayedsupers`, `armored`, `superdrain`, `vampire`, `crits`, `arena`, `oneweapon`, `instagib`, `scorelimit`, `idealplayers`. A mutator that does nothing in the scope's game type prints a warning and is not applied. Mutators marked "1.4+" need Alpine Faction 1.4 or newer clients, and the server restricts older clients while they are active.

| Identifier | Name | Effect | Options | Clients |
| --- | --- | --- | --- | --- |
| `instagib` | Instagib | Players spawn with only the rail driver, which never reloads and cannot be switched away from. Infinite ammo, all pickups hidden, no weapon drops, 0.5 second spawn delay. | none | 1.4+ |
| `oneweapon` | One Weapon | Players spawn with the baton. Every weapon and ammo pickup becomes the featured weapon's pickup; all other pickups are hidden. Weapons stay is turned on, 0.5 second spawn delay. | `featured_weapon` (String, weapons.tbl name of a weapon that has a pickup; default the rail driver), `exclude_thrown` (Boolean, default `true`: Remote Charge and Grenade pickups are left alone) | 1.4+ |
| `arena` | Arena | 100 health and 100 armor at spawn, refilled by each kill (+300 effective health). Spawn with the baton and Assault Rifle. The current weapon reloads instantly on a kill, magazines are infinite, weapon pickups give full ammo. Only weapon pickups remain; no weapon drops; force respawn; 0.5 second spawn delay. | none | 1.4+ |
| `vampire` | Vampire | Damage dealt to other players heals the attacker by half that amount, as effective health. | `hide_health_armor_pickups` (Boolean, default `true`), `full_lifesteal` (Boolean, default `false`: heal the full amount of damage dealt) | any |
| `crits` | Critical Hits | Shots can randomly deal critical damage. | none | 1.4+ |
| `superdrain` | Super Drain | Health and armor above the normal maximum drain back down to it. | none | 1.4+ |
| `armored` | Armored | Players spawn with 100 armor. | none | any |
| `superrail` | Super Rail | The rail driver pickup ignores weapons stay and respawns after 60 seconds. | none | any |
| `bigcraters` | Big Craters | Explosion geomod craters are twice as large. | none | any |
| `flamingenemies` | Flaming Enemies | Sustained flamethrower damage sets players on fire. | none | 1.4+ |
| `gibbing` | Gibbing | Turns on `gibbing`. | `scrapyard` (Boolean, default `false`: gibs are random debris instead of meat) | any |
| `jetpacks` | Jetpacks | Every player wears a jetpack and thrusts by holding jump while falling. | `explode` (Boolean, default `false`: a direct shot in the back or burning to death gibs the wearer) | 1.4+ |
| `humansvsbots` | Humans vs. Bots | Bots always play on Blue and humans on Red. Team game types only. | none | any |
| `delayedsupers` | Delayed Supers | Adds Multi Super Health, Multi Super Armor, Multi Invulnerability, Multi Damage Amplifier and shoulder cannon (plus rail gun when `superrail` is active) to `delayed_items`. | none | any |
| `weirdgungame` | Weird Gun Game | Replaces Gun Game's built-in progression and final weapon with unusual weapons. GG only. | none | any |
| `lowgravity` | Low Gravity | Reduced gravity. | none | 1.4+ |
| `skiing` | Skiing | Hold crouch to slide without ground friction. | none | 1.4+ |
| `dodging` | Dodging | Double-tap or crouch-jump to dodge in a direction. | none | 1.4+ |
| `pogo` | Pogo | Hold jump to jump again on every landing. | none | 1.4+ |
| `scorelimit` | Score Limit Override | Sets the score limit of the scope's game type (in WO, the round wins needed). Not available in REV, RUN and ESC. | `score_limit` (Integer, minimum 1, then clamped to that game type's range) | any |
| `idealplayers` | Ideal Player Count Override | Sets `ideal_player_count`. Not available in RUN. | `ideal_players` (Integer, 1-32) | any |
| `joinbots` | Join Bots | Server bots join for the level, or fill a match's empty slots. Not available in RUN. | `bots` (Integer, 1-8, default `2`), `skill` (`beginner`, `easy`, `average`, `hard`, `expert` or `nightmare`; default `average`), `personality` (`balanced`, `aggressive`, `defensive`, `tactician`, `berserker`, `objective`, `sniper`, `guerrilla`, `collector`, `brawler`, `destroyer` or `random`; default `balanced`) | any |

## Game types

`game_type` takes any identifier in the first column (case-insensitive). Every game type spawns players with the Riot Stick plus its spawn weapon. The "Score limit key" column names the rule key that ends the level.

| Identifier | Game type | Score limit key | Defaults that differ from the rule defaults above |
| --- | --- | --- | --- |
| `DM` | Deathmatch: frag everyone, free for all | `individual_kill_limit` | Spawn weapon 12mm handgun. |
| `TDM`, `TEAMDM` | Team Deathmatch | `team_kill_limit` | Spawn weapon 12mm handgun. |
| `CTF` | Capture the Flag | `cap_limit` | Spawn weapon 12mm handgun. |
| `KOTH` | King of the Hill: hold the hill to score | `koth_score_limit` | Spawn weapon Machine Pistol, plus Remote Charge (3). 2 second spawn delay. Location pinging. |
| `DC` | Damage Control: hold control points to score | `dc_score_limit` | Spawn weapon 12mm handgun. 2 second spawn delay. Location pinging. |
| `REV` | Revolt: Red captures control points in order, Blue defends | none | Spawn weapon Machine Pistol, plus Remote Charge (3). 2 second spawn delay. Location pinging. |
| `RUN` | Run: finish the course as fast as you can | none | Spawn weapon 12mm handgun. No PvP damage, no player collision, position saving, location pinging. |
| `ESC` | Escalation: capture control points toward the enemy base | none | Spawn weapon Machine Pistol, plus Remote Charge (3). 2 second spawn delay. Location pinging. |
| `BAG`, `BM`, `BAGMAN` | Bagman: steal and hold the bag to score | `bag_score_limit` | Spawn weapon 12mm handgun. 2 second spawn delay. |
| `TBAG`, `TBM` | Team Bagman | `tbag_score_limit` | Spawn weapon 12mm handgun. 2 second spawn delay. Location pinging. |
| `PIT` | Pit: duels, the winner plays the next player in the queue | `pit_score_limit` | Spawn weapon Assault Rifle (999 reserve). 200 health and armor, infinite reloads, no weapon or amp drops, no spawn delay. Rounds: 90 seconds, 3 second post-round, 3 second intermission. |
| `WO`, `WIPEOUT` | Wipeout: frag all enemy players before they respawn | `max_rounds` (round wins) | Fixed loadout: 12mm handgun, Assault Rifle, Sniper Rifle, Rocket Launcher, Shotgun (999 reserve each), spawn weapon Assault Rifle. 200 health and armor, infinite reloads, no weapon or amp drops, location pinging. 5 second spawn delay, multiplied by the player's deaths this round. Best of 7 untimed rounds, 3 second post-round, 5 second intermission. |
| `GG`, `GUNGAME` | Gun Game: frags advance you to a new weapon | `gg_score_limit` | 1 second spawn delay. No weapon or amp drops. +50 effective health per kill. |
| `SAL`, `SALVAGE` | Salvage: steal the flag and take it to your base | `sal_cap_limit` | Spawn weapon 12mm handgun. 1 second spawn delay. Location pinging. |
| `SCAV`, `SCAVENGER` | Scavenger: find the cartridge and take it to the target | `scav_cap_limit` | Same fixed loadout, health, armor, reload and drop settings as WO. 2 second spawn delay. Location pinging. |

Team game types are TDM, CTF, KOTH, DC, REV, ESC, TBAG, WO, SAL and SCAV.

## Bot profile files

Each file listed in `bot_profiles` is a separate TOML file describing one bot.

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `player_name` | String | random | Bot name, truncated to 31 characters. |
| `mp_character` | String | random | Multiplayer character name. |
| `personality_preset` | String | `"balanced"` | `balanced`, `aggressive`, `defensive`, `tactician`, `berserker`, `objective`, `sniper`, `guerrilla`, `collector`, `brawler`, `destroyer` or `random`. An unknown name falls back to `balanced`. |
| `skill_preset` | String | `"average"` | `beginner`, `easy`, `average`, `hard`, `expert`, `nightmare` or `random`. An unknown name falls back to `average`. |
| `personality_overrides` | Table | none | Numeric overrides of personality fields, keyed by field name. Unknown field names print a warning. |
| `skill_overrides` | Table | none | Numeric overrides of skill fields, keyed by field name. Unknown field names print a warning. |
| `quirks` | Table | none | Boolean switches: `railgun_no_los_hunter`, `melee_finisher`, `shotgun_low_health_finisher`, `jump_pad_pathing_affinity`, `spawn_hunter`, `crater_unlock_affinity`, `super_item_hoarder`, `sneaky_capper`, `never_drives`, `turret_camper`. Including this table replaces the preset's quirks with exactly the ones set to `true` here. |

## Example

```toml
# ---- Top-level server settings (must come before the first [table]) ----
server_name = "Alpine Faction Example"
max_players = 16
password = ""
rcon_password = "changeme"     # also creates a limited "legacy" rcon profile
dynamic_rotation = false
demo_auto_record = true

[[rcon_profiles]]
name = "moderator"
password = "modpass"
allowed_commands = ["kick", "ban", "map_next", "map_rest"]

[inactivity]
kick_after_warning = true      # enabled defaults to true

[vote_level]
enabled = true
allowed_levels = ["dm02", "ctf03"]

[vote_next]
enabled = true

# ---- Base rules: the default for every level ----
[base]
game_type = "TDM"
time_limit = 900               # seconds
team_kill_limit = 150
balance_teams = true
weapons_stay = true

[base.spawn_protection]
enabled = true
duration = 1.5

[[base.spawn_loadout]]         # added to the game type's loadout
weapon_name = "Shotgun"
ammo = 24

[[base.mutators]]              # keys below this header belong to the mutator
name = "vampire"
full_lifesteal = false

# ---- Rotation, played in this order ----
[[levels]]
filename = "dm02"              # plays TDM with every base rule

[[levels]]
filename = "ctf03.rfl"
[levels.rules]
game_type = "CTF"              # CTF defaults, then the base rules, then these
cap_limit = 5
time_limit = 1200
[levels.rules.overtime]
enabled = true                 # overtime tables do not inherit: always set enabled
time = 5

[[levels]]
filename = "dm07"
[levels.rules]
time_limit = 600
[[levels.rules.mutators]]
name = "vampire"
enabled = false                # switch off the base mutator for this level
[[levels.rules.mutators]]
name = "instagib"
```
