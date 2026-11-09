# Command Line Switches

Pass these switches to `AlpineFactionLauncher.exe`. The launcher handles its own switches (listed under [Launcher](#launcher)) and forwards every other argument, in order, to the game (`RF.exe`) or level editor (`RED.exe`) it starts. The launched process runs with the game directory as its working directory, so relative paths below resolve from there unless noted.

Stock Red Faction and RED switches still work: pass them to the launcher the same way and they are forwarded.

## Launcher

| Switch | Arguments | Description |
| --- | --- | --- |
| `-game` | | Starts the game immediately, skipping the launcher window. |
| `-editor` | | Starts the level editor immediately, skipping the launcher window. |
| `-help`, `-h` | | Shows a message box listing the launcher switches, then exits. |
| `-exe-path` | `<path>` | Launches this `RF.exe` (with `-game`) or `RED.exe` (with `-editor`) instead of the one set in the launcher options. The executable still has to pass the launcher's version check. |
| `-aflink` | `<url>` | Handles an `af://` link. `af://link/<token>` links the client to a FactionFiles account, `af://download/<file id>` downloads and installs a level or mod from FactionFiles after asking for confirmation, and `af://demo/<game id>` downloads a demo into `demos\downloaded` and offers to play it. The `af://` prefix is optional. The installer registers the `af://` protocol to run `-aflink`. |
| `-play-demo` | `<file>` | Shows the demo's details (level, game type, server, date, size) and asks before starting the game in demo playback. Use a full path. The installer's `.afd` file association runs this switch. |
| (bare path) | `<file>.afd` | An argument that ends in `.afd`, does not start with `-` and is not the value of another switch is treated like `-play-demo <file>.afd`. This handles dropping a demo onto the launcher or "Open with". Only the first one is used. |

If neither `-game` nor `-editor` is given, the launcher picks one from the other switches:

- `-bake <input>` starts the editor.
- `-level`, `-levelm`, `-awpgen`, `-demo`, `-dedicated` or `-ads` starts the game.
- Anything else, for example `-url`, `-mod` or `-bot` on its own, opens the launcher window. The forwarded switches then apply when the game or editor is launched from that window, so add `-game` or `-editor` to start straight away.

## Game (RF.exe)

| Switch | Arguments | Description |
| --- | --- | --- |
| `-url` | `rf://<host>:<port>[/][?password=<password>]` | Joins the multiplayer server at that address once the game has started. The host can be an IP address or a host name, and the port must be 1-65535. The installer registers the `rf://` protocol to run `-game -url <url>`. |
| `-levelm` | `<level>` | Starts a local multiplayer (listen server) game on the level. `.rfl` is added if missing, and the level has to be installed. The game type comes from the file name prefix (`ctf`, `koth`, `dc`, `rev`, `run`, `esc`), otherwise Deathmatch. The server is not listed on the tracker, and its password is `password`. |
| `-demo` | `<file>` | Plays an Alpine Faction demo (`.afd`) once the game has started. `.afd` is added if missing. A relative name is looked up inside `<game dir>\demos` (`..` is not allowed); a full path is used as is. |
| `-afs` | `<file>` | Loads and saves Alpine Faction settings from this file instead of `alpine_settings.ini` (or a mod's `alpine_settings_<mod>.ini`). |
| `-script` | `<name>` | Runs a console script after the game has fully started. The game opens `<name>.txt`: any extension you give is replaced with `.txt`. Can be given more than once; the scripts run in order. |
| `-mod` | `<name>` | Stock switch that loads a TC mod. Alpine Faction also gives each mod its own settings file, `alpine_settings_<name>.ini`, unless the mod's `af_game.tbl` sets `$Use Base Game Players Config`. The launcher adds `-mod` itself when a mod is selected in its window. |
| `-debug` | | Turns on extra network debug logging. |
| `-d3d11-debug-layer` | | Creates the Direct3D 11 device with the debug layer. Only applies when the Direct3D 11 renderer is selected in the launcher options. If the debug layer is not installed, a warning is logged and the game starts without it. |

## Dedicated server

| Switch | Arguments | Description |
| --- | --- | --- |
| `-ads` | `<config.toml>` | Starts an Alpine Faction dedicated server with this TOML config. It does not need `-dedicated`. The Alpine Faction log file is `logs\AlpineFaction-dedicated-<config name>.log`, where the config name is the file name without folders or extension. A config can be built at https://dedi.alpinefaction.com. |
| `-dedicated` | `<name>` | Stock dedicated server switch, now deprecated. Without `-ads`, the server prints a message saying to use `-ads` with a TOML config and stops with a critical error. |
| `-min` | | With `-ads`: prints only minimal server information at startup. |
| `-log` | | With `-ads`: writes the full server console output to `logs\AlpineFaction-console-<YYYYMMDD>-<HHMMSS>.log`. |
| `-nodl` | | With `-ads`: does not download levels in the config that are not installed. |
| `-nogsk` | | With `-ads`: ignores the config's `fflink_gsk` setting, so the server skips FactionFiles registration. |
| `-win32-console` | | Uses a Windows console for the server instead of the stock console window. Under Wine this is always on. |

## Bots / waypoints

These switches are ignored on a dedicated server.

| Switch | Arguments | Description |
| --- | --- | --- |
| `-bot` | `<secret>` | Runs the client as a headless bot: no window, no rendering, no sound. Needs `-url` to say which server to join, and `<secret>` must be a non-zero number that matches the server's `bot_shared_secret` setting. Without both, the client prints an error and quits. |
| `-debugbot` | `<secret>` | Like `-bot`, but keeps the game window, rendering and sound so you can watch the bot, even if `-bot` is also given. |
| `-noquit` | | With `-bot` or `-debugbot`: the bot does not quit when it is disconnected from the server. |
| `-awpgen` | `<level>` | Loads the level, generates a waypoint (`.awp`) file for it, saves it to `user_maps\waypoints\<level>.awp` and quits. Runs without a window. `.rfl` is added if missing, and the level has to be installed. If an `.awp` for the level already exists, the new file gets the next revision number. Gives up after 60 minutes. |
| `-awpgen_fine` | | With `-awpgen`: probes Alpine terrain like other geometry instead of laying a grid over it. Wins over `-awpgen_coarse` if both are given. |
| `-awpgen_coarse` | | With `-awpgen`: also probes open ground away from Alpine terrain, at wider spacing. |

## Level editor (RED.exe)

Editor switches are not case-sensitive.

| Switch | Arguments | Description |
| --- | --- | --- |
| `-level` | `<file.rfl>` | Opens the level at startup, skipping the splash screen. Add `-editor`: without it, the launcher starts the game instead. The installer's `.rfl` file association runs `-editor -level <file>`. |
| `-skiplegacywarning` | | Skips the warning shown when opening a level saved by an older editor version. |
| `-d3d11` | | Uses the Direct3D 11 renderer for this session, overriding the editor Preferences. |
| `-d3d9` | | Uses the Direct3D 9 renderer for this session, overriding the editor Preferences. Wins over `-d3d11` if both are given. |
| `-d3d11-debug-layer` | | Creates the editor's Direct3D 11 device with the debug layer. If the debug layer is not installed, a warning is logged and the editor starts without it. |
| `-rawlightmaps` | | Saves the Alpine lightmap atlas as uncompressed RGB8 instead of BC7. Mainly for comparing output with stock lightmaps; files are larger. |
| `-sound` | | Stock switch that turned on sound in the editor. It is no longer needed: Alpine Faction turns editor sound on without it. |

### Headless bake

These switches build a level's geometry and lighting without anyone at the editor, save the result to a new file and exit.

| Switch | Arguments | Description |
| --- | --- | --- |
| `-bake` | `<input.rfl>` | Opens the level, runs Build Geometry, calculates lighting and saves to the `-bakeout` file, then exits. Implies `-editor`. Needs `-bakeout`. The launcher shows an error if `-bake` is the last argument and has no path after it. |
| `-bakeout` | `<output.rfl>` | With `-bake`: where to save the result. Must be a different file from the input. Progress and errors go to `<output.rfl>.log`. |
| `-buildonly` | | With `-bake`: skips the lighting calculation and saves straight after Build Geometry. |

During a bake the splash screen and legacy level warning are skipped, and message boxes are answered automatically and written to the log. The launcher does not report the result. A bake that finishes ends its log with `done rc=<code>`, which is also the `RED.exe` exit code. Without `-bakeout` there is no log file, only the exit code.

| Code | Meaning |
| --- | --- |
| `0` | Saved successfully. |
| `1` | Bad arguments: `-bakeout` missing, same as the input, or the input file not found. |
| `2` | The input level did not load. |
| `3` | More than 50 message boxes came up. |
| `4` | Saving the output failed. |
| `5` | The lighting calculation was refused. Nothing was saved. |
| `6` | Build Geometry failed. Nothing was saved. |

## Examples

Run a dedicated server from a TOML config in the game directory, in a Windows console, with the full console log written to a file:

```
AlpineFactionLauncher.exe -ads myserver.toml -win32-console -log
```

Build and light a level without opening the editor UI:

```
AlpineFactionLauncher.exe -bake "C:\levels\mymap.rfl" -bakeout "C:\levels\mymap_baked.rfl"
```

Start the game with a TC mod from `mods\mymod`:

```
AlpineFactionLauncher.exe -game -mod mymod
```

Join a password-protected server by URL:

```
AlpineFactionLauncher.exe -game -url "rf://192.168.1.10:7755/?password=secret"
```

Generate a waypoint file for a level:

```
AlpineFactionLauncher.exe -awpgen dm02
```
