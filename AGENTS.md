# Agent guide for Alpine Faction

This file is intended to help Codex, Claude, and other assistants (and other automated tools) work in this repository efficiently.

## Basic information
- Alpine Faction is a mod/patch for the video game Red Faction and its level editor.
- Original source code for the game and editor is not available.
- Alpine Faction's core files are a launcher (`AlpineFactionLauncher.exe`) and 2 dll files (`AlpineFaction.dll`), (`AlpineEditor.dll`)
- The launcher injects the new code from the dll files into the stock Red Faction processes (`rf.exe`), (`RED.exe`)
- Everything targets 32-bit Windows (x86), because the dlls run inside the stock 32-bit processes.

## Repository overview
- `common/`, `patch_common/`: Shared components.
- `game_patch/`: Core C++ code for the game patch.
- `editor_patch/`: Core C++ code for the level editor patch.
- `launcher/`, `launcher_common/`: Core C++ code for the launcher.
- `crash_handler/`: Core C++ code for the crash handler.
- `crash_handler_stub/`: In-process crash hook and watchdog timer that hand off to the crash handler.
- `xlog/`: Logging library.
- `vendor/`: Third-party libraries.
- `cmake/`, `CMakeLists.txt`: Build system and configuration.
- `docs/`: Documentation (see `docs/BUILDING.md` for build instructions).
- `resources/`: Assets and licensing information.
- `setup/`: Installer script.
- `tools/`, `mesh_patches/`: Supporting tooling.

## Build and run
- Start with the build guide: `docs/BUILDING.md`.
- Prefer out-of-source CMake builds.
- If you add new targets or dependencies, update `docs/BUILDING.md` as needed.

## Testing and validation
- Preferred baseline check (Windows/MSVC):
  1. Open an "x86 Native Tools Command Prompt for VS".
  2. Configure an out-of-source build:
     - `cmake -S . -B build-msvc -G "Visual Studio 17 2022" -A Win32`
  3. Build the default configuration:
     - `cmake --build build-msvc --config Release`
- If you cannot build in the environment, document that in your final response
  and mention the recommended MSVC steps above.
- Changes should always be rigorously reviewed before a merge is considered:
  - As surgical as possible to achieve the goal
  - Effective, necessary, safe, and properly/robustly written
  - Consolidates code where reasonable using `common/`
  - Reuses existing shared logic where possible (such as the utilities in `common/utils`)
  - Adheres to existing codebase style convention and `STYLE.md`
  - No unexpected regressions affecting existing behaviour

## Change guidelines
- Keep changes minimal and focused on the requested task.
- Match existing formatting and naming in nearby code. Never run clang-format over whole files;
  format only the lines you touch.
- Never guess game addresses, offsets, or struct layouts. Take them from existing code or verify
  them against the binary.
- Update documentation when behavior or usage changes.
- For any changes players, server hosts, or level designers will notice, add an entry to
  `docs/CHANGELOG.md` under the unreleased version (see `CONTRIBUTING.md`).
- When adding, removing, or changing console commands, command line switches, or dedicated server config options, update the matching file in `docs/reference/`.
- Do not prepare large or cross-cutting PRs unless the user confirms the change was discussed
  with the maintainers first (see `CONTRIBUTING.md`).
- PR descriptions: a few sentences on what changed, why, and how it was tested. No
  file-by-file changelogs and no claims that weren't verified.

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
