# Architecture

clay is a minimalist C render engine and CLI toolkit for building a Claude Code-style terminal agent. Right now it's just the render/UI layer plus a demo driver in `main.c` — there is no agent/model backend yet. The layering exists specifically so that backend can be dropped in later without touching the rendering code.

## Layers

```
src/mm/            growable primitives: arena, str, array, map
src/render/         drawing primitives, built on mm + term
src/render/modals/  purpose-built composite interactive widgets, built on render
src/commands/       command registry + app state machine
src/main.c          demo driver: wires commands to the app and render engine
```

Each layer only depends on the layers listed above it. `mm` depends on nothing else in the project. `render` depends on `mm` and `term`. `commands` depends on `render` (it calls render functions, never the other way around). `main.c` is the only file that knows about all of them at once.

### `src/mm/` — memory

- `arena.c` — bump allocator for scoped batch allocations.
- `str.c` (`ClayStr`) — growable string buffer; the default choice for anything of unknown length.
- `array.c` (`ClayArray`) — growable, type-erased dynamic array (doubles on growth).
- `map.c` (`ClayMap`) — string-keyed hash map, separate chaining, used for the command registry's name → handler lookup.

These have no knowledge of terminals or rendering. They exist so nothing above this layer needs a fixed-size buffer for caller-supplied data.

### `src/render/` — drawing primitives

- `term.c` — the only platform-aware file. Cursor movement, raw mode, key reading (`ClayKey`), color enable/disable (`clay_color`, `NO_COLOR` handling), UTF-8-aware width, OSC 8 hyperlinks, and `clay_term_row_enter` (the shared safe-redraw primitive, see CODESTYLE.md).
- `box.c` — bordered boxes with per-line text/border colors.
- `banner.c` — the startup banner, built on `box.c`.
- `list.c` — the `◆ clay` response prefix (`clay_say`/`clay_sayc`), plan/list rendering (`clay_list_step`), bullets.
- `task.c` — spinner-driven task lines (`clay_task_start/success/fail`), each on its own background thread that animates until stopped.
- `prompt.c` — the main `>` input line (history-aware, arrow-key recall) and the two general-purpose interactive pickers: `clay_prompt_select` (horizontal, left/right, no free text) and `clay_prompt_choice` (vertical, up/down, optional free-text fallback row).
- `below.c` — the status-module system: named modules registered via `clay_below_add`, rendered inline on one row under the prompt (`Model: ... · Tokens: ... · Provider: ...`), each with an optional icon state (`NONE`/`LOADING`/`FINISHED`/`IDLE`). Owns a background animator thread that only redraws while `clay_below_set_editing(1)` is active, so it never clobbers unrelated output. `clay_prompt_line`'s interactive path is the only caller of `clay_below_render`/`clay_below_finish`.

### `src/render/modals/` — composite widgets

Purpose-built interactive widgets that combine several `render/` primitives into one specific UI, when a need doesn't fit `select`/`choice`. Not meant to be generic — each file is its own thing.

- `model_select.c` — tabs (provider, left/right) over a filterable, scrollable list (model, up/down, max 6 visible with "N more above/below" hints). Each provider supplies a `fetch(ctx, out, max)` callback so its model list is queried lazily, only when its tab becomes active.

### `src/commands/` — registry and state machine

- `command.c` — parses one line of input into a command (`/name args...`, dispatched by name via `ClayMap`) or a plain message (`ClayInput`), and holds the registry of registered command handlers.
- `app.c` — `ClayApp`: a small state machine (`IDLE` / `BUSY` / `PROMPTING` / `EXITING`) with a listener hook. Command handlers never call `render/` functions directly — they call `clay_app_say`, `clay_app_task_start`, `clay_app_select`, etc., which update `ClayApp`'s state *and then* delegate to the matching render function. This keeps "what's happening" (state) and "how it looks" (ANSI output) in sync by construction, and is the seam a future agent daemon would drive through instead of writing to the terminal directly.

### `src/main.c`

The demo/test driver. Registers commands (`/help`, `/exit`, `/confirm`, `/select`, `/choice`, `/below`, `/model`, `/mm`), registers the below-prompt status modules (`model`, `tokens`, `provider`), and runs the main read-parse-dispatch loop. `run_demo_turn` is a canned sequence (scan → plan → write → test) that exercises the render engine end to end; it doesn't inspect the actual message text yet, since there's no agent behind it.

## Key design decisions

**Interactive vs. fallback duality.** Every interactive widget (`clay_prompt_line`, `clay_prompt_select`, `clay_prompt_choice`, `clay_model_select`) checks `clay_term_is_interactive()` first. On a real tty it uses raw mode and arrow keys; otherwise it falls back to buffered line reads (numbers for choices, plain text for input). This is what keeps the binary usable when piped or scripted, and is exercised by feeding it through a pipe rather than a pty.

**The row-tracking primitive.** Any widget that redraws more than one line in place (`below.c`'s status block, the `choice` picker, `model_select`) shares the same mechanism: track a resting cursor position between redraws, use `clay_term_cursor_down` to revisit rows that already exist (never scrolls) and a real `'\n'` only the first time a row is created (scrolls if needed). Getting this wrong is the main historical bug class in this codebase — it makes a widget visually "walk" up the screen and duplicate itself once the terminal is full, because a pure cursor-down escape can't scroll into new territory but a `'\n'` always can. `clay_term_row_enter` centralizes the decision.

**Color is a single switch.** `clay_color(CODE)` returns `CODE` or `""` depending on one process-wide flag, initialized from `NO_COLOR` and overridable via `--no-color`. Every ANSI constant is routed through it, so no code path can leak color when it's supposed to be off.

**Ownership follows the codebase's `_create`/`_destroy` and `_init`/`_free` convention throughout** — `ClayApp`, `ClayCommandRegistry`, `ClayStr`, `ClayTask`, `ClayModelSelection` all pair a constructor with an explicit teardown; nothing relies on process exit to clean up.

## Build

`Makefile` builds natively (`make build`, objects in `build/`, binary in `bin/clay`) and cross-compiles for Windows via mingw-w64 (`make build-win`, `build-win/`, `bin-win/clay.exe`, statically linked). Both share the same `src/` tree; only `term.c` branches on `_WIN32`. `make run` builds and runs the native binary.
