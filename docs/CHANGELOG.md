⚙️⛏ Alpine Faction Changelog ⛏⚙️
===================================

Version 1.2.0 (Willow): Not yet released
--------------------------------
### Major features
[@GooberRF](https://github.com/GooberRF)
- Implement TOML-based `ADS` configuration for dedicated servers
    - Per-level rules configuration for server hosts
    - On-demand loading of changed settings without server needing restart
    - Support changing server game type on-demand or per-level
- Add `King of the Hill` (KOTH) multiplayer game type
- Add `Damage Control` (DC) multiplayer game type
- Add `Revolt` (REV) multiplayer game type
- Add `Run` (RUN) multiplayer game type
- Add `Escalation` (ESC) multiplayer game type

### Minor features, changes, and enhancements
[@GooberRF](https://github.com/GooberRF)
- Raise maximum confgurable pixels/m apply map texture setting in level editor to 8192
- Default level fog color to flat black (0, 0, 0) instead of stock game default (40, 40, 40)
- Update Level Properties window strings to be more descriptive
- Add `cl_wh_altdmgindicators` command to toggle cumulative world HUD damage indicators
- Add `ui_realarmor` command to toggle real armor display on HUD (1:1 with health instead of the 2:1 default)
- Add `-afs FILENAME` command line argument to specify a settings file when launching
- Add `Suppress autoswitch` control bind (if held while picking up a weapon, autoswitch is suppressed)
- Add `Flag Captures While Stolen` dedicated server config option
- Add damage reduction handicap feature
- Add `mp_handicap` command to configure your damage reduction handicap (0 - 99%)
- Add `cl_autoswitchfirewait` command to suppress autoswitch during and immediately after weapon fire
- Maintain third person camera view between level loads after `camera3` command entered
- Add `cl_autoswitchempty` command to always autoswitch empty weapons even if autoswitch is turned off
- Bump RFL version to 302
- Add support for custom AlpineLevelProperties chunk in rfl files
- Add `legacy_cyclic_timers` setting to level properties window in level editor
- Add "ignore nonvoters" option to dedicated server vote configs
- Reformatted output of `info` command to contain more useful info
- Deprecate `sv_connectedclients` command as all info is now in the output of `info`
- Updated new player joining message printed to ADS server console to include socket, client info, and max RFL version
- Support weapon stay exemption functionality for any weapons, including stock fusion behaviour and weapons from TC mods
- Deprecate `$Max FOV` dedicated server setting
- Support GunGame configuration using weapon names (from `weapons.tbl`) rather than weapon indices
- Add support for custom weapon loadouts in dedicated servers
- Add support for spawn delays in dedicated servers
- Identify Alpine Faction and Dash Faction players in server console on join and when `info` command is run
- Log player join request rejection reason to server console on attempted join
- Log accepted and rejected `rcon` commands to server console
- Log accepted and rejected `rcon_request` commands to server console
- Log all chat messages to server console
- Add support for control points as game type objectives
- Report custom gamemodes to legacy clients as TDM for compatibility
- Add `Control_Point_Handler` event
- Add `cl_outlineheightscale` command to scale height of control point outlines
- Add `cl_outlinesegments` command to set segment count for control point outline rings
- Add game type selection for `Gametype_Gate` event for KOTH, DC, REV, RUN, ESC
- Print vote initiation alerts to dedicated server console
- Add `reject_incompatible_clients` ADS setting
- Add `r_colorblind` command for configuring colorblind mode rendering filter (Direct3D 11 renderer only)
- Add `r_picmip` command to configure texture resolution divisor (Direct3D 11 renderer only)
- Identify known server browser clients in server console on join and when `info` command is run
- Log a warning to console when trying to load a mesh file that doesn't exist
- Add `-min` command line argument to launch ADS servers with minimal console logging
- Add `-log` command line argument to log full console output from ADS servers
- Add `cl_columnsegments` command to set segment count for control point light columns
- Add `cl_columnheightscale` command to scale height of control point light columns
- Support `levelm` and `Play (Multi)` in RED launching KOTH maps with KOTH gamemode
- Set default gamemode settings when starting a listen server
- Add `sp_exposuredamage` command to control exposure damage when outside without armor
- Add `sv_gametype` command to override server configured game type
- Update `info` command output to include current game type
- Add game type to `Level initializing` console message on level load
- Communicate changed server settings to supported clients
- Support game type switching on client without having to rejoin
- Kick legacy clients on game type switch and notify them they can rejoin
- Add `vote gametype` chat command
- Support mappers associating MP respawn points with control points
- Add `Respawn_Point_State` event
- Add `Modify_Respawn_Point` event
- Add new team scores HUD widget and capture point progress bars for KOTH and DC game types
- Simplify and optimize level info (`mapname_info.tbl`) handling
- Support locked capture points
- Support stage param on `Capture_Point_State` event
- Add `Set_Capture_Point_Owner` event
- Add `When_Captured` event
- Add `Owner_Gate` event
- Allow using `Edit Links` window in level editor to link objects to MP respawn points
- Support rule presets and aliases in ADS dedicated servers
- Add preset support to `vote level` and `vote match` chat commands
- Expand `sv_gametype` to allow setting a new level
- Add `gt` alias to `sv_gametype` command
- Allow `sv_gametype` execution via rcon
- Add `dbg_numgeos` debug command to count number of geo craters in current level
- Add legacy mover setting to level properties window in level editor
- Add `Inspect weapon` control bind
- Add `cl_locationpings` command to toggle location pinging functionality
- Fit scoreboard on screen by splitting into columns when server has > 16 players
- Deprecate `enforce_min_server_version` Alpine restrict setting
- Add `sv_restrict_status` command to check status of client restrictions on the current map and ruleset
- Add tracking of `Loads` to scoreboard in RUN game type
- Add `ui_runtimer` command to toggle display of timer in RUN game type
- Allow `K` link hotkey in level editor to create multiple links at once
- Dynamically scale `Spectating:` name plate based on player name length
- Color player name on name plate based on their team in team game types
- Exclude clients from votes if they don't meet the server's minimum requirements
- Add winter seasonal effect to main menu background
- Suppress missing asset warnings for files known to be referenced by but missing from the stock game
- Add `Play in multi (camera)` button to level editor
- Add `Player starts with headlamp` setting to level properties window in level editor
- Add `Set_Gameplay_Rule` event and first supported rule `Player has headlamp`
- Rearrange quick access buttons on level editor toolbar
- Add level editor hotkey `F9` to play in multi, and `F10` to play in multi (camera))
- Deprecate `level_info_ext` and `map_info_ext`, use `level_info` and `map_info` instead
- Add `Override static mesh ambient light scale` setting to level properties window in level editor
- Update output of `level_info` command to include advanced level properties
- Add browser, spectator, and idle icon assets
- Support setting freelook spectator camera accel with scroll wheel
- Alias `Ping Location` control to reset to freelook spectator camera accel to base value
- Add scroll wheel support to message log panel and scrollable item lists in menus
- Add `kick after warning` option to dedicated server client inactivity tracking

[@is-this-c](https://github.com/is-this-c)
- Add `Tlv{Writer, Reader}` and `HighResTimer`
- Prevent idle players from voting
- Add colored `bot` text to a bot's name
- Add `bot`, `browser`, `spectator`, and `idle` text to `info` command's output
- Add `bot_shared_secret` command to signal your client as a bot
- Add a `bot_shared_secret` ADS option for dedicated servers
- Send `pf_player_stats_packet` with score
- Add browser, spectator, and idle icon logic
- Ignore browsers in spectate mode
- Add `bighud` support for chat menus
- Support `•` in TrueType fonts
- Add the year and the number of spawned players to the scoreboard
- Add `ui_simple_server_chat_messages` command to display automated server chat messages without `Server: `
- Color automated server chat messages with a gold name instead of a red name
- Add `key_quick_exit` command to restore keyboard shortcut `Shift+Esc` to quit out of Red Faction
- Support Alt+F4 or window close button (X) to quit out of Red Faction
- Add a configurable control to display a remote server's config
- Add `ui_always_show_spectators` command to always display spectators in multiplayer
- Improve code that fixed broken events if their delay parameter was set

[@nickalreadyinuse](https://github.com/nickalreadyinuse)
- Improve dedicated server framerate consistency

### Bug fixes
[@GooberRF](https://github.com/GooberRF)
- Fix very niche case where armor values were displayed differently when Big HUD was enabled
- Fix Direct3D 11 renderer not properly rendering detail brushes in skyboxes
- Fix super powerups applying in SP in non-Alpine levels
- Fix `Cyclic_Timer` event activating twice on initial activation (subject to legacy switch)
- Fix broken rendering of particle emitter 23616 on community level `dm-birthday.rfl`
- Fix broken rendering of particle emitter 14313 on community level `DM-RFU2-Halloween.rfl`
- Fix `NAME has been kicked` not being printed to server console when players kicked due to inactivity
- Fix `Advertise AF` sometimes sending the wrong advert to players running old Alpine versions
- Fix inactivity incorrectly being tracked when endgame scoreboard is displayed
- Fix very niche case where server could crash if gungame is on and a weapon type is invalid
- Fix overtime initiating in match mode even if no match is active
- Fix scanner and scope sensitivity set in options menu not applying until new level is loaded
- Fix Direct3D 11 renderer crashing when generating realtime bitmaps
- Fix dynamic lights not rendering on meshes (fixed in Direct3D 11 only)
- Fix dynamic lights not rendering on mover brushes (fixed in Direct3D 11 only)
- Fix Direct3D 11 renderer not properly rendering vertex lighting for static meshes
- Fix `r_fullbright`, `r_lightmaps`, and `r_nearest` not working properly on Direct3D 11 renderer
- Fix `Winner` and `Game Over` sounds played at the end of a match usually being wrong
- Fix `Try Avoid Players` behaviour for spawn point selection not working properly
- Fix rare crash when rapidly switching between players in first person spectate mode
- Fix server processing `af_ping_location_req` packets when feature was turned off
- Fix `Solo ignore resets` trigger option not working properly
- Fix `No match is queued` message mistakenly being sent to all players instead of just the requester
- Fix match mode logic error if map is changed during pre-match phase
- Fix skybox rendering issues with Direct3D 11 renderer on community level `dm-rfu-friday.rfl`
- Fix MP timer alert sounds not playing if already played once this level (e.g. if level is extended)
- Fix rare crash on game shutdown when certain mods are loaded
- Fix low velocity translating movers not working as expected (subject to legacy switch)
- Fix accelerating/decelerating rotating movers not working as expected (subject to legacy switch)
- Fix `Ping Pong Infinite`, `Loop Once`, and `Loop Infinite` modes for rotating movers (subject to legacy switch)
- Fix `Lift` mode not working for translating movers (subject to legacy switch)
- Fix strange behaviour of translating movers when accel + decel > total travel time (subject to legacy switch)
- Fix crash when `Force Orient` flag is used with translating movers
- Fix `Force Orient` mover flag not working (subject to legacy switch)
- Fix rare crash when starting to spectate a player immediately when they land on a surface

[@is-this-c](https://github.com/is-this-c)
- For console arguments, parse hexadecimal values as unsigned integers
- Set `rf::gr::text_2d_mode` to ignore fog
- Fix `gr_d3d_bitmap`, so `gr_d3d_set_state` is called earlier
- Support `PgUp`, `PgDown`, `End`, and `Home` on numeric keypads
- Fix a potential crash after a client quits a game if Directd3D 11 is enabled

### From [Dash Faction](https://github.com/rafalh/dashfaction)
[@rafalh](https://github.com/rafalh)
- Fix compatibility with misbehaving GPU drivers which report support for texture formats which they don't actually support (e.g. VirtualBox)
- Add `DashLevelProps::lightmaps_full_depth`

### Imported libraries
- [toml++](https://github.com/marzer/tomlplusplus) by Mark Gillard

Version 1.1.0 (Tamarack): Released Apr-19-2025
--------------------------------
### Major features
[@GooberRF](https://github.com/GooberRF)
- Add fully integrated achievements system via FFLink
- Add gaussian bullet spread method (bell-curve distribution weighted to center of reticle)
- Add command menu, spectate menu, and chat menus for radio messages and taunts
- Add location pinging in team modes
- Add visual hit notifications, enhanced hit sounds, and death sounds
- Add `Advanced` panel to in-game options menu for configuring AF settings
- Add health, armour, and ammo display to first person spectate
- Add "World HUD" sprite rendering for objective and player labels
- Implement saving/loading of settings using readable `alpine_settings.ini`, remove reliance on legacy encoded `players.cfg`
- Make servers report basic config information to tracker (visible on https://rfsb.factionfiles.com/)
- Resolve texture alignment problems affecting many older community levels

### Minor features, changes, and enhancements
[@GooberRF](https://github.com/GooberRF)
- Add `Anchor_Marker_Orient` event
- Add `Light_State` event
- Bump RFL version to 301
- Make endgame voting text prompt more appropriate
- Make `vote rand` the same as `vote next` if `$Dynamic Rotation` is on (avoids repeats)
- Add `cl_wh_overdraw` command for toggling objective world HUD sprites drawing through the world
- Add world HUD icons for CTF flags
- Add `cl_wh_ctf` command for toggling world HUD icons for CTF flags
- Add support for loading packfiles from `user_maps\projects` (intended for WIP projects)
- Make `Ready for match` action bind toggle ready status
- Deprecate `/unready` chat command
- Allow .bik video files to be loaded from `client_mods`
- Add handling for `Active Distance` setting in Particle Emitter properties (level designers can use to optimize performance)
- Add `sp_spreadmode` command to toggle gaussian bullet spread in single player
- Add `$Use Gaussian Bullet Spread` dedicated server config option to use gaussian bullet spread for damage calculation
- Make clientside visual representation of bullet impacts use gaussian spread in servers with it enabled
- Add AF flags, level filename, patch version, and build type to `game_info` packets
- Optimize spawn point selection methods and resolve niche logic issues
- Add world HUD icons for multiplayer respawn points
- Draw multiplayer respawn point world HUD icons in pre-match
- Add `dbg_wh_mpspawns` command to draw multiplayer respawn point world HUD icons (supported in SP and as listen server)
- Add compatibility table (lightmap clamp floor) for `dm-SpecialForces.rfl`
- Add action binds for `Radio message menu`, `Taunt menu`, and `Command menu`
- Add support for `$Chat Menu 1` (1 - 9) to `MAPNAME_info.tbl` (populates map chat menu on that map)
- Import settings from `players.cfg` on launch if `alpine_settings.ini` is not found
- Support TC mods with new settings file approach - `alpine_settings_MODNAME.ini`
- Prevent MP character from being permanently changed after spawning in a server with `$Force Player Character` configured
- Make multiplayer games started via `levelm` console command or editor `Play (multi)` button 60 minutes long
- Add `World_HUD_Sprite` event
- Add notification to old Alpine clients when `$Advertise Alpine Faction` is on to advise them of the update
- Add `dbg_mapver` command to check RFL versions
- Add `sv_connectedclients` command to help servers understand which clients can load which maps
- Add notification to players when the server is about to switch to a map with a version they can't load
- Add `mapm` command as an alias for `levelm`
- Add `+Require Official Build` and `+Enforce Server Version Minimum` settings to `$Clients Require Alpine Faction`
- Add framework for custom packets
- Add new action bind for location pinging
- Add `+Location Pinging` setting to `$Clients Require Alpine Faction`
- Add `cl_showping` command to display current ping when connected to a server
- Make cutscene skip prompt text more clear
- Deprecate `(AF)` prefix on Alpine-specific action names
- Change behaviour of `cl_screenshake` to only disable screenshake from weapon fire
- Add hit indicator numbers
- Add `cl_wh_bigtext` command to control the size of location ping and damage indicator text
- Add `cl_hitsounds` command to toggle local playing of Alpine Faction hit sounds
- Add `cl_wh_hitnumbers` command to toggle drawing of Alpine Faction hit number indicators
- Deprecate legacy hitsounds method in favour of new damage notification method using new packet
- Add compatibility for legacy clients (send normal sound packet for hitsounds)
- Add `$Damage Notifications` dedicated server config option and `+Legacy Client Compatibility` setting
- Deprecate `$Hitsounds` dedicated server config option
- Add custom sounds framework
- Add custom sounds for location ping and achievement unlock
- Add kill sounds
- Support built-in RF taunts in Alpine Faction taunt menu (always included in game files but unused)
- Add `mp_taunts` command to disable audio taunts
- Default gore level to 2 unless otherwise set in `alpine_settings.ini`
- Add `cl_wh_teamplayerlabels` command to toggle display of teammate player labels
- Add `spectate_playerlabels` command to toggle display of player labels in spectate mode
- Add `-skiplegacywarning` command line argument to skip warning prompt when opening a legacy level file
- Add `$Rail Driver Scanner Color` to `af_client.tbl`
- Add spectate mode options menu
- Add `dbg_berserk` command for testing berserk entity state
- Display control binds in spectate mode help test
- Remove unrar library (no longer needed)
- Add `cl_swapsgcontrols` command to swap shotgun controls
- Move settings to `alpine_settings.ini`:
  - Big HUD, linear pitch, FPGun FOV scale, swap controls for AR, SG, GN
  - Weapon shake, fullbright characters, lightmaps only
  - Scanner sensitivity modifier, scope sensitivity modifier, static scope sensitivity
  - World HUD objective icons, overdraw, big text, damage numbers, spectate labels, team labels
  - Hit sounds and audio taunts
  - Muzzle flash lights, unlimited semi auto, gaussian spread, ricochet
  - Damage screen flash, death bars, mesh static lighting, show glares, show enemy bullets
  - Show FPS, show ping, spectate mode minimal UI, nearest texture filtering
  - DirectInput, autosave, scoreboard animations, level sound modifier
  - High monitor resolution, Alpine branding, multiplayer tracker, fast animations
  - Max FPS, max server FPS, server netfps
  - Simulation distance, lod distance scale, disable MP character LOD
  - Join beep, lighting colour range
- Add `SkipCutsceneBindAlias` setting to `alpine_settings.ini`
- Deprecate `IgnoreSwapAssaultRifleControls` and `IgnoreSwapGrenadeControls` in `af_game.tbl` (no longer needed)
- Add `dbg_consolehistory` command to toggle whether console history persists between game launches
- Add `sp_camerashake` command to turn off all instances of camera shake
- Add `mp_playernames` command to toggle drawing of names of targeted players in multiplayer
- Add `mp_verbosetimer` command to toggle drawing of the "Time Left" label on the multiplayer timer
- Add `Multi Timer X Offset`, `Multi Timer Y Offset`, and `Multi Timer Color` to `af_client.tbl`
- Add `Default Third Person` to `af_game.tbl` to set default player camera to third person
- Made lava and acid damage result in a self kill in multiplayer rather than a mysterious kill
- Add radio message audio
- Allow `Anchor_Marker_Orient` event as skybox eye anchor
- Add support for `$Crater Texture PPM` to `MAPNAME_info.tbl`
- Add options to `af_ui.tbl` to set the Summoner Trailer button to load a specific level in TC mods
- Add `r_fastanims` console command to toggle fast animations
- Shift High Monitor Resolution config to MonitorResolutionScale, allowing for greater control
- Add `r_monitorscale` console command to set monitor resolution scale
- Add environmental sounds slider to audio options panel
- Add in-game panel to recommend FF account linking
- Add launcher notifications for fflink status - link success, link failure, validation failure
- Add `r_fullrangelighting` and `r_clampofficiallightmaps` commands
- Allow goal value output in hud messages via `$goal_GOALNAME$` magic word
- Add control magic word resolution in hud messages:
    - `Toggle headlamp` (`$HEADLAMP$`)
    - `Skip cutscene` (`$SKIP_CUTSCENE$`)
    - `Quick save` (`$QUICK_SAVE$`)
    - `Quick load` (`$QUICK_LOAD$`)
- Add static bomb code option (combination provided by `eckozero`)
- Add `sp_staticbombcode` console command to toggle static bomb code option
- Add `Set_Light_Color` event
- Add entity pain sounds option to in-game options panel and settings ini file
- Add `cl_painsounds` console command to toggle pain sounds
- Add `dbg_pow2tex` command for power of 2 texture enforcement debugging

[@natarii](https://github.com/natarii)
- Add new damage notification hit sound file
- Master radio message audio

### Bug fixes
[@GooberRF](https://github.com/GooberRF)
- Fix server log output for status of `$Enforce Semi Auto Fire Rate Limit` being incorrect
- Fix `AF_Teleport_Player` events sometimes not correctly translating player velocity
- Fix `r_fullbright` sometimes applying to the host in listen servers even when not turned on
- Fix character meshes not respecting level ambient light when `r_fullbright` is off
- Fix pre-match ready up prompt sometimes displaying even when ready
- Fix several niche issues where multiplayer spawn point selection would not work as intended
- Fix match ready up prompt not being properly reset for players who leave the server during pre-match
- Fix incorrect information in "You are no longer ready" message
- Fix players leaving server during pre-match not always being removed from list of ready players
- Fix players picking up items and activating triggers while teleporting
- Fix listen server launching whenever menu opens if game was launched with `-levelm` command line argument
- Fix clients not knowing correct ammo counts for other clients
- Fix `When_Picked_Up` events not activating for some item classes
- Fix bug where players would be disconnected from high framerate servers if they were standing in a solo trigger
- Fix listen server create panel not properly loading `Level Name` field for Alpine levels
- Fix geomod autotexture ppm not being correctly calculated for all crater textures
- Fix "Time Left" text label not being localized
- Fix `Player Entity Type`, `Player Undercover Suit Entity Type`, `Player Undercover Scientist Entity Type` not always applying
- Fix lava and acid damage sometimes being attributed to a random player in the game
- Fix level editor not handling new events with orient values from loaded groups (bump version field for rfg files)
- Fix `Play (multi)` button in level editor not launching levels with spaces in filename
- Fix crash when a `Music_Start` event is called with a sound file that has already been played (upstream issue)
- Fix magic word resolution in hud message output being wrong for unbound controls

Version 1.0.0 (Maple): Released Jan-25-2025
--------------------------------
[@GooberRF](https://github.com/GooberRF)
- Introduce Alpine levels, using new rfl file version (300 current):
	- Add many new events (full documentation at wiki link below)
	- Fix triggers not being able to activate events in MP
	- Fix delay param on some events breaking them
	- Add additional functionality to some existing stock game events
	- Remove lightmap clamping
	- Allow use of dynamic lights placed in levels
	- Limit on Multiplayer Respawn Point objects raised from 32 to 2048
	- Full documentation of Alpine level features is available here: https://www.redfactionwiki.com/wiki/Alpine_Level_Design

- Enhance level editor functionality:
	- Add button to playtest levels in multiplayer
	- Stop editor output window from turning red when map exceeds legacy RF geometry limits
	- Remove references to legacy geometry limits from build report in editor output window
	- Stop faces being added to "fix PS2 tiling" at build time in editor
	- Stop mover brushes being split to "fix PS2 tiling" in editor
	- Add stock event `Set_Liquid_Depth` to editor objects list
	- Disable legacy level editor lightmap clamping when loading level files
	- Add 3D arrows to level editor for stock events that save orientation
	- Remove `You must rebuild geometry before X` warnings in editor
	
- Expanded capabilities for mod developers:
	- Add new `client_mods` folder for loading clientside mods
	- Expose many previously hardcoded features for customization via new table files in mods
	- Add map-specific tables to configure settings for that map
	- Enable clientside mods to edit "safe" stock game tables (those that cannot be used to cheat)
	- Scale geomod crater textures based on texture dimensions
	- Full documentation of Alpine mod development features is available here: https://www.redfactionwiki.com/wiki/Alpine_Faction_Mod_Development

- New competitive play features (all configurable by server):
	- Full match mode ready up system for participants
	- Overtime mode when tied
	- Modern semi auto fire rate limiting system
	- Allow players to turn off camera shake, muzzle flash blinking, and level textures
	- Allow players to render all character meshes fullbright for visibility
	- Lock servers to Alpine clients only
		
- New configurable control binds:
	- Toggle player headlamp
	- Skip cutscenes
	- Vote yes/no on server votes in multiplayer
	- Ready for queued match
	- On-demand	self kill and respawn
	- Drop held flag

- New revamped Alpine Faction launcher featuring:
	- Overhauled layout, designed to optimize UX
	- Integration with `af://` protocol handler for directly installing files from FF
	- Integration with `af://` protocol handler for linking AF clients to FF accounts
	- Community News feed
	- Update notification system
	- On-UI links to custom content folders and community resources
	- Many additional configurable settings in options panel

- Many new console commands, and some renamed existing commands from upstream
	- Full list of console commands available in AF is here: https://www.redfactionwiki.com/wiki/Red_Faction_Console_Commands#Alpine_Faction
    - New commands:
        - `cl_gorelevel`
        - `cl_scannersens`
        - `cl_scopesens`
        - `cl_screenshake`
        - `cl_staticscopesens`
        - `cl_togglecrouch`
        - `dbg_goal`
        - `drop_clutter`
        - `drop_entity`
        - `drop_item`
        - `fflink_status`
        - `level_info_ext`
        - `levelm`
        - `mp_deathbars`
        - `mp_notifyonjoin`
        - `mp_ricochet`
        - `pcollide`
        - `r_fullbright`
        - `r_lightmaps`
        - `r_muzzleflash`
        - `r_scopefullrangelights`
        - `sp_autosave`
        - `sp_difficulty`
        - `sp_levelhardness`
        - `sp_unlimitedsemiauto`
        - `spectate_followkiller`
        - `sv_netfps`
        - `sv_pass`
        - `sv_rconpass`
        - `teleport`
	
- Revamped approach to dedicated server configuration, including many new config options and enhanced logging
	- Full documentation of dedicated server config options available in AF is here: https://www.redfactionwiki.com/wiki/Alpine_Faction_Dedicated_Server_Config
    - New dedicated server config options:
        - $Player Respawn Logic
        - $GunGame
        - $Critical Hits
        - $Overtime Enabled
        - $Weapon Stay Exemptions
        - $Desired Player Count
        - $Use SP Damage Calculation
        - $Dynamic Rotation
        - $Drop Amps On Death
        - $Allow Fullbright Meshes
        - $Allow Lightmaps Only Mode
        - $Allow Disable Screenshake
        - $Allow Disable Muzzle Flash Lights
        - $Allow Client Unlimited FPS
        - $Advertise Alpine Faction
        - $Clients Require Alpine Faction
        - $Kick Inactive Players
        - $Enforce Semi Auto Fire Rate Limit
        - $Item Respawn Time Override
	
- Miscellaneous changes and updates
	- Reimplement cut stock game gibbing feature for enemies
	- Add player headlamp (flashlight)
	- Allow picking up Super Health and Super Armor in single player
	- Spawn players at Player Start position in multiplayer if level has no respawn points
	- Improved handling for players leaving/joining games during votes
    - Handle tied votes after all players have voted
	- Add `/hasmap` chat command
	- Add handling for rfl extensions for `/vote level` chat command
	- Add "follow killer" option for first person spectate
	- Display current player count on scoreboard
	- Add missing help strings for builtin cheat commands
	- Alias `spectate` command to camera free look when issued in single player
	- Add `/vote rand` chat command
	- Fix multiple bugs that resulted in some fire requests being wrongfully dropped by servers
	- Lower minimum character count for console command and level filename tab complete to 1
	- Move placement of dedicated server config lines to above `$Map` lines
	- Add `/whosready` chat command (for match mode)
	- Set FPS maximum limit to 10000 for single player and menus, maintain 240 limit for multiplayer due to impactful bugs
	- Add link drawing for `debug event` and `debug trigger`
	- Add support for endgame voting through FFLink
	- Add `Ignore Resets` option to triggers to replicate legacy functionality (default is off)
    - Add first launch information window
    - UX-focused updates to installer application
    - Add HUD notifications for voting and match mode ready up system

[@natarii](https://github.com/natarii)
- Fix music desyncing when entering menus in multiplayer

[@HeyItsDuke](https://github.com/HeyItsDuke)
- Add Big HUD version of rocket lock on reticle




Dash Faction (Upstream) Changelog
=================================

NOTE: Alpine Faction was forked from the Dash Faction main branch in late Jan 2025 and has merged only a subset of commits made since then.

Version 1.9.0 (released 2025-04-06)
--------------------------------
[@rafalh](https://github.com/rafalh)
- Added experimental D3D11 renderer
- Added option to switch between D3D8, D3D9 and D3D11 renderer
- Fix possible crash when alt-tabbing in fullscreen mode before game is fully initialized
- Fix writing corrupted save files when autosaving some levels
- Add `gamma` command
- Fix holes in driller cockpit on a widescreen display
- Change default 'Use' key to E
- Fix "Play (camera)" editor button for level filenames with spaces
- Fix a patch for a possible buffer overflow in "Play" editor button caused by a long filepath
- Fix cull radius for particle emitters with growing particles
- Skip textures with too long names (32+ characters) in editor to avoid buffer overflow
- Speed up shadows rendering in levels with multiple detailed movers
- Add `debug particle_emitter` subcommand (shows emitter cull radius)
- Fix crash reporter URL
- Fix buffer-overflow when importing mesh with more than 8000 faces in the editor
- Fix various issues when server switches to a new level before player finishes downloading the previous one
- Adjust letterbox effects in cutscenes and after death for wide screens
- Add support for bik files (Bink videos) in mods
- Hide player riot shield third person model in cutscenes
- Fix player third person weapon model animations after loading the game from a save file
- Change default volume to 0.5
- Log a warning when sound file cannot be find
- Add Ogg Vorbis audio format support
- Shorten the wait between levels when the server is empty
- Support CIDR IP ranges in banlist (should help with banning VPNs)
- Fix alpha sorting regression in level L5S3 (and possibly others)
- Add more info to `level_info` command and add `map_info` alias
- Fix ambient sound panning when EAX is disabled
- Fix quiet 3D sounds being often skipped when EAX is enabled, e.g. first miner in L1S1 (DF bug)
- Allow saving in training levels
- Fix final level time left count-down in multi using wrong font (DF bug)
- Add Spawn Health/Armor settings for dedicated servers
- Add Kill Reward settings for dedicated servers
- Do not load unnecessary VPPs in dedicated server mode
- Add level filename to "Level Initializing" console message
- Properly handle WM_PAINT in dedicated server, may improve performance (DF bug)
- Disable adding faces to fix PS2 tiling in editor
- Disable red background in log window if geometry limits are crossed in editor
- Update geometry limits in editor so they show static pools size (crossing it can have tiny performance impact)

[@GooberRF](https://github.com/GooberRF)
- Fix crash when `verify_level` command is run without a level being loaded
- Fix cockpit not rendering for any jeeps after the first one entered each level load
- Add `server_password` command
- Add `$DF CTF Flag Return Time` option in dedicated server config
- Add mod name to main menu
- Make value of `spectate_mode_minimal_ui` persist between game launches
- Add `version` command
- Add support for bluebeard.bty (sound config file) in mods

[@is-this-c](https://github.com/is-this-c)
- Support `©` in TrueType fonts
- Improve frag message format
- Search command descriptions in `.` command
- Improve PF network protocol compatibility

[@Mystyle-48](https://github.com/Mystyle-48)
- Simplify installation detection in setup and launcher
- Add German and French RF 1.10 detection to setup
- Text improvements to setup
- Don't copy over the changelog during setup

Version 1.8.0 (released 2022-09-17)
-----------------------------------
[@rafalh](https://github.com/rafalh)
- Add autosave after a level transition
- Disable big HUD for resolutions lower than 1024x768 to prevent crashes
- Fix "Wrong player ID" warnings when multiplayer quick save/load is used (client-side fix)
- Restructure Options window
- Add option to disable beep sound when new player joins multiplayer game and window is not focused
- Change update checker URL
- Make editor window resizeable
- Add `debug_event_msg` command that allows to track event messages in console
- Do not panic when character animation cannot be loaded, instead fall back to the first animation - fixes
  "Too many animations" errors occuring in specific conditions in some run maps (note that this problem was
  not occuring in the stock game because it allowed memory corruption to happen)
- Log critical error message
- Do not show glock with silencer in 3rd person view if current primary weapon is not a glock (RF bug)
- Change screenshot filename template (it now includes date, time and level filename)
- Fix multiple out-of-bounds writes in save game code (fixes crash when saving the game in some custom levels)
- Do not use TTF fonts in game menu if VF fonts are modded (should fix some unofficial game localizations)
- Change transparency sorting algorithm to fix flamethrower particles rendering in rooms with liquid and/or
  semi-transparent details
- Optimize finding a new room for moving objects
- Fix possible OOB write in waypoint list read code
- Add server verification in update checker using Ed25519 signature
- Fix opacity of Display_Fullscreen_Image being affected by previous drawing operations
- Restore blackout from a save file if it was killing the player or ending the game
- Fix crash when skipping a cutscene that stared when blackout was in progress
- Remove limit of restored objects during a level transition
- Fix possible buffer overflow if transitioned object mesh name is longer than 31 characters
- Change default FPS limit to 120 to comply with the stock game
- Fix loading a save game when player entity is out of level bounds
- Fix possible freeze when burning entity is destroyed

[@GooberRF](https://github.com/GooberRF)
- Add `fps_counter` command

[@is-this-c](https://github.com/is-this-c)
- Add version and date to `/info` server chat command
- Fix particle damage on dedicated servers
- Add `.` command to make finding commands easier
- Fix blurry fonts in the launcher on HiDPI monitors

[@Mystyle-48](https://github.com/Mystyle-48)
- Allow start menu shortcut to be skipped in setup program
- Simplify installation name in Windows control panel (no longer includes "version [version]")
- Add icon for control panel entry
- Restore original Red Faction Launcher on uninstall if replaced with Dash Faction Launcher link
- Add game installation path autofill for German GOG RF to setup program
- Remove unnecessary "Unknown tables.vpp version" error from setup program (the launcher already displays an error for mismatched tables.vpp files)

[@natarii](https://github.com/natarii)
- Ignore browsers when calculating player count for info requests
- Fix Message event crash on dedicated server
- Prevent browsers from voting
- Add browser detection for RfServerBrowser 5.1.4
- Add `swap_grenade_controls` command
- Don't overwrite existing levels in `download_level` command
- Add `download_level_force` command that overwrites existing levels
- Allow the person who started a vote to cancel it
- Allow setting max players server setting to 1
- Change launcher window title
- Allow autocomplete for `map` command
- Enable `map` command in RCON

Version 1.7.0 (released 2021-06-05)
-----------------------------------
[@rafalh](https://github.com/rafalh)
- Change maximal value of Max Kills dedicated server config to 999
- Support Team DM scores above 255
- Display proper server name in the scoreboard when joining by link
- Level auto-downloader has new UI and does its job without leaving the server
- Use SSL for communication with FactionFiles in level auto-downloader
- Add `pow2_tex` command for forcing usage of power of two textures
- Add support for redirecting dedicated server standard output stream to a file in win32-console mode
- Add server-side part of Pure Faction client verification
- Add `$DF Anticheat Level` option in dedicated server config
- Send Kills/Deaths statistics to players joining an already started gameplay
- Send player statistics (kills, deaths, accuracy) in private message after round ends in Dash Faction server
- Update fpgun skin when switching players in spectate mode
- Update spectate mode UI
- Do not render the level twice when Message Log is open
- Add `spectate_mode_minimal_ui` command for disabling most of spectate mode texts
- Add welcome message function for dedicated servers
- Fix 3D sounds ignoring master volume during first frame after creation if EAX is enabled
- Fix restarting muted ambient sounds
- Change editor hotkey for lightmap+lighting calculation to Ctrl+Shift+L because old hotkey was conflicting with
  other function
- Fix handling conflicting Activated By property when editing multiple triggers in editor
- Fix copying bool properties in some events
- Fix move sound not being muted if entity is created hidden
- Fix console being rendered in wrong place on endgame screen
- Fix memory corruption when transitioning to 5th level in a sequence and the level has no entry in ponr.tbl
- Fix music being unstoppable by events after entering the game menu
- Calculate mesh static lighting when object mesh is switched to corpse
- Make sure GR/FR game localizations have priority over EN files in case someone messed his RF directory
- Fix persona messages being displayed too low in Big HUD mode in vehicles
- Fix rfl file extension association with Dash Editor not working properly for filenames containing space characters
- Add `mute_all_players` and `mute_player` commands for hiding chat messages from specific players
- Add server config option to give max ammo to the player when a weapon item is picked up
- Append dedicated server config name to the log file name so logs from multiple servers won't mix
- Change log entry time format
- Fix clients randomly losing connection to the server (RF bug)
- Fix 2D sound regression when EAX is enabled
- Remove limit of loaded skeletons (*.rfa) - fixes crash when loading L1S1 and then pdm-deathmatch-run-v2
- Remove memory corruption when too many skeletons/animations are added to a single character
- Optimize skeleton unlinking when character is destroyed
- Fix rendering menu background if its height is lower than 480 (for example AQuest Speedrun Edition mod)

[@GooberRF](https://github.com/GooberRF)
- Added additional textures from base game to level editor texture browser

Version 1.6.1 (released 2021-02-20)
-----------------------------------
[@rafalh](https://github.com/rafalh)
- Improve tbl parsing error message
- Bring back team icons in team scores HUD (left-bottom screen corner) in Team DM multiplayer game (1.5.0 regression)
- Fixed rarely occuring client-side crash in multiplayer (regression from 1.6.1-beta)
- Fix possible game freeze when scrolling the message log on a wide screen
- Add `$DF UPnP Enabled` option in dedicated server config that allows to disable UPnP
- Fix hit sound packet sometimes being sent from the client
- Do not play hit sounds if player damages themselves
- Fix possible network issues if multiple clients sharing one IP address connect to a server in a very short period of
  time (RF bug)
- Fix drawing main menu background on some graphics cards/drivers (1.6.1-rc1 regression)
- Change the key that crashes a frozen game process to Alt and change the minimal hold time to 1 second
- Add `$DF Max FOV` option in dedicated server config that allows to limit FOV used by clients for level rendering

[@GooberRF](https://github.com/GooberRF)
- Change launcher header image
- Add more textures from base game to the level editor texture browser

Version 1.6.1-rc1 (released 2021-02-13)
---------------------------------------
[@rafalh](https://github.com/rafalh)
- Fix background image being scrolled too fast in multiplayer menu
- Make sure CTF flag does not spin after level change if it was in dropped state in the previous level
- Make main menu background image scroll animation smooth on high resolution displays
- Add `fpgun_fov_scale` command that allows adjustment of fpgun (view model) FOV
- Remove UID limit (50k) in editor
- Fix shape and script name being overwritten if editing properties of multiple triggers that have those fields set
  differently
- Do not verify mod vpp checksum if `Require Client Mod` option is disabled in server config
- Fix near plane clipping issues happening when FOV is big
- Increase max FOV to 160
- Fix clicking "Fav" checkbox in the server list

Version 1.6.1-beta (released 2021-01-29)
----------------------------------------
[@rafalh](https://github.com/rafalh)
- Fix items not respawning in DF-hosted server (1.6.0 regression)
- Fix ambient sound volume not being affected by `levelsounds` command if EAX sounds are disabled (1.6.0 regression)
- Add `screenshot` command
- Add `d_profiler` command for Release builds (single player only)
- Fix vote not ending when level ends in hosted server (1.6.0 regression)
- Fix CTF flag sometimes not returning to the base automatically (RF bug)
- Fix spectate camera staying in fixed mode after level change
- Fix editor crash when building geometry after lightmap resolution for a face was set to Undefined (RF bug)
- Improve crash reporting for Dash Editor and launcher
- Do not print redundant new lines in `-win32-console` server mode and optimize it a little
- Fix unwanted server-side weapon fire when switching from a continous weapon or a weapon with burst fire
- Prevent shooting and reloading while switching weapon server-side
- Increase decals limits
- Add server-side CPS limit
- Separate maxfps cvar for dedicated server and client
- Clear console input field when Control+C is pressed
- Do not add repeated commands to the console history
- Rename option "Disable LOD models" to "High model details" - it now scales LOD distances by 10, scale can be updated
  using `lod_distance_scale` command
- Add `fov` command which allows to override horizontal FOV value
- Fix crash that is occuring when cutscene path node is copied in the level editor (RF bug)
- Fix out of ammo sounds being played when ammo got out of sync client-side
- Add missing `consolebutton2A.tga` texture which is used by "Console Button02" clutter object
- Fix chat input box rendering in big HUD mode when a very long message is being entered
- Improve scope weapons zoom in spectate mode

Version 1.6.0 (released 2021-01-07)
-----------------------------------
[@rafalh](https://github.com/rafalh)
- Fix sounds being often skipped if volume in game options is set very low (1.6.0-rc1 regression)
- Fix rockets not making damage after hitting a detail brush (fix included in 1.6.0-beta did not always work)
- Add `frametime_graph` command
- Small code improvements

Version 1.6.0-rc1 (released 2020-12-31)
---------------------------------------
[@rafalh](https://github.com/rafalh)
- Fix rocket launcher IR scanner in spectate mode (regression)
- Fix 3D sounds having wrong volume for a very short time (regression)
- Disable unsupported color depths in launcher options
- Open RF wiki instead of hlp file when F1 is pressed in editor
- Add more tools in editor menu
- Change editor hotkey for create packfile function to Ctrl+Shift+S
- Do not render CTF flag being hold by a spectated player
- Add more checks for buffer overflow errors when parsing tbl files
- Add a warning message when starting game with a mod that has a space in the name
- Add missing textures: `mtl_archway04-mip1.tga`, `pls_airlockmat01-mip2.tga`
- Add more textures from base game to the level editor texture browser
- Fix buffer overflow occuring when using Play Level command in level editor if level path and/or Dash Faction path is
  long
- Improve performance of sound function `ds_get_channel`
- Fix `level_sounds` command not affecting ambient sounds (1.6.0-beta regression)
- Refactor sounds allocation to avoid frequent allocation and freeing of DirectSound buffers when channels pool is
  full - should improve performance
- Add `kill_messages` command that allows to disable printing information about kills in the chatbox and the game console
- Fix hud msg never disappearing in spectate mode
- Fix Brush Cliping Tool window causing speed reduction for level editor
- Other stability fixes

Version 1.6.0-beta (released 2020-12-12)
----------------------------------------
[@rafalh](https://github.com/rafalh)
- fix alpha draw order issues in envirosuit1.vcm, Vet1.v3m, coffeesmokedtbl2.v3m, coffeesmokedtblAlt.v3m meshes
- add support for bitmaps with alpha channel in Display_Fullscreen_Image event
- fix level editor not using all space for rendering when used with a big monitor
- fix wrong aspect ratio in level editor when used with a wide screen
- use Hor+ FOV scaling method in level editor perspective view
- make level editor perspective view FOV depend on view aspect ratio
- add Save, Pack, Lighting, Play, Play (camera) buttons in top bar of main editor window
- remove "Packfile saved successfully!" message after packfile generation in the level editor
- do not reduce editor FPS when editor dialog box is open
- fix changing properties of multiple respawn points in level editor
- add multiplayer settings in trigger properties window in level editor
- load V3M meshes from `user_maps\meshes` and `red\meshes` in the level editor
- fix lights randomly stopping working after copying in the level editor
- fix rendering of VBM textures from `user_maps\textures` in the level editor
- fix level editor crash when selecting decal texture from 'All' folder
- no longer require "-sound" argument when launching level editor with sound support
- allow opening a level in the level editor by using `-level` command line argument
- add option to associate `rfl` file extension with Dash Faction level editor in the setup program
- fix level editor crash randomly occuring when opening cutscene properties
- add more textures from base game to the level editor texture browser
- fix packing a level that contains a particle emitter with a default texture in the level editor
- fix path node connections sometimes being rendered incorrectly in the level editor
- use dynamic allocation when pool is full for weapons, debris, corpses, decal polygons, faces, face vertices,
  bounding boxes, vertices, meshes (it was listed as a feature of Dash Faction 1.5.0 but the patch was actually
  broken)
- support starting the launcher from a symlink
- fix objects having invalid radius after Switch_Model event
- allow spectating in servers with Force Respawn enabled before player spawns for the first time
- fix crash when using Switch_Model event on object with collisions disabled
- fix replacing collision spheres when using Switch_Model event
- recalculate mesh static lighting after Switch_Model event
- optimize logging
- add an option to override RF launcher by a symbolic link in the setup program
- fix rockets not making damage after hitting a detail brush
- allow "debug network" command usage in multiplayer
- fix rotation interpolation (Y axis) in multiplayer when it goes from 360 to 0 degrees
- add links to Red Faction Wiki in RED Help menu replacing old "Help Topics" menu item that did not work on modern
  Windows versions because of the lack of support for WinHelp format
- fix F4 key (Maximize active viewport) in RED for screens larger than 1024x768
- fix shooting through transparent sections of DDS textures
- change default server update rate to 20
- add 'update_rate' command server-side and client-side
- fix object interpolation in multi-player being played too fast causing a possible jitter
- replace editor icon
- add P hotkey for packfile generation in editor
- add Ctrl+L hotkey for lightmaps and lighting calculation in editor
- add more items in editor main menu
- add option in setup program to enable Windows Visual Styles for the level editor
- performance optimizations
- fix memory corruption when saving game after 4 level transitions and current level does not have an entry in ponr.tbl
  (fixes crash in Bearskinner SP campaign)
- use DirectSound3D position and velocity parameters for 3D sounds instead of emulating 3D by panning (enables doppler effects and allows proper 3D experience in setups with more than 2 sound channels e.g. 5.1)
- improve compatibility with dsoal (allows EAX effects emulation)
- fix entity staying in crouched state in a liquid if it was crouching when entering the liquid
- use Direct3D 9 rendering API
- optimize dynamic textures for Direct3D 9
- send operating system version when checking for updates in the launcher

[@GooberRF](https://github.com/GooberRF)
- add icon for EAX Effect objects in the level editor

Version 1.5.0 (released 2020-09-05)
-----------------------------------
[@rafalh](https://github.com/rafalh)
- increase object limit from 1024 to 65536
- remove additional limits on number of allocated bullet, item, debris and corpse objects
- enable and fix items and clutters static lighting calculation
- enable Set_Liquid_Depth implementation - check out Geothermal Plant in single player and see what changed
- add DDS texture format support (useful for DXTn compressed textures)
- rewrite crash reporting system UI
- improve sound handling when active sounds pool is full
- allow custom meshes in Switch_Model event
- add 'rf' protocol handler (URLs has format: rf://IP:PORT)
- add setup program which installs Dash Faction and patch Red Faction if needed
- add '/save' and '/load' chat commands and quick save/load controls handling for saving and restoring player position
  in multiplayer (useful for runmap servers, can be enabled in dedicated_server.txt)
- add 'Big HUD' option and 'bighud' command for making the HUD larger
- add 'reticle_scale' command for changing reticle size
- make texts in main menu and in HUD sharp by using TrueType fonts
- log error when RFA cannot be loaded
- improve render to texture performance (e.g. mirrors and scanners)
- improve 2D drawing performance
- improve level loading time
- fix performance degradation occurring if no level with fog effect was played in current game session, for some levels
  it can double the FPS (RF bug)
- add 'damage_screen_flash' command for toggling damage screen flash effect
- render held corpse in monitors/mirrors
- add better support for non-US keyboard layouts
- add support for CTRL+V (paste from clipboard) in dialog boxes
- use SHA1 for files verification in launcher
- add support for RF.exe in version 1.20 NA with 4GB flag enabled
- stretch image header in launcher so it is properly displayed when system DPI is changed
- add option for switching game language (English, German or French)
- improve hiding HUD by 'hud' command
- add server-size validation of ammo when handling weapon fire
- use 24-bit depth buffer if supported even if video card is not Nvidia (fixes Z-fighting on Intel and AMD cards)
- use dynamic light detail texture for all cards if multi-texturing is available (improves graphics quality on Intel
  cards)
- not treat every chat message starting with 'server' as a server command
- add GOG install directory auto-detection
- add server-side support for late joiners aware triggers (improves Pure Faction compatibility)
- load .tbl files in user_maps (default files cannot be overridden) - allows inclusion of HUD messages in custom maps
  (*_text.tbl)
- increase maximal number of simultaneously playing sounds to 64
- use port 7755 for server hosting when 'Force port' option is disabled
- add UPnP automatic port forwarding when hosting a server
- add 'download_level' command which invokes level downloader
- improve algorithm for selecting texture D3D format (should reduce VRAM usage)
- add 'Rate' setting in Options window (allows specifying network connection speed)
- add 'Adapter' setting in Options window (allows selection of graphics card)
- insert server name in window title when hosting dedicated server
- display joining player IP address in dedicated server console
- add dedicated server option for forcing player character
- add '-exe-path' launcher command line argument that allows running multiple dedicated servers using separate RF
  directories
- add support for nearest neighbor texture filtering ('nearest_texture_filtering' command)
- try setting gamma using Direct3D in full-screen mode before falling back to WinAPI (can fix gamma issues)
- fix sending crash reports
- fix non-working face scrolling before first geomod use in custom levels (RF bug)
- fix memory and VRAM leak when quick saving (RF bug)
- fix VRAM leak when playing Bink video (RF bug)
- fix AI flee animation regression
- fix crash caused by AI state not being properly updated during level load for entites that are taken from the
  previous level (RF bug)
- fix crash caused by corpse pose pointing to not loaded entity action animation for corpses that are taken from the
  previous level (RF bug)
- fix buffer overflow on level load when too many objects are being preserved from the previous level; fixes crash in
  Spaceport Redux campaign (RF bug)
- fix crash when executing camera2 command in main menu (RF bug)
- fix crash when item cannot be created during RFL load in multi-player (RF bug)
- fix entity collisions with objects near absolute zero position (RF bug)
- fix bullets collisions with big objects, especially with high FPS (RF bug)
- fix Message Log window rendering if resolution ratio is not 4:3 (RF bug)
- fix flamethrower graphical issues when playing with high FPS (RF bug)
- reduce maximal particle emitter spawn rate to 60 particles per frame in order to improve graphics and performance
  (framerate dependent particle emitters like flamethrower weren't designed with 240 FPS in mind so they look bad with
  too many overlapping particles)
- fix rendering of liquids and details with alpha in Railgun scanner (RF bug)
- fix entity/fpgun ambient color being improperly calculated when 'True color textures' option is active
- fix invalid lightmap sometimes being used for rendering of details with alpha; fixes glass color being affected by
  shooting in glass_house.rfl (RF bug)
- fix items not being respawned after ~25 days of server uptime (RF bug)
- fix players' pings being improperly calculated after ~25 days of server uptime (RF bug)
- fix compatibility of Crash Handler module with Windows XP and Vista
- fix infinite loop when failed to load Bink video (RF bug)
- fix memory leak when trying to load non-existing Bink video (RF bug)
- fix crash caused by explosion near dying player-controlled entity (RF bug)
- fix unwanted fire after weapon switch from weapon with impact delay (RF bug)
- fix wall-peeking (RF bug)
- fix weapon being auto-switched to previous one after respawn even when auto-switch is disabled (RF bug)
- fix swap_assault_rifle_controls cvar issues with stopping the fire and shooting from vehicles
- fix team scores in TDM
- fix auto-detection of Steam based Red Faction installation directory
- fix crash if camera cannot be restored to first-person mode after cutscene (RF bug)
- fix handling of alpha-only (A8) texture format when 32-bit textures are used
- fix switch weapon UI resetting fire wait timer without checking old value (RF bug)
- fix crash when particle emitter allocation fails during entity ignition (RF bug)
- fix possible buffer overflow in console input buffer (RF bug)
- fix possible server side crash when kicking a player using rcon (RF bug)
- fix alpha draw order issues in Hendrix, Generator_Small01, Generator_Small02, LavaTester01, Weapon_RiotShield meshes
  (RF bug)
- fix corona lights being visible through clutters, items, corpses (RF bug)
- fix possible 'buzzing' sound in multi-player game if some players are in water (RF bug)
- fix dedicated server crash when loading level that uses directional light (RF bug)
- fix heap corruption when loading a bitmap with corrupted header (RF bug)
- fix stack corruption when loading a corrupted packfile (RF bug)

[@GooberRF](https://github.com/GooberRF)
- add textures for big HUD

Version 1.4.1 (released 2019-12-07)
-----------------------------------
[@rafalh](https://github.com/rafalh)
- add 'skip_cutscene_bind' command allowing to change the control used for skipping cutscenes (by default Multiplayer
  Stats - TAB)
- fix crash after exiting administration with Gryphon (regression)
- fix crash when joining Pure Faction servers (regression)

Version 1.4.0 (released 2019-12-04)
-----------------------------------
[@rafalh](https://github.com/rafalh)
- add free camera spectate mode support
- add vote system (server-side)
- rework fix for sticking to the ground when jumping - now it is based on PF code (by Trotskie) - should fix free-fall
  animation bug in multi-player and occasionally sticking to the ground in some levels
- add 'map_ext' command for extending round time (server-side)
- increase precision of output from 'ms' command
- add 'show_invisible_faces' subcommand in 'debug' command
- make sure log file is not spammed by errors happening every frame
- allow cursor movement outside of game window when in menu
- slightly optimize screenshot capturing
- add optional hit-sounds in dedicated server
- add 'map_rest', 'map_next', 'map_prev' commands
- add optional Win32 native console support in dedicated server mode
- remove stripping of '%' character in chat messages
- make sure required registry values are always created on game launch
- improve FPS limit accuracy
- add item replacements support in dedicated_server.txt
- add default player weapon class and ammo override support in dedicated_server.txt
- add 'pctf' level name prefix support (server-side)
- reset fpgun animation when player dies
- sync kills/deaths statistics when joining to Pure Faction servers
- reduce launcher size and allow building it on Linux after replacing MFC by Win32++ library
- skip processing of non-damaging particles on a dedicated server (optimization)
- improve particles simulation performance
- delete projectiles when they reach bounding box of the level (should fix playing space levels with huge amount of
  people)
- stop distant sounds when sound pool is full and a new sound is being started (should fix sounds sometimes stoping
  working when playing multiplayer with a lot of players)
- sort items and clutter by mesh to improve performance
- change cutscene skip control to Multiplayer Stats to avoid unintended skipping
- add control name in cutscene skip hint message
- increase entity simulation max distance (fixes far entities not being animated even when moving)
- add support for players_request PF packet (used by online server browser)
- fix VRAM leak on level load caused by font textures (RF bug)
- fix occasionally getting stuck on jump pads on high FPS
- fix 'debug' command not flushing geometry cache when disabling flags
- fix dedicated server crash when loading level with screens
- fix submarine exploding bug if L5S3 level was loaded using 'level' command
- fix killed glass restoration from a save file
- fix very long startup on Windows XP in some hardware configurations
- fix crash when saving/loading custom levels with a lot of objects
- fix debris object pool (used for ejected shells) easly getting exhausted when local player is dead
- fix loading images with color palette in 32-bit mode
- fix default player weapon being incorrect in listen server after playing on a modded server
- fix crash during texture load if it allocation in RAM fails
- fix RAM leak if texture creation by D3D fails
- fix cutscene cinema bars when MSAA is enabled (regression)
- fix improper alpha sorting for Invulnerability Power-up items (RF bug)
- fix flee animation on steep ground for AI controlled characters on high FPS

Version 1.3.0 (released 2019-05-07)
-----------------------------------
[@rafalh](https://github.com/rafalh)
- enable mip-mapping for textures bigger than 256x256
- change maximal FPS limit to 240
- speed up server list refreshing
- put not responding servers at the bottom of server list
- add linear pitch option (stops mouse movement from slowing down when looking up and down)
- fix bullet collision testing if mover is in the line of the shot
- add mod selector in launcher
- add "Keep launcher open" option
- increase resolution of monitors and mirrors
- change screen-shake emulated FPS to 150 to match PF default FPS
- fix invalid player entity model when undercover (RF bug)
- fix decal fade out effect (RF bug)
- open server list menu instead of main menu when leaving multiplayer game (affects level download too)
- optimize Flamethower rendering
- optimize particles and HUD rendering by reducing number of draw-calls (both in game and level editor)
- optimize rendering of railgun and rocket launcher scanners by using different back buffer capture technique
- optimize glare rendering
- use hardware vertex processing in game and level editor (optimization)
- reduce number of draw-calls when rendering lines in level editor (optimization)
- properly handle Team property in Spawnpoint object in TeamDM game (server-side) - improves PF compatibility
- client-side/solo/teleport triggers handling (server-side) - improves PF compatibility
- handle trigger team property (server-side) - improves PF compatibility
- fix 'Orion bug' - default 'miner1' entity spawning periodically on clients (server-side)
- fix beeping every frame if chat input buffer is full
- apply proper chat input limits client-side so PF server does not kick the sender
- preserve password case when processing rcon_request command (server-side)
- fix possible crash when loading a corrupted save file
- block creation of corrupted save files when cutscene starts
- allow skipping cutscenes by pressing jump key
- fix time synchronization on high FPS in cutscenes
- fix glares being visible through characters in some cutscenes
- add 'findlevel' command as alias for 'findmap'
- add 'map' command as alias for 'level'
- allow 'level' command outside of multiplayer game and remove now redundant 'levelsp' command
- add 'show_enemy_bullets' command for toggling enemy bullets visibility (configuration is persisted) - it was always
  enabled in previous versions
- add persisting of volumetric lights (glares) configuration changed by 'vli' command
- add 'fullscreen' and 'windowed' commands
- fix "Buffer overrun" message being displed in RED (level editor) when too many objects were selected
- optimize displaying selected objects in log view in RED (level editor)
- add 'debug' command for enabling RF built-in debugging features
- add 'playercount' command
- fix multiple security vulnerabilities
- stability improvements
- a lot of code was rewritten into modern C++

Version 1.2.1 (released 2017-08-15)
-----------------------------------
[@rafalh](https://github.com/rafalh)
- added high resolution scopes
- fixed camera shake dependence on FPS (e.g. for Assault Rifle)
- fixed Level Sounds option not working in most levels
- fixed Stretched Window Mode not using full screen if choosen framebuffer resolution was lower than screen
  resolution
- fixed Bink videos not working when True Color textures are enabled
- added Drag&Drop support in Level Editor for opening RFL files
- added auto-completition with TAB key for console commands: level, kick, ban, rcon
- improved crash reporting functionality
- adjust game configuration (default weapon, clip size, max ammo) based on packets from server (improves gameplay
  on highly modified servers like Afaction)
- fixed game crash when level geometry is highly modified by GeoMod and has too many vertices
- speed up game startup
- other stability improvements

Version 1.2.0 (released 2017-05-10)
-----------------------------------
[@rafalh](https://github.com/rafalh)
- added running and jumping animations in Spectate Mode
- added Rocket Launcher scanner rendering in Spectate Mode
- fixed Spectate Mode target entity being rendered (visible when player looks down)
- fixed keyboard layout for QWERTZ and AZERTY keyboards (automatic detection)
- added option to disable level ambient sounds (in launcher or by levelsounds command)
- added Game Type name to Scoreboard
- added fast show/hide animations to Scoreboard (configurable in launcher)
- added 'findmap' command
- fixed improper rendering after alt-tab in full screen (especially visible as glass rendered only from one side)
- fixed cursor being not visible in menu when changing level
- hidden client IP addresses in server packets
- fixed compatibility with old CPUs without SSE support
- removed error dialog box when launcher cannot check for updates
- changed chatbox color alpha component
- added small rendering optimizations
- other minor fixes

Version 1.1.0 (released 2017-02-27)
-----------------------------------
[@rafalh](https://github.com/rafalh)
- improved loading of 32 bit textures (improves quality of lightmaps and shadows)
- launch DashFaction from level editor (editor must be started from DF launcher)
- improved Scanner resolution (Rail Gun, Rocket Launcher, Fusion Launcher)
- added alive/dead indicators in Scoreboard
- added truncating text in Scoreboard if it does not fit
- added Kills/Deaths counters in Scoreboard
- fixed console not being visible when waiting for level change in Multiplayer (RF bug/feature)
- changed font size for "Time Left" timer in Multi Player
- hackfixed submarine exploding in Single Player level L5S3 on high FPS (RF bug)
- fixed bouncing on low FPS
- added support to setting custom screen resolution in Launcher Options (useful for Windowed mode)
- changed default tracker to rfgt.factionfiles.com
- stability fixes

Version 1.0.1 (released 2017-02-07)
-----------------------------------
[@rafalh](https://github.com/rafalh)
- fixed monitors/mirrors in MSAA mode
- fixed aspect ratio in windowed mode if window aspect ratio is different than screen aspect ratio
- fixed left/top screen edges not rendering properly in MSAA mode
- fixed right/bottom screen edges being always black due to bug in RF code
- fixed water waves animation on high FPS (with help from Saber)
- fixed invalid frame time calculation visible only on very high FPS
- player name, character and weapons preferences are no longer overwritten when loading game from save
- fixed exiting Bink videos by Escape
- other stability fixes
- launcher icon has higher resolution

Version 1.0.0 (released 2017-01-24)
-----------------------------------
[@rafalh](https://github.com/rafalh)
- Spectate Mode
- launcher GUI
- DirectInput support
- windowed and stretched mode support
- support for country specific game edition (German and French)
- added level author and filename information on scoreboard
- fixed jump, hit-screen and toggle console animations on high FPS
- added checking for new Dash Faction version in launcher
- fixed multiple Buffer Overflows in RF code (security)
- added "ms" command for changing mouse sensitivity
- added "vli" command for disabling volumetric lightining
- changed default sorting on Server List to player count

Version 0.61
------------
[@rafalh](https://github.com/rafalh)
- added widescreen fixes
- enabled anisotropic filtering
- enabled DEP to improve security
- implemented passing of launcher command-line arguments to game process

Version 0.60
------------
[@rafalh](https://github.com/rafalh)
- use exclusive full screen mode (it was using windowed-stretched mode before)
- check RF.exe checksum before starting (detects if RF.exe has compatible version)
- added experimental Multisample Anti-aliasing (to enable start launcher from console with '-msaa' argument)
- improved crashlog
- added icon

Version 0.52
------------
[@rafalh](https://github.com/rafalh)
- use working directory as fallback for finding RF.exe

Version 0.51
------------
[@rafalh](https://github.com/rafalh)
- first public release
