# Agent guide for Alpine Faction

This file is intended to help Codex, Claude, and other assistants (and other automated tools) work in this repository efficiently.

## Basic information
- Alpine Faction is a mod/patch for the video game Red Faction and its level editor.
- Original source code for the game and editor is not available.
- Alpine Faction's core files are a launcher (`AlpineFactionLauncher.exe`) and 2 dll files (`AlpineFaction.dll`), (`AlpineEditor.dll`)
- The launcher injects the new code from the dll files into the stock Red Faction processes (`rf.exe`), (`RED.exe`)

## Repository overview
- `common/`, `patch_common/`: Shared components.
- `game_patch/`: Core C++ code for the game patch.
- `editor_patch/`: Core C++ code for the game patch.
- `launcher/`, `launcher_common/`: Core C++ code for the launcher.
- `crash_handler/`: Core C++ code for the crash handler.
- `cmake/`, `CMakeLists.txt`: Build system and configuration.
- `docs/`: Documentation (see `docs/BUILDING.md` for build instructions).
- `resources/`: Assets and licensing information.
- `tools/`, `mesh_patches/`: Supporting tooling.

## Build and run
- Start with the build guide: `docs/BUILDING.md`.
- Prefer out-of-source CMake builds.
- If you add new targets or dependencies, update `docs/BUILDING.md` as needed.

## Testing and validation
- Preferred baseline check (Windows/MSVC):
  1. Open an "x64 Native Tools Command Prompt for VS".
  2. Configure an out-of-source build:
     - `cmake -S . -B build-msvc -G "Visual Studio 17 2022" -A x64`
  3. Build the default configuration:
     - `cmake --build build-msvc --config Release`
- If you cannot build in the environment, document that in your final response
  and mention the recommended MSVC steps above.

## Change guidelines
- Keep changes minimal and focused on the requested task.
- Match existing formatting and naming in nearby code.
- Update documentation when behavior or usage changes.

## Core game tie-ins (hooking/injection primitives)
- `FunHook`: Function-level hook helper. Use when you need to intercept or wrap an
  existing game function; typically stores the original function pointer so the hook
  can call through when needed.
- `CallHook`: Call-site patch helper. Use when you want to redirect a specific call
  instruction to a new target without changing other call sites of the same function.
- `CodeInjection`: Patch helper for inserting custom code at a specific address or
  instruction range, often directly referencing memory registers. Use for more
  involved patches that require multiple instructions or logic near the original site.
- `AsmWriter`: Low-level machine-code writer used by hook/injection helpers. Use when
  you need fine-grained control over emitted instructions or to build custom patches.
