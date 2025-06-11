# Alpine Faction Codebase Structure

## Overview

Alpine Faction is a comprehensive patch/modification for the 2001 FPS game Red Faction. It's built as a fork of Dash Faction and uses DLL injection and runtime patching techniques to modify the original game's behavior without requiring source code access to the game itself.

## Architecture

The project uses a modular architecture with several key components:

### Core Components

1. **Launcher** (`/launcher/`)
   - Windows MFC application providing the user interface
   - Handles game configuration and settings management
   - Manages the injection of patches into the game process
   - Key files:
     - `MainDlg.cpp/h` - Main launcher dialog window
     - `LauncherApp.cpp/h` - Application entry point
     - `Options*.cpp/h` - Various configuration dialogs

2. **Game Patch** (`/game_patch/`)
   - The main DLL that gets injected into the Red Faction game process
   - Implements all gameplay modifications, bug fixes, and new features
   - Organized into subsystems:
     - `bmpman/` - Bitmap/texture management enhancements
     - `debug/` - Debugging and profiling tools
     - `graphics/` - Graphics system improvements (D3D11 renderer)
     - `hud/` - HUD modifications and enhancements
     - `input/` - Input system improvements
     - `misc/` - Various gameplay modifications
     - `multi/` - Multiplayer enhancements
     - `object/` - Game object system modifications
     - `os/` - Operating system interface improvements
     - `purefaction/` - Anti-cheat system
     - `rf/` - Headers defining Red Faction's internal structures
     - `sound/` - Sound system enhancements

3. **Editor Patch** (`/editor_patch/`)
   - DLL injected into the Red Faction level editor (RED)
   - Adds new features and fixes bugs in the level editor
   - Key functionality:
     - Event system enhancements
     - Graphics improvements
     - Texture management fixes
     - Trigger system improvements

4. **Common Libraries**
   - **Common** (`/common/`) - Shared utilities and configuration management
   - **Patch Common** (`/patch_common/`) - Code injection and hooking framework
   - **Launcher Common** (`/launcher_common/`) - Shared launcher functionality
   - **XLog** (`/xlog/`) - Logging framework

5. **Support Components**
   - **Crash Handler** (`/crash_handler/`) - Enhanced crash reporting
   - **Crash Handler Stub** (`/crash_handler_stub/`) - Lightweight crash detection
   - **Resources** (`/resources/`) - Game assets (textures, sounds, shaders, etc.)
   - **Tools** (`/tools/`) - Build tools and utilities

## Key Technologies

### Code Injection Framework
The project uses sophisticated runtime patching techniques:
- **Function Hooks** - Intercept and modify game function calls
- **Call Hooks** - Replace specific function calls
- **Code Injection** - Insert new code into the game's memory space
- **Virtual Table Patching** - Modify C++ object virtual function tables

### Graphics Enhancement
- Custom D3D11 renderer replacing the original D3D8 renderer
- HLSL shaders for modern graphics effects
- Support for high resolutions and anti-aliasing

### Networking
- Enhanced multiplayer protocol with custom packets
- Level auto-download system
- Improved server-client communication

## Module Details

### Game Patch (`/game_patch/`)

The game patch is the heart of Alpine Faction. Key subsystems include:

#### Graphics System (`graphics/`)
- `d3d11/` - Modern Direct3D 11 renderer implementation
- `gr.cpp/h` - Main graphics interface
- `gr_font.cpp` - Font rendering improvements
- `legacy/` - Original D3D8 renderer hooks

#### Multiplayer System (`multi/`)
- `alpine_packets.cpp/h` - Custom network packet definitions
- `server.cpp/h` - Server enhancements
- `votes.cpp` - Voting system implementation
- `level_download.cpp` - Auto-download functionality

#### HUD System (`hud/`)
- `hud.cpp/h` - Core HUD modifications
- `multi_scoreboard.cpp/h` - Enhanced multiplayer scoreboard
- `hud_weapons.cpp` - Weapon display improvements
- `hud_world.cpp/h` - World HUD elements (objective markers, etc.)

#### Object System (`object/`)
- Event system enhancements for level scripting
- New event types for advanced level design
- Improved trigger functionality

### Editor Patch (`/editor_patch/`)

Enhances the RED level editor with:
- New event types matching game patch additions
- Improved texture handling
- Bug fixes for editor stability
- Enhanced trigger options

### Resources (`/resources/`)

Contains all additional game assets:
- `shaders/` - HLSL shader source files
- `images/` - UI textures and HUD elements
- `sounds/` - Custom sound effects
- `fonts/` - High-resolution fonts
- `meshes/` - Fixed/enhanced 3D models
- `tables/` - Configuration files

## Build System

The project uses CMake for cross-platform build configuration:
- Supports Windows (MSVC) and Linux (MinGW) builds
- Generates 32-bit binaries (required for compatibility)
- Modular CMakeLists.txt files in each component

## Development Workflow

1. **Understanding the Game**: The `rf/` directory contains reverse-engineered structures and interfaces from Red Faction
2. **Making Changes**: Most modifications involve hooking game functions and replacing/augmenting their behavior
3. **Testing**: Changes require testing in both single-player and multiplayer contexts
4. **Debugging**: Extensive debug commands and logging help diagnose issues

## Key Features Implementation

### Achievements System
- Integrated with FactionFiles.com API
- Tracks player progress across various gameplay metrics
- Implemented in `misc/achievements.cpp`

### Auto-Download System
- Allows players to automatically download custom maps
- Uses FactionFiles.com CDN
- Implemented in `multi/level_download.cpp`

### Enhanced Graphics
- D3D11 renderer provides modern graphics capabilities
- Support for high resolutions and widescreen
- Anti-aliasing and improved lighting

### Multiplayer Enhancements
- Increased tick rate for smoother gameplay
- Hit sounds and damage indicators
- Improved scoreboard and spectator mode
- Custom game modes (GunGame)

## Configuration and Settings

- `alpine_settings.ini` - New human-readable settings format
- Replaces legacy binary `players.cfg`
- Per-mod settings support (e.g., `alpine_settings_MODNAME.ini`)

## Security Considerations

- Fixes multiple security vulnerabilities in the original game
- Anti-cheat measures for multiplayer
- Safe handling of downloaded content

## Future Development

The codebase is designed to be extensible:
- New event types can be added for level designers
- Graphics system supports additional effects
- Multiplayer protocol allows custom packets
- Modular architecture facilitates feature additions