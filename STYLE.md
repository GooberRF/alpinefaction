# Alpine Faction code style

In all cases where an instance is not addressed below, endevour to adhere to the spirit of what's written
here as closely as possible.

## Formatting
- `.clang-format` is authoritative for layout: 4 spaces, 120 columns, `PointerAlignment: Left`, and a
  brace on its own line after functions, structs, classes, enums and namespaces. `else`, `catch` and
  `while` also go on their own line after `}`.
- Never run clang-format over a whole file. `SortIncludes` would reorder `windows.h` and `commctrl.h`,
  which can break the build. Format only the lines you touch.
- An `if` body goes on its own line. A bare guard like `if (!x) return;` is tolerated (it is common on
  master), but never put `if (...) a; else b;` on one line.
- Only an empty function body may sit on one line (`AllowShortFunctionsOnASingleLine: Empty`).
- Wrap long conditions with the operator at the start of the continuation line (master's majority,
  1738 to 301). Keep each file consistent.
- Within a file, put `case` labels either all flush with `switch` or all indented.
- HLSL puts `} else` on the same line (master's majority), unlike C++.

## Engine interop: game (RF.exe) and editor (RED.exe)
- **Engine struct fields are typed members**, each with an offset comment and a `static_assert`:
  `static_assert(offsetof(CDedLevel, dialog_panels) == 0x444);`.
  - Game: `game_patch/rf/*.h`. Example: `rf::GRoom` in `rf/geometry.h`, locked down with
    `sizeof`/`offsetof` asserts.
  - Editor: `editor_patch/level.h` (`CDedLevel`, `GRoom`, `GSolid`, `BrushNode`) and
    `editor_patch/mfc_types.h` (`DedObject` subclasses such as `DedBoltEmitter`).
  - New code never reads engine memory through `struct_field_ref(p, 0x..)`, `ptr + 0x..` casts or a
    `constexpr ..._offset` constant. Split the padding and add a typed field instead. The `sizeof`
    assert guards the split.
- **Engine globals are declared once**, as `static auto& name = addr_as_ref<T>(0x...);`, in an rf/
  header (game) or in `vtypes.h` (editor). Never declare the same address twice. Reuse what exists.
  Example: `rf/geometry.h` around `g_num_geomods_this_level`. Master's `destruction.cpp` has no local
  `addr_as_ref` at all.
- **Engine functions are declared once**, either as `static auto& fn = addr_as_ref<sig>(0x...)` in a
  header, or as a struct method that wraps `AddrCaller{0x...}`. Examples: `CDedLevel` methods,
  `GSolid::destroy`, `BrushNode::create` in `level.h`. Feature code does not scatter raw
  `AddrCaller{0x...}` calls.
- **Engine bit fields and indices use named enums**: `rf::GFaceFlags`, `FACE_LIST_SOLID`,
  `FACE_LIST_BBOX`, `DIK_*`. A missing flag gets an enumerator; never a literal.
- **Before adding a helper, check whether one exists.** Examples: `WndToHandle`,
  `GRoom::face_list_head`, `bm_create`, `room_cleanup`, `GSolid::face_list_head`.

## Hooks
- Use only `FunHook`, `CallHook`, `CodeInjection`, `AsmWriter` or `write_mem`. No naked asm stubs.
- Declaration pattern: a forward-declared function, then `FunHook<decltype(fn)> fn_hook{0x..., fn};`,
  then the definition. Example: any hook in `editor_patch/level.cpp`.
- A lambda-bound `__fastcall` hook needs `FASTCALL_LAMBDA`; a `__stdcall` one needs `STDCALL_LAMBDA`.
- A hook site must be the start of an instruction, span at least 5 bytes, contain no relative
  `CALL`/`Jcc`, and not be a branch target. Say in a comment why a trampoline-less
  (`false`) injection is safe.
- Prefer a stateless condition to a global flag that says "we are inside hook X".
- Verify every address and calling convention before first use.

## Naming and organisation
- Functions, variables and constants are `snake_case`; types are `PascalCase`. Mutable file or global
  state takes a `g_` prefix. UPPER_CASE is only for lists that already use it (`DIK_*`, engine
  macros). No single-letter constant names.
- File-local code goes in an anonymous namespace. It is the only way to make file-local types
  local, so a same-named struct in two files can't break the one-definition rule. Older files that
  use `static` functions may keep them, but new types go in the anonymous namespace. Keep one block
  per file where practical, and don't reopen it for a single function.
- No `using namespace` at file scope. Use an alias: `namespace at = alpine_terrain;`.
- Includes: a sibling header uses `"name.h"`; any other directory uses the rooted form
  `<common/terrain/alpine_terrain.h>`. Include what you use.
- `common/` is for logic that is genuinely identical in the game and the editor. Engine glue stays per
  binary. `common/` uses bare `uintN_t` (not `std::uintN_t`) and names every header in its
  `CMakeLists.txt` `SRCS`.
- Console commands take a category prefix (`cl_`, `r_`, `ui_`, `sv_`, `dbg_`, `d_`, ...) and always
  call `.register_cmd()`.
- D3D11 subsystems are classes owned by `Renderer`, with `ComPtr` members, constructed from the device
  and `ShaderManager`. Example: `CausticsRenderer`. Shader slots are named constants. Constant-buffer
  mirror structs use `std::array` and carry `offsetof` asserts.

## Comments and logging
- Keep comments sparse and purposeful. Say why, not what. Engine-fact narratives go in
  `docs/private`.
- Never reference `docs/private`, planning docs, scratch paths or the gitignored `research/` from code.
- A `// ─── Section ───` banner is fine for long files.
- Log with `xlog` and a `[Tag]` prefix. Editor messages the user must see go through
  `editor_report` / `editor_report_blocking`.
- Don't ship debug scaffolding (verify harnesses, timing dumps, kill-switch cvars) unless it has a
  `dbg_` name and a CHANGELOG line.

## File formats (RFL chunks)
- Alpine chunk ids come from the `0x0AFBAE0x` series. Level-properties items are append-only.
- A chunk re-serialized from memory on every save grows by appending fields gated on the RFL
  version. A section that RED can re-emit byte for byte (Alpine Lightmaps) grows only through its own
  version fields, never through the RFL version.
- Tags, enums and ids on the wire are append-only. Never reuse a value.
- A value the reader recomputes and compares with a stored one (a fingerprint or hash) is part of the
  format. Pin it with a golden `static_assert`, and change its output only for non-default new
  inputs.
- Reader caps (counts, sizes) can rise freely. Wire field widths cannot.
- Never extend the stock savegame format.

## Housekeeping
- Every `.cpp`, and every header in `common/`, is listed in its `CMakeLists.txt`.
- New vendored libraries follow the existing pattern: a plain directory, `add_subdirectory`, a
  `LICENSE` file, an entry in `resources/licensing-info.txt`, and the CHANGELOG `Imported libraries`
  section.

## Changelog
- One line per change, in the version's `Major features`, `Minor features, changes, and enhancements`
  or `Bug fixes` section, under the author's `[@name](https://github.com/name)` line. For joint work,
  join the authors' links with `&`.
- Start with a verb in the imperative: `Add`, `Make`, `Allow`, `Speed up`, `Remove`. A bug fix starts
  with `Fix`. No trailing full stop.
- Name the user-visible effect, not how it was done. Keep it to one short sentence. Leave out
  exhaustive lists, edge cases, defaults and ranges; those belong in the docs in `docs/reference`.
- Put console commands, options, settings, events and file names in backticks, spelled exactly as
  the player or mapper sees them. Say `level editor` for changes to RED.
- Mark a change limited to one renderer with `(Direct3D 11 renderer only)` at the end.
- Indent sub-bullets two spaces, and use them only for the parts of a major feature, not to qualify a
  line.
- Changes to features that haven't been released yet get no line.
- Examples:
  "- Add `ui_color_console` console command to set the console background color"
  "- Fix lighting for pistol silencer and remote charge detonator"
