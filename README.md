Alpine Faction
============
![Alpine Faction](docs/alpinelogo1.png)

[![Download](https://img.shields.io/github/v/release/GooberRF/alpinefaction?label=download&color=brightgreen)](https://dl.alpinefaction.com/)
[![License: MPL-2.0](https://img.shields.io/badge/license-MPL--2.0-blue)](LICENSE.txt)
[![Changelog](https://img.shields.io/badge/changelog-view-blue)](docs/CHANGELOG.md)
[![CI](https://github.com/GooberRF/alpinefaction/actions/workflows/ci.yml/badge.svg)](https://github.com/GooberRF/alpinefaction/actions/workflows/ci.yml)

[![Website](https://img.shields.io/badge/website-alpinefaction.com-blue)](https://alpinefaction.com)
[![Discord](https://img.shields.io/badge/Discord-FactionFiles-5865F2?logo=discord&logoColor=white)](https://discord.gg/factionfiles)

About
-----
Alpine Faction is a free, open source, and actively developed patch for Red Faction (2001), and the definitive way to play the game today. It fixes the original game's bugs and security vulnerabilities, runs properly on modern systems, restores cut content, improves performance and graphics, and adds the features players expect from a modern game.

It isn't a source port, but it offers much of what you'd expect from one. It patches the original game and is compatible with any official copy of Red Faction.

Whether you're playing through the campaign, competing in multiplayer, running a server, building levels, or making a total conversion, Alpine Faction gives you the tools and the control you need to get the most out of Red Faction.

Alpine Faction began as a fork of rafalh's <a href="https://github.com/rafalh/dashfaction">Dash Faction</a>.

Alpine Faction requires:
* Windows 7 or newer (or you can use Wine)
* Any official distribution of Red Faction (Steam, GOG, retail, etc.)
    * All official localizations are supported (English, French, German)
    * Many other localizations are supported via mods

Key Features
-----
### Highlights
* Fixes for critical security vulnerabilities, game-breaking bugs, and high frame rate issues (including the infamous submarine explosion bug)
* Direct3D 11 renderer with real-time per-pixel lighting and full mesh shadows
* Drivable multiplayer vehicles and turrets: jeeps, APCs, drillers, fighters, and submarines
* 12 new multiplayer game types, plus server-configurable mutators and an in-game voting panel
* Multiplayer bots, with automatic waypoint generation for custom maps
* Demo recording and playback
* FactionFiles integration: multiplayer stats, single player achievements, level ratings, automatic level downloads, and one-click level and mod installs
* Terrain, ray traced lightmap baking, and 60+ new scripting events for level designers

### Gameplay
* Checkpoint autosaves at the start of each level
* Skippable cutscenes
* Restored water/lava rising in Geothermal Plant
* Player headlamp (flashlight)
* Support for any resolution and aspect ratio
* Fullscreen, windowed, borderless window modes
* Controller Support
* Enhanced mouse input with selectable sensitivity scaling

### Graphics and display
* Any resolution and aspect ratio, in fullscreen, windowed, or borderless window modes
* Anti-aliasing, full color range lighting, and colorblind mode
* Underwater effects including caustics, depth-based water fog, and distortion
* High resolution HUD and vector fonts
* Many performance and visual improvements

### Multiplayer
* 12 new game types: King of the Hill, Damage Control, Revolt, Escalation, Run, Bagman, Team Bagman, Pit, Gun Game, Wipeout, Salvage, and Scavenger
* Higher (and configurable) tick rate and many netcode improvements
* Damage feedback with hit sounds, kill sounds, and damage numbers
* Sprays, awards, assists, radio messages, and taunts
* Objective and teammate indicators, nameplates, location pings, and a minimap
* Player outlines with team and enemy colors, plus optional team X-ray
* Enemy and teammate footstep audio
* First person, third person, and free camera spectating, with static cameras and lag compensation
* Improved scoreboard with separate categories for spectators, bots, browsers, and idle players
* Competitive match framework with ready-up and overtime
* Cheating prevention
* Join servers from `rf://` links
* Discord rich presence

### Server hosting
* Dedicated server configuration file with per-level rules, live reloading, and on-the-fly game type switching
* Configurable (and votable) mutators, gibbing, weapon loadouts, spawn delays, delayed item spawns, and more
* Voting panel for kicking players, changing levels or game types, toggling mutators, and surrendering matches
* Server-side bots and headless bot clients, with customizable bot profiles
* Automatic demo recording
* Many server fixes and performance improvements

### Level and mod creation
* Ray traced lightmap baking with directional sunlight, baked mesh shadows, and high-resolution Alpine Lightmaps
* Heightmap terrain with sculpting, texture layer painting, mesh decorations (grass, rocks, etc.), and geomod support
* 60+ new scripting events, including events for gas regions, respawn points, capture points, mesh animation, fullscreen images, and gameplay rules
* Events and triggers that work properly in multiplayer, with new multiplayer trigger options
* New object types: Mesh, Corona, Note, Rope Emitter, Directional Light, Projection Camera, and Vehicle Factory
* Gas regions, weather regions (rain and snow), and dynamic lights
* Movers that can carry dynamic lights, particle emitters, and push regions
* More immersive skyboxes, with support for movers, meshes, and mesh lighting
* RF2-style brush-based geomod and breakable detail brushes with debris
* Full color range lightmaps
* Alpine Texture (ATX) format for texture animation (scriptable through events) and detail textures
* DDS, PNG, JPG, and 8-bit greyscale TGA textures, OGG audio, and custom BIK videos
* Custom HUDs and translation packs as client-side mods, custom meshes in multiplayer, and many hardcoded settings exposed to mods
* Many engine limits raised or removed, and legacy PS2 restrictions that hurt map performance removed
* Advanced debugging tools for maps and mods

### Level editor
* Move, rotate, and scale gizmo with snapping
* Mesh, sound, and improved texture browsers
* Texture mode tools such as Fit, Justify, Wrap, texture lock, and UV unwrapping
* Mirror tool, plus new face tools (Delete, Split, Flip Normal) and vertex tools (Delete, Bridge)
* Brush to static mesh (`.v3m`) conversion, and back
* Custom texture subdirectories
* Packfile creation that includes used meshes, animations, and textures
* Many bug fixes, performance, and workflow improvements

See the [CHANGELOG file](docs/CHANGELOG.md) for a detailed list of all features.

Compatibility
-------------
Alpine Faction runs on Red Faction 1.20 (North America). The installer converts other official versions automatically:
* For 1.00 or 1.10 NA, the installer applies the official 1.20 patch.
* For other retail localizations (French/German), the installer patches your game executable as needed and keeps your localization.
* For digital distributions (Steam, GOG, etc.), the installer patches your game executable as needed.

If you are using an unsupported game executable, the installer and launcher will prompt you to obtain the correct one when you try to launch the game.

Usage
-----
1. Download the installer from [dl.alpinefaction.com](https://dl.alpinefaction.com/) and run it. Alpine Faction does NOT have to be installed to your Red Faction folder.

2. Run `AlpineFactionLauncher.exe`.

3. Click the gear icon (next to "Play") to adjust options as desired.

4. Click the "Play" button to start playing.

### Settings files
Alpine Faction stores player settings in its own file, `alpine_settings.ini`, so that it can support its new features. The first time you launch Alpine Faction, it automatically imports your settings from a legacy `players.cfg` file left by any other Red Faction client.

The level editor likewise stores its `Preferences` window settings in `alpine_editor.ini`, importing them from the stock `red\red.cfg` the first time it starts.

Advanced usage
--------------
Alpine Faction adds many new console commands, command line options, and dedicated server configuration settings. See the reference documentation:
* [Console commands](docs/reference/CONSOLE_COMMANDS.md)
* [Command line switches](docs/reference/COMMAND_LINE.md)
* [Dedicated server configuration](docs/reference/DEDICATED_SERVER_CONFIG.md)

Pass command line switches to `AlpineFactionLauncher.exe`, which forwards them to the game or level editor.

Alpine Faction also adds many new features for level designers and mod developers. You can find full documentation on the RF Wiki:
* [Level design](https://www.redfactionwiki.com/wiki/Alpine_Faction_Level_Design)
* [Mod development](https://www.redfactionwiki.com/wiki/Alpine_Faction_Mod_Development)

Troubleshooting and support
---------------------------
Some antivirus software flags Alpine Faction as malicious. The launcher injects Alpine Faction's code into the game process as it starts, which is the kind of behaviour antivirus heuristics look for. Alpine Faction is safe to use, but you may need to explicitly allow it to run. If you do not trust the official Alpine Faction builds, you can review the code and compile it yourself.

If you run into any problems or have questions, please ask in the [Faction Files Discord server](https://discord.gg/factionfiles). You can also report bugs on [GitHub Issues](https://github.com/GooberRF/alpinefaction/issues).

Building
--------
See [docs/BUILDING.md](docs/BUILDING.md) for information about building Alpine Faction from source.

Contributing
------------
Contributions are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md) to get started, and use [GitHub Issues](https://github.com/GooberRF/alpinefaction/issues) to report bugs or suggest features.

Terms of Use
------------
Alpine Faction is provided as-is. Installing or using it means you agree to the terms and notices in [resources/terms.txt](resources/terms.txt).

License
-------
Alpine Faction's source code is licensed under Mozilla Public License 2.0. See [LICENSE.txt](LICENSE.txt).

Alpine Faction includes contributions from several authors. See [resources/licensing-info.txt](resources/licensing-info.txt) for more information.

Alpine Faction is a fork of Dash Faction, which is developed by rafalh. Licensing information for Dash Faction is available in [its README](https://github.com/rafalh/dashfaction?tab=readme-ov-file#license).
