# Code Style

## Language and build

- C11 (`-std=gnu11`), compiled with `-Wall -Wextra`. Keep the build warning-free — treat any new warning as a bug to fix before moving on, not something to suppress.
- No third-party dependencies, with one exception: `src/http.c` links libcurl (`-lcurl`) for HTTPS/TLS, since hand-rolling TLS is not something to do in-house. It's the only third-party dependency in the project, confined to that one file. The native build links it dynamically against the system's libcurl, never bundling/vendoring its source; the Windows cross-build statically links it (`mingw64-curl-static`), matching how it already statically links everything else there to ship one self-contained `.exe`. Don't reach for another dependency without the same justification (a capability libc genuinely can't provide).
- Otherwise: only libc, pthreads, and (on Windows) the Win32 API.
- Platform-specific code (`#ifdef _WIN32`) stays isolated inside `src/render/term.c`. No other file should need an `#ifdef` for OS differences — if a new platform-specific need shows up, add the primitive to `term.h`/`term.c` rather than spreading `#ifdef`s around.

## Naming

- Public functions: `clay_<module>_<verb>`, e.g. `clay_task_start`, `clay_below_set_text`, `clay_prompt_choice`.
- Types: `Clay` + PascalCase, e.g. `ClayTask`, `ClayBelowState`, `ClayModelSelection`.
- Macros/constants: `CLAY_SCREAMING_CASE`, e.g. `CLAY_ORANGE`, `CLAY_ICON_CHECK`.
- One header per concern, named after it (`term.h`, `box.h`, `task.h`, `model_select.h`, ...). `clay.h` is the umbrella that pulls in every leaf header; `mm.h` does the same for the four `mm` headers. Don't add content directly to an umbrella header — it only `#include`s.
- File name matches the module/widget it implements, not the feature request that produced it.

## Comments

- English, technical, to the point. State the non-obvious fact or constraint, not the reasoning essay behind it.
- Default to no comment. Add one only when the code alone would mislead or hide a real gotcha (e.g. why a cursor-down escape can't be used here, why a lock is held across this call).
- No decorative separators (`---- SECTION ----`, `==== SECTION ====`). No em dashes in comments. No multi-paragraph explanations — if a comment needs more than ~3-4 lines, the design is probably too subtle and should be simplified instead.
- Don't restate what the code obviously does. Don't reference the task/request/issue that led to the change — that belongs in the commit message, not the source.

## Memory

- No fixed-size buffers for data whose length isn't structurally bounded (user input, labels, ids, model names, filter text, task results). Use `ClayStr` (growable string), `ClayArray` (growable typed array), or `ClayMap` (string-keyed hash map) from `src/mm/`.
- A fixed-size array or buffer is fine only when the bound is a real structural constant, not a guess about data size — e.g. `CLAY_MODEL_VISIBLE_ROWS` (a screen budget), `CLAY_BELOW_MAX_MODULES` (a sane ceiling on a small in-process registry), or `PATH_MAX` (an actual OS limit). If you're tempted to write `char buf[256]` for something that holds caller-supplied text, use `ClayStr` instead.
- Ownership is explicit: a function that returns a `malloc`'d/`ClayStr`-backed value documents "caller frees" in its header comment, and pairs with a `_free`/`_destroy` counterpart (`clay_str_free`, `clay_model_selection_free`, `clay_app_destroy`, ...).
- `clay_str_vprintf`/`clay_str_printf` are the way to build a dynamic string from a format string; don't hand-roll a `vsnprintf`-into-fixed-buffer pattern.

## Color and terminal output

- Never write a raw ANSI escape inline outside `color.h`. Reference the named constant (`CLAY_ORANGE`, `CLAY_GRAY`, ...) and always wrap it with `clay_color(...)` before printing — that's the single point that respects `NO_COLOR` and `--no-color`. A color constant used unwrapped is a bug.
- Icons live in `color.h` too (`CLAY_ICON_CHECK`, `CLAY_ICON_SLEEP`, ...), as UTF-8 string literals.
- `clay_utf8_width` is ANSI-aware (skips escape sequences) — use it, not `strlen`, whenever you need the on-screen width of text that might carry color codes.

## Multi-row interactive widgets

- Any live-redrawn, multi-row terminal region (a prompt with modules below it, a scrollable list, tabs) must use `clay_term_row_enter(row, &established)` to move between rows during a redraw, not a bare `'\n'` or `clay_term_cursor_down`. Rows that already exist need a plain cursor move (never scrolls); rows being created for the first time need a real `'\n'` (scrolls the viewport if pinned at the bottom). Mixing these up is the single most common bug in this codebase — it makes a widget "walk" up the screen and duplicate itself when the terminal is full. `below.c`, the `choice` widget in `prompt.c`, and `model_select.c` are the reference implementations.
- Every interactive widget checks `clay_term_is_interactive()` and provides a plain line-based fallback for non-tty stdin (piped/scripted input). Don't add a new interactive prompt without one.

## General

- No premature abstraction. Three similar lines beat a speculative helper. Extract a shared primitive only once the same non-trivial logic is genuinely needed in more than one place (see `clay_term_row_enter`).
- No defensive error handling for states that can't happen internally. Validate only at real boundaries (user input, external data).
- Don't add a feature flag or backwards-compat shim — just change the code.
