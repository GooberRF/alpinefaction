# Contributing

Thanks for contributing to Alpine Faction! A few basics:

- See [AGENTS.md](AGENTS.md) for a repo overview (it's written for AI tools but
  works for people too) and [docs/BUILDING.md](docs/BUILDING.md) for build instructions.
- Follow [STYLE.md](STYLE.md) and match the code around your change. Don't run
  clang-format over whole files; format only the lines you touch.
- Keep pull requests focused on one feature or fix.
- Build it and test it in game (or in the editor) before opening the PR. Test
  the situations your change affects, not just the one you were targeting:
  for example as a client, listen server host, and dedicated server for
  multiplayer changes, or across save/load for single player changes.
- For anything players, server hosts, or level designers will notice, add an
  entry to [docs/CHANGELOG.md](docs/CHANGELOG.md) under the unreleased version,
  in the right section and under your GitHub handle.
- If you add, remove, or change a console command, command line switch, or
  dedicated server config option, update the matching file in
  [docs/reference/](docs/reference/).

## Before you start

Alpine Faction is under active development and a lot of areas are in flux.
For anything beyond a small, self-contained fix, please open an issue or
talk to us on [Discord](https://discord.gg/factionfiles) before writing a
lot of code. Large unannounced PRs (new systems, broad refactors, sweeping
cleanups) are hard to review, often conflict with work in progress, and may
be closed even if the code is good.

## What we look for in review

These are the standards every change is held to, including the maintainers'
own:

- **Focused.** The change is as small as it can be while still doing the
  job properly. Unrelated cleanups, renames, and reformatting belong in a
  separate PR.
- **Correct and robust.** It does what it says, handles edge cases and
  failures sensibly, and doesn't add crashes, leaks, or ways for a malicious
  client or server to cause trouble. Data from the network or from files is
  validated before it's used.
- **No regressions.** Existing behaviour keeps working: stock and custom
  levels and mods, levels made with older versions of the level editor, and
  players and servers on other Alpine Faction versions (to the greatest
  practical extent).
- **Reuses what exists.** Check `common/` (including `common/utils`) and
  nearby code for an existing helper before writing a new one. Logic that's
  identical in the game and the editor goes in `common/`. If you add a shared
  helper, move existing copies of that logic onto it in the same PR.
- **Engine code done the established way.** Engine structs, globals, and
  functions are declared once, with typed fields and offset asserts. Hooks
  use `FunHook`, `CallHook`, `CodeInjection`, or `AsmWriter`, and every
  address is verified. [STYLE.md](STYLE.md) has further details.
- **Compatible formats and protocol.** Level file chunks and network packet
  types, ids, and enums are append-only, and values are never reused. In
  general, extend an existing packet before adding a new one.
- **Readable.** It follows [STYLE.md](STYLE.md). Comments are sparse and say
  why, not what. No leftover debug code or logging.
- **Documented.** It has a changelog entry written as described in
  [STYLE.md](STYLE.md#changelog), and updates `docs/reference/` where needed.

It's normal for a PR to go through a few rounds of review comments before
it's merged. That isn't a judgment of you or your work; every change goes
through the same process.

## Using AI tools

You're welcome to use AI tools; several of us do. Where the code came from
doesn't matter (as long as you have the right to submit it, see Licensing
below). AI-assisted changes are reviewed against the same standards as
everything else (see above), and you're responsible for what you submit:

- Understand the change and be able to explain it in review. "The AI wrote
  it" isn't an answer to a review question.
- Keep the diff to what the PR is about: no unrelated reformatting,
  renames, or drive-by "improvements".
- Verify any game addresses, offsets, or struct layouts against the binary.
  AI tools routinely invent plausible-looking ones.
- Keep the PR description short and written for a human: what changed,
  why, and how you tested it. Don't include claims you haven't checked.

## Licensing

Alpine Faction is licensed under the [Mozilla Public License 2.0](LICENSE.txt).
By submitting a pull request, you agree that your contribution is provided
under the same license, and you confirm that you have the right to submit it
(e.g. it's your own work, or you otherwise have permission to contribute it).
