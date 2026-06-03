# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

This is Simon Tatham's Portable Puzzle Collection: ~40 single-player puzzle games sharing one portable C core, with platform-specific front ends (GTK, Windows, macOS, WebAssembly, Java/NestedVM, KaiOS).

## Build

Built with CMake. Simplest in-tree build (binaries land in the source dir):

```
cmake .
cmake --build .
```

Out-of-tree is preferred for real work:

```
cmake -B build && cmake --build build
```

The build auto-detects platform via `cmake/setup.cmake` and includes one of `cmake/platforms/{unix,windows,osx,emscripten,nestedvm}.cmake`. On Unix this needs GTK (2 or 3) via pkg-config; `halibut` is needed only to build docs.

Useful options (pass as `-DNAME=value`):
- `STRICT=ON` — adds `-Wall -Wwrite-strings -Wmissing-prototypes -std=c99 -pedantic -Werror`. Match this before submitting; the project builds clean under it.
- `PUZZLES_ENABLE_UNFINISHED="name;name"` — build games from `unfinished/` as if official.
- `BUILD_SDL_PROGRAMS=ON` — build SDL-based test programs.

Note: assertions are deliberately kept **on** even in Release/MinSizeRel builds (see `cmake/setup.cmake`) — they guard against memory corruption, so don't write code that relies on `NDEBUG`.

Each puzzle builds to its own executable (e.g. `./net`, `./solo`). The `puzzle()`, `solver()`, `cliprogram()`, and `guiprogram()` CMake functions are defined in `cmake/setup.cmake`; each game and its extra tools are declared in the top-level `CMakeLists.txt`.

## Architecture

The code is split into **four parts** (full detail in `devel.but`, the developer guide; user manual is `puzzles.but` — both are Halibut source):

- **Front end** — the non-portable part. Owns `main()` and the GUI; one file per platform: `gtk.c`, `windows.c`, `osx.m`, `emcc.c` (+ `emcclib.js`/`emccpre.js`), `nestedvm.c`. Implements the drawing API that games call.
- **Back end** — a "back end" *is* a puzzle. Each game is a single self-contained `.c` file (`net.c`, `solo.c`, …) exporting one `const struct game` (named `thegame`, or game-specific under `COMBINED` builds). The struct (defined in `puzzles.h`) is a large vtable of function pointers plus a few constant flags.
- **Middle end** (`midend.c`) — single, universal, platform- and game-independent. Sits between front and back ends. Handles undo/redo (it owns the list of `game_state`s), timers/animation, game-ID parsing, serialisation, standard keystrokes/menu commands, and mouse-event normalisation. **Do not edit `midend.c` to add a game** — coordinate with upstream before any mid-end change.
- **Utilities** — shared helpers usable everywhere (see below).

### The game vtable and its four data structures

A back end keeps persistent state in four structs (each game defines its own concrete versions). Putting data in the *right* one is essential or midend features break:

- `game_params` — game settings/difficulty (what a preset selects).
- `game_state` — the actual puzzle position. The midend keeps a list of these for undo/redo, so a `game_state` must be a complete, dup-able snapshot.
- `game_ui` — transient UI state *not* part of the undo history (e.g. cursor position, current drag). Usually not serialised; if you store anything persistent here you must implement and test `encode_ui`/`decode_ui`.
- `game_drawstate` — cached drawing state for incremental redraw; never affects gameplay.

### The interpret_move / execute_move split (important)

Moves go through two functions, never one:
- `interpret_move(state, ui, ds, x, y, button)` turns a user input into a short **move string**, or updates only the `ui`.
- `execute_move(state, move)` parses that move string and returns a new `game_state`.

Because the same string format is used during normal play, serialisation/undo/save are exercised constantly — so if the game is playable at all, save/load is very likely correct. Preserve this split when adding games. Game IDs and move strings are plain text; `devel.but` (`writing-textformats`) covers format design.

### Shared utility modules

Prefer these over reinventing: `random.c` (portable RNG + SHA), `malloc.c` (`smalloc`/`srealloc`/`sfree` and the `snew`/`snewn`/`sresize` macros — use these, not raw malloc), `tree234.c` (balanced 2-3-4 tree), `dsf.c` (disjoint-set forest, incl. flip/min variants), `findloop.c` (loop/bridge detection), `grid.c`/`grid.h` (polygon grids for loopy etc.), `latin.c` (Latin-square solver core, shared by keen/towers/unequal), `loopgen.c`, `divvy.c` (region division), `matching.c` (bipartite matching), `combi.c`, `laydomino.c`, `drawing.c` + `ps.c` (drawing/printing abstraction), `printing.c`. Aperiodic tilings: `penrose.c`, `hat.c`, `spectre.c` (with generated `*-tables*.h`). All exported in `puzzles.h`.

## Testing

The collection is **self-testing by design** — there is no `ctest` harness. Tests are run via the binaries themselves:

- **Generation/solve sweep** (the main correctness test for a game): every game binary supports CLI modes parsed in `gtk.c`'s `main`:
  - `./<game> --generate <n> [<params>]` — generate `n` game IDs (optionally for a given parameter string) to stdout.
  - `./<game> --test-solve` — exercise the solver on generated/stdin games.
  - `./<game> --list-presets` — list preset parameter strings.
  - `benchmark.sh` (run from the build dir; reads `gamelist.txt`) runs `--test-solve --time-generation --generate 100` over every preset of every game; `benchmark.pl` formats the output.
- **Standalone solvers**: games with `solver(...)` in CMakeLists build a `<game>solver` binary (compiled with `-DSTANDALONE_SOLVER`), typically invoked as `./<game>solver [-v|-g] <game_id>`. Other `-DSTANDALONE_*` modes exist (e.g. `STANDALONE_PICTURE_GENERATOR`, `STANDALONE_OBFUSCATOR`, `STANDALONE_SOAK_TEST`).
- **Unit-test programs** in `auxiliary/` for the utility modules: `tree234-test`, `sort-test`, `latin-test`, `findloop-test`, `hat-test`, `spectre-test`, `penrose-test`, `divvy-test`, `combi-test`.
- **Fuzzing**: `fuzzpuzz.c` (built when CLI programs are enabled); supports Clang libFuzzer via `-DWITH_LIBFUZZER=ON`.

When changing a game, also manually test undo/redo of every move type (animations on undo are a common breakage) and, if `game_ui` holds persistent state, save/load.

## Adding a new puzzle

Follow `CHECKLST.txt`. In brief: copy `nullgame.c` (the template — search for `FIXME` markers to remove), **rename its `thegame` struct/`#define`** away from `nullgame`, add a `puzzle()` (and optional `solver()`) stanza to `CMakeLists.txt`, document it in `puzzles.but` with a Windows help topic, write `html/<name>.html`, add binary names to `.gitignore`, and create an icon save file under `icons/`.

## Conventions

- C99; single-file games; static helpers within each game file.
- Memory: always `smalloc`/`srealloc`/`sfree` and the `snew*`/`sresize` macros from `malloc.c`.
- Commit messages use a `Gamename: imperative summary.` prefix when game-specific (e.g. `Mosaic: fix some memory leaks in new_game_desc.`), plain imperative otherwise.
