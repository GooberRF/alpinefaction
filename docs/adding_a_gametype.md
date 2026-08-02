Adding a multiplayer game type
==============================

This is an ordered checklist of every place a new multiplayer game type has to be
registered in Alpine Faction. It is written for a contributor who has not read the
game type code before.

The worked example throughout is **Salvage (`SAL`)**, added in AF 1.4. Salvage is a
1-flag CTF variant: one neutral flag spawns at the computed center of a `ctf` level
and either team carries it to their own base. Its own logic lives in
`game_patch/multi/salvage.{h,cpp}`; everything else on this list is a registration
touch point shared by all game types.

Line numbers are omitted deliberately — always locate code by symbol name. All paths
are relative to the repository root.

Before you start
----------------

Decide these up front, because several steps depend on them:

- **Acronym and full name** (`SAL` / `Salvage`).
- **Team or free-for-all.** Team mode is a single switch case that pulls in a large
  amount of engine behaviour for free (see step 4).
- **Level-name prefix**, or "plays on any MP level" (see step 6).
- **Win condition** — which engine or Alpine limit ends the round.
- **Round based or not** (`gt_type_uses_rounds`). Round-based types delegate
  round/win detection to `game_patch/multi/rounds.cpp`.
- **Whether score is frags or something else** (`gt_uses_custom_scoring`).

1. Identity enums (do both together)
------------------------------------

`rf::NetGameType` and `RF_GameType` are matched **by numeric ordinal**. Adding one
without the other makes clients reject the join, or worse, silently mismatch.

- `game_patch/rf/multi.h`, `enum NetGameType`: add your entry immediately before the
  `NG_TYPE_UNK` sentinel.

      NG_TYPE_SAL = 13,   // Salvage, as of AF v1.4

- `common/include/common/rfproto.h`, `enum RF_GameType`: add the matching wire value
  immediately before `RF_GT_UNK`.

      RF_GT_SAL = 0x0D,

Both sentinels (`NG_TYPE_UNK`, `RF_GT_UNK`) must stay last. `RF_GT_UNK` is the
client-side join rejection threshold (`game_patch/multi/network.cpp`), so a game type
at or above it cannot be joined.

2. Server-browser name table
----------------------------

`game_patch/multi/gametype.cpp`:

- Add a `static char <gt>_name[] = "SAL"; static char* <gt>_slot = <gt>_name;` pair
  next to the existing ones, above the `unk_name` sentinel.
- Register it in `populate_gametype_table()`:
  `g_af_gametype_names[rf::NG_TYPE_SAL] = &sal_slot;`

`g_af_gametype_names` is patched into five engine call sites by `gametype_do_patch()`
and is what the server browser and the listen-server "Create Game" dropdown read.
The dropdown's end pointer stops at `NG_TYPE_UNK`, so a new entry becomes
host-selectable automatically.

3. Predicate helper
-------------------

Add `bool gt_is_salvage();` to `game_patch/multi/gametype.h` and the one-line
definition to `gametype.cpp`, beside the other `gt_is_*` helpers. Everything else in
the codebase tests the game type through these helpers rather than comparing enums
inline.

4. Team-type switch
-------------------

`multi_game_type_is_team_type()` in `game_patch/multi/gametype.cpp`. Adding a single
`case` here enables, for free:

- team spawn point selection and `pick_weaker_team()` / auto team balance,
- team chat and the `[Team]` chat prefix,
- team-coloured player models, fpgun arms, and target name colours,
- the split (red/blue) scoreboard and the team-scores HUD block,
- the stock `team_scores` packet path,
- team damage gating.

If your type is FFA, skip this step entirely.

5. Display names
----------------

`game_patch/multi/multi.cpp`, four parallel if-chains:

| Function | Salvage returns |
|---|---|
| `multi_game_type_name` | `"Salvage"` |
| `multi_game_type_name_upper` | `"SALVAGE"` |
| `multi_game_type_name_short` | `"SAL"` |
| `multi_game_type_prefix` | `"ctf"` |

Also `gametype_short_name()` in `game_patch/misc/vote_panel.cpp` — the vote panel's
game type selector needs its own short tag because it must tolerate ids a client
build does not know.

6. Level matching
-----------------

Three places decide which levels your type may be played on. They must agree.

- `multi_game_type_uses_any_level()` (`game_patch/multi/multi.cpp`) — add your type
  **only** if it has no prefix of its own and should accept every standard MP level
  (BAG/TBAG/PIT/WO/GG do). Salvage has a real prefix, so it is **not** listed here.
- `multi_is_level_matching_game_type_hook` (`game_patch/multi/server.cpp`) — drives
  the listen-server level list. Salvage shares CTF's branch (`ctf` and `pctf`).
- `does_level_match_gametype_prefix()` (`game_patch/multi/votes.cpp`) — drives vote
  validation and the votable-level blob. Salvage shares CTF's `pctf` special case.

7. Config name resolution
-------------------------

`resolve_gametype_from_name()` in `game_patch/multi/server.cpp` maps the dedicated
server config's `game_type` string and the `gt` console command onto the enum. Accept
the acronym plus any spelled-out aliases (`"sal"`, `"salvage"`).

8. Help text
------------

`multi_gametype_help_text()` in `game_patch/multi/gametype.cpp`. One line describing
the objective; shown in server info UI.

9. Vote-system bounds (easy to miss)
------------------------------------

`game_patch/multi/votes.cpp` has **three** places that iterate or bound the game type
enum. All three must be bumped to the new highest real type, or the new type is
invisible to (and rejected by) the vote system:

- `build_level_valid_gametype_mask()` — loop bound.
- the vote-options blob builder — `gametype_count`.
- `resolve_vote_gametype()` — the incoming wire-value rejection bound.

10. Win-condition triple (easy to miss)
---------------------------------------

Three switches must agree on which limit your type advertises to clients, or the
client will display or enforce the wrong limit after a game type change:

- `game_patch/multi/alpine_packets.cpp`, `af_server_info` build — which value goes
  into `pkt.win_condition`.
- `game_patch/multi/alpine_packets.cpp`, `af_process_server_info_packet` — where the
  client stores it when the game type is already current.
- `game_patch/misc/misc.cpp`, the deferred `g_local_pending_win_condition` apply —
  where the client stores it after a delayed game type swap.

Salvage advertises captures, so it joins CTF's case in all three
(`rf::netgame.max_captures`).

11. Server config
-----------------

- Add a nested config struct in `game_patch/multi/server_internal.h` with **clamped
  setters** (see `BagmanConfig` / `SalvageConfig`), and a member on
  `AlpineServerConfigRules`.
- Parse the TOML keys in `parse_server_rules()` (`game_patch/multi/dedi_cfg.cpp`),
  beside the other per-gametype limits.
- Print them in `print_rules()` in the same file (reached from
  `print_alpine_dedicated_server_config_info()`). Each line is emitted only when it
  differs from the base rules.
- Add a `case` to `apply_defaults_for_game_type()` if your type needs different
  spawn/loadout/round defaults than the `default:` branch (which is what DM, TDM and
  CTF use).
- Map your score limit onto the engine's netgame limit in
  `apply_alpine_dedicated_server_rules()`. Salvage sets `netgame.max_captures` from
  `salvage.cap_limit`, mirroring CTF; GG sets `netgame.max_kills`.

Salvage's keys: `sal_cap_limit` (5), `sal_flag_spawn_delay` (30s),
`sal_flag_capture_respawn_delay` (5s), `sal_flag_return_time` (30s).

12. Scores wiring
-----------------

If your type keeps team scores of its own, provide
`<gt>_get_red_team_score()` / `_get_blue_team_score()` and setters (setters must be a
no-op on the server — it owns the truth), then add branches to:

- `send_team_score_patch` and `process_team_score_patch`
  (`game_patch/multi/gametype.cpp`) — the stock `team_scores` packet.
- `pick_weaker_team()` (`game_patch/multi/server.cpp`) — auto balance tie-break.
- `round_is_tied()` (`game_patch/multi/server.cpp`) — overtime detection.
- the limbo winner switch in `multi.cpp` — decides the end-of-level win/lose jingle.

13. Round end condition
-----------------------

`multi_check_for_round_end_hook()` in `game_patch/multi/server.cpp`. Add a `case`
that sets `round_over` when your limit is reached. Round-based types
(`gt_type_uses_rounds()`) return early from this hook and are driven by
`rounds.cpp` instead.

14. Level init and per-frame hooks
----------------------------------

- `multi_level_init_gametypes_injection` (`game_patch/multi/gametype.cpp`, @
  `0x0046E466`) — runs on client **and** server, before the engine's own multi level
  init. Reset your state struct and resolve item type indices here.
- `multi_level_init_post_gametypes()` (same file, called from `level_init_post_hook`
  in `game_patch/main/main.cpp`) — runs after the level's objects exist. Server-side
  world setup (finding items, spawning objectives) belongs here. Add your call
  **before** `rounds_level_init_post()`.
- `server_do_frame()` (`game_patch/multi/server.cpp`) — your server tick.
- `player_destroy_hook` (`game_patch/misc/player.cpp`) — disconnect cleanup. On
  clients this must at minimum null any `rf::Player*` you cache, because the engine
  is about to free the struct.
- the entity-death hook in `game_patch/multi/server.cpp` (beside
  `bagman_on_entity_will_die`) if death has to release an objective.

Note the ordering trap Salvage hit: the injection at `0x0046E466` runs *before* the
`CALL` it is attached to, and that call is `multi_ctf_level_init` — which clears
`IIF_SPINS_IN_MULTI` on the `flag_red`/`flag_blue` item classes for **every** game
type. Anything a level-init function does to that flag is undone immediately.
Salvage therefore re-applies it from `salvage_apply_flag_spin_flag()`, called at the
end of `multi_ctf_level_init_hook` in `multi.cpp`.

15. Custom state packet
-----------------------

If the client needs state the stock packets do not carry:

- Add an `af_packet_type` value in `game_patch/multi/alpine_packets.h` (next free
  id; Salvage took `0x63`).
- Add the packed wire struct inside the `#pragma pack(push, 1)` region of that
  header, and declare `af_send_*`, `af_send_*_to_all` and `af_process_*`.
- Implement them in `alpine_packets.cpp` following `af_bagman_state_packet`: a
  single `build_*` helper shared by both send paths, and a process function that
  validates header size, declared payload size and buffer length before use.
- Add the dispatch `case` in `af_process_packet`.
- **Whitelist the id in `game_patch/multi/network.cpp` (easy to miss)** — that file
  keeps a *second*, local copy of the packet-type enum, unrelated to the one in
  `alpine_packets.h`, plus two arrays: `g_server_side_packet_whitelist` (client →
  server) and `g_client_side_packet_whitelist` (server → client). Add the enumerator
  to the local enum, then add it to the **one** array matching your direction — a
  server → client state packet goes in the client-side list only, exactly like
  `af_bagman_state`. `packet_check_whitelist()` runs before `af_process_packet`
  dispatches, so an id missing from the receiving side's array is dropped with
  nothing but `Ignoring packet 0x63` in the console; the dispatch `case` you just
  wrote is never reached and every feature fed by the packet silently does nothing.
- **Late joiners**: call your force-sync from `send_team_score_state_info_patch`
  (`game_patch/multi/gametype.cpp`), beside the bagman and Pit sync calls. Without
  this a mid-game joiner sees a blank objective state until the next broadcast.
- Broadcast on every state transition, plus a periodic refresh if the client renders
  a countdown from the packet (Salvage re-broadcasts every second while a timer is
  running so client-side drift is corrected).

16. HUD
-------

- **Scoreboard** (`game_patch/hud/multi_scoreboard.cpp`): the team header block in
  `draw_scoreboard_header`, and any extra column in `draw_scoreboard_players` (the
  column width, the header string and the per-row value are three separate gates —
  Salvage routes all three through one `show_caps` bool). Per-player status icons are
  chosen in the `status_bm` selector.
- **Team-scores HUD** (`game_patch/hud/multi_hud.cpp`): add your type to the gate in
  `multi_hud_render_team_scores_new_gamemodes_patch` — **required**, or the score box
  never renders at all — and then to the score selection inside
  `multi_hud_render_team_scores()`. The box size branch at the top of that function
  decides the layout; falling into the final `else` gives the CTF/TBAG-sized box.
- **Spectate layout** (`game_patch/hud/multi_spectate.cpp`,
  `spectate_bottom_left_hud_top`): add a `case` returning the same height your box
  uses, so spectate hints do not overlap it.
- **World HUD** (`game_patch/hud/hud_world.cpp`): add a `build_*_icons()` function
  and call it from `hud_world_do_frame()`, gated on `rf::is_multi` and your
  predicate. Reuse `do_render_world_hud_sprite` and `render_string_3d_pos_new`;
  respect the relevant `g_alpine_game_config.world_hud_*` toggle.
- **Carrier attachment**: the cosmetic mesh worn on a carrier's back is rendered by
  the single `carrier_attachment_render_patch` CodeInjection at `0x00421C0B` in
  `gametype.cpp`. Two CodeInjections cannot share an address, so extend that one
  rather than adding another.

17. Version gating
------------------

`server_features_require_alpine_client()` in `game_patch/multi/server.cpp` already
forces AF 1.4+ and hard-rejects legacy clients for any game type
`>= NG_TYPE_BAG`, so a new 1.4-era type needs no change. A type added in a later
release needs a new block bumping `min_minor_version`.

18. Build and changelog
-----------------------

- Add your `.cpp`/`.h` to the source list in `game_patch/CMakeLists.txt`.
- Add the game type to the "Add new multiplayer game types" list in
  `docs/CHANGELOG.md`, and a line for any new dedicated server config fields.

Verification pass
-----------------

After it compiles, re-read each switch you extended and confirm no case fell
through. The four that have historically been missed:

1. the three vote bounds (step 9),
2. the win-condition triple (step 10),
3. the `new_gamemodes` HUD gate (step 16) — the symptom is a completely missing
   score box, which is easy to mistake for a scoring bug.
4. the `network.cpp` packet enum + whitelist (step 15) — the symptom is a client
   whose objective state never changes: no glow, no carrier attachment, no world
   HUD countdown, while anything the client derives from level items on its own
   still renders, which is easy to mistake for a rendering bug. Check the console
   for `Ignoring packet 0x<your id>`.

There are no automated tests for game types; validate in game with a listen server
and a dedicated server, including a mid-game joiner.
