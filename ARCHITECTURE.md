# Architecture

clay is a minimalist C render engine and CLI toolkit for building a Claude Code-style terminal agent. It's the render/UI layer plus a demo driver in `main.c`, and a separate backend layer (`json.c`, `http.c`, `providers/`, `config.c`) for talking to LLM APIs and remembering how to reach them. `/connect` saves provider credentials; `/model` uses those live connections to retrieve and select their available models.

## Layers

```
src/mm/            growable primitives: arena, str, array, map
src/json.c          minimal JSON value tree: parse, build, stringify
src/http.c          HTTP client, built on mm + libcurl
src/providers/      LLM provider clients, built on http + json (openai.c, ...)
src/config.c         saved provider credentials and model selection (~/.clay), built on mm + json + term
src/render/         drawing primitives, built on mm + term
src/render/modals/  purpose-built composite interactive widgets, built on render
src/commands/       command registry + app state machine
src/main.c          demo driver: wires commands to the app and render engine
src/test_openai.c   standalone harness for the OpenAI provider (see "Backend")
```

Each layer only depends on the layers listed above it. `mm` depends on nothing else in the project. `json` depends only on `mm`. `http` depends on `mm` and libcurl, not on `json` - it's a generic transport, agnostic of what's riding on it. `providers/` depends on `mm`, `json`, and `http`. `config` depends on `mm`, `json`, and `term` (for the home directory and file permissions - see below), not on `http`/`providers` - it only persists connection and selection data. `render` depends on `mm` and `term`. `commands` depends on `render` and, as of `/connect`, on `config`. `main.c` is the integration point for the render/commands/config/provider stack; `test_openai.c` is a separate provider/http harness.

### `src/mm/` — memory

- `arena.c` — bump allocator for scoped batch allocations.
- `str.c` (`ClayStr`) — growable string buffer; the default choice for anything of unknown length.
- `array.c` (`ClayArray`) — growable, type-erased dynamic array (doubles on growth), with `clay_array_insert`/`clay_array_remove` for mid-array edits.
- `map.c` (`ClayMap`) — string-keyed hash map, separate chaining, used for the command registry's name → handler lookup.

These have no knowledge of terminals or rendering. They exist so nothing above this layer needs a fixed-size buffer for caller-supplied data.

### `src/json.c` — JSON

An opaque `ClayJson` value tree (null/bool/number/string/array/object), built on `ClayStr`/`ClayArray`. Constructors (`clay_json_object`, `clay_json_string`, ...) and mutators (`clay_json_object_set`, `clay_json_array_push`) take ownership of what's passed in; `clay_json_clone` deep-copies a value the caller wants to keep borrowing (e.g. a tool schema reused across several requests) into an independently-owned tree. `clay_json_parse` is a plain recursive-descent parser; `clay_json_stringify` serializes compact JSON. No schema validation, no comments/trailing-comma leniency - it only needs to round-trip what the providers below send and receive.

### `src/http.c` — HTTP client

A thin wrapper over libcurl's easy interface - the only place in the project that links a third-party library, and only dynamically (`-lcurl`, never bundled/static; see CODESTYLE.md). `clay_http_request` runs synchronously on the calling thread; a streaming response is read via a caller-supplied `on_chunk` callback (raw bytes, not line-buffered) instead of being accumulated whole, since a chat completion can stay open for a while. Callers needing whole lines (SSE) do their own buffering - see `providers/openai.c`.

### `src/providers/` — LLM provider clients

- `openai.c` — talks to any OpenAI-compatible endpoint: `GET /models` returns every model id for the connected account, while `/chat/completions` handles streaming responses (SSE, parsed line-by-line as chunks arrive in `on_http_chunk`, since a chunk boundary can land mid-line), final input/output token usage when supplied by the provider, JSON tool calls (accumulated across streamed deltas by their `index`, since the API sends id/name/argument-fragments as separate events), and the tool-call loop (`clay_openai_run` appends the assistant's tool-call message and each tool's result to `messages`, then resends the conversation, up to `max_rounds`, until the model answers with plain content and no further tool calls).

`src/test_openai.c` is a standalone binary (`bin/test_openai`, the Makefile's `test-openai` target) that exercises streaming and tool calls against a real endpoint. It reads `OPENAI_BASE_URL`/`OPENAI_API_KEY`/`OPENAI_MODEL` from the environment so a token never ends up in argv or committed code. It's excluded from the main `clay` binary (both define `main`).

### `src/config.c` — saved provider credentials

Persists a `ClayProviderConfig {id, apikey, base_url}` per provider as `~/.clay/providers/<id>.json`, restricted to the owner (`clay_term_restrict_file`, POSIX 0600) since it holds a plaintext API key. `~/.clay/config.json` separately stores the selected `provider` and `model` (or JSON null when unset). Doesn't know about provider *types* (openai vs. openrouter vs. a custom endpoint) - that mapping lives in `main.c`'s `PROVIDER_TYPES` table, since they're all the same OpenAI-compatible wire format and only differ by default `base_url`. `id` doubles as both the config filename and the provider type it was connected as.

### `src/render/` — drawing primitives

- `term.c` — the only platform-aware file. Cursor movement, raw mode, key reading (`ClayKey`), color enable/disable (`clay_color`, `NO_COLOR` handling), UTF-8-aware width, OSC 8 hyperlinks, `clay_term_row_enter` (the shared safe-redraw primitive, see CODESTYLE.md), and the small OS-specific primitives other layers need (`clay_term_home_dir`, `clay_term_mkdir`, `clay_term_restrict_file`) rather than letting them spread their own `#ifdef _WIN32`.
- `box.c` — bordered boxes with per-line text/border colors.
- `banner.c` — the startup banner, built on `box.c`.
- `list.c` — the `◆ clay` response prefix (`clay_say`/`clay_sayc`), incremental streamed assistant replies (`clay_response_begin`/`write`/`end`), plan/list rendering (`clay_list_step`), bullets.
- `task.c` — spinner-driven task lines (`clay_task_start/success/fail`), each on its own background thread that animates until stopped.
- `prompt.c` — the main `>` input line (history-aware, arrow-key recall, paste detection) and the general-purpose interactive prompts: `clay_prompt_select` (horizontal, left/right, no free text), `clay_prompt_choice` (vertical, up/down, optional free-text fallback row), and `clay_prompt_secret` (masked, for API keys).
- `below.c` — the status-module system: named modules registered via `clay_below_add`, rendered inline on one row under the prompt (`Idle · Model: ... · Tokens: ... · Provider: ...`), each with an optional icon state (`NONE`/`LOADING`/`FINISHED`/`IDLE`). Owns a background animator thread that only redraws while `clay_below_set_editing(1)` is active, so it never clobbers unrelated output. `clay_prompt_line` renders the normal prompt-plus-status block; a conversation can temporarily render just that same status row while it waits for the first streamed token.

### `src/render/modals/` — composite widgets

Purpose-built interactive widgets that combine several `render/` primitives into one specific UI, when a need doesn't fit `select`/`choice`. Not meant to be generic — each file is its own thing.

- `model_select.c` — one active provider control (left/right) and its fixed-width search input share the header row. A filterable, scrollable six-row model area sits below it, bracketed by stable blank-or-count rows for models above and below. Each provider supplies a lazy `fetch(ctx, out)` callback; the selector retains each tab for the current modal, while `main.c`'s connected-provider state retains fetched results for the life of the CLI.

### `src/commands/` — registry and state machine

- `command.c` — parses one line of input into a command (`/name args...`, dispatched by name via `ClayMap`) or a plain message (`ClayInput`), and holds the registry of registered command handlers.
- `app.c` — `ClayApp`: a small state machine (`IDLE` / `BUSY` / `PROMPTING` / `EXITING`) with a listener hook. Command handlers never call `render/` functions directly — they call `clay_app_say`, `clay_app_task_start`, `clay_app_select`, etc., which update `ClayApp`'s state *and then* delegate to the matching render function. This keeps "what's happening" (state) and "how it looks" (ANSI output) in sync by construction, and is the seam a future agent daemon would drive through instead of writing to the terminal directly.

### `src/main.c`

The demo/test driver. Registers commands (`/help`, `/exit`, `/confirm`, `/select`, `/choice`, `/below`, `/model`, `/connect`, `/demo`, `/mm`), registers the below-prompt status modules (`model`, `tokens`, `provider`), and runs the main read-parse-dispatch loop. It loads every saved provider connection into a session-owned client; fetching models on a tab starts a spinner task, then caches that provider's result until exit. It restores the selected model/provider from `~/.clay/config.json`, with `Model: None` and `Provider: None` when no selection exists. Normal messages form a selected-model conversation: a system message is created at startup, a dedicated left-aligned `Thinking` spinner owns its terminal row until the first assistant token arrives, every successful user/assistant turn is retained with its OpenAI roles, and assistant tokens are written as their SSE chunks arrive. Final provider usage updates the below-prompt `Tokens: N in / N out` module. Changing provider or model resets that history and token counter under the same system prompt. `/demo` runs the canned scan → plan → write → test render sequence.

`/connect [type]` is the command that reaches into the backend stack: with no argument it's a `clay_app_choice` picker over `PROVIDER_TYPES` (`openai`, `openrouter`, `custom`), each row's title getting a green checkmark appended when `clay_config_exists` finds a saved config; with an argument it skips straight to that type. Either way it prompts for a base URL (skipped - `type->default_base_url` is used - unless the type is `custom`) and an API key (`clay_prompt_secret`), then `clay_config_save`s the result and refreshes that session's client/cache. `/model` shows only connected providers as tabs, errors immediately when none are connected, and writes its selected provider/model to the global config. No `openrouter.c`/`openai_custom.c` exist - every entry in `PROVIDER_TYPES` is the same `providers/openai.c` client underneath, just a different `id`/default `base_url`.

## Key design decisions

**Interactive vs. fallback duality.** Every interactive widget (`clay_prompt_line`, `clay_prompt_select`, `clay_prompt_choice`, `clay_model_select`) checks `clay_term_is_interactive()` first. On a real tty it uses raw mode and arrow keys; otherwise it falls back to buffered line reads (numbers for choices, plain text for input). This is what keeps the binary usable when piped or scripted, and is exercised by feeding it through a pipe rather than a pty.

**The row-tracking primitive.** Any widget that redraws more than one line in place (`below.c`'s status block, the `choice` picker, `model_select`) shares the same mechanism: track a resting cursor position between redraws, use `clay_term_cursor_down` to revisit rows that already exist (never scrolls) and a real `'\n'` only the first time a row is created (scrolls if needed). Getting this wrong is the main historical bug class in this codebase — it makes a widget visually "walk" up the screen and duplicate itself once the terminal is full, because a pure cursor-down escape can't scroll into new territory but a `'\n'` always can. `clay_term_row_enter` centralizes the decision.

**Color is a single switch.** `clay_color(CODE)` returns `CODE` or `""` depending on one process-wide flag, initialized from `NO_COLOR` and overridable via `--no-color`. Every ANSI constant is routed through it, so no code path can leak color when it's supposed to be off.

**Ownership follows the codebase's `_create`/`_destroy` and `_init`/`_free` convention throughout** — `ClayApp`, `ClayCommandRegistry`, `ClayStr`, `ClayTask`, `ClayModelSelection` all pair a constructor with an explicit teardown; nothing relies on process exit to clean up.

## Build

`Makefile` builds natively (`make build`, objects in `build/`, binary in `bin/clay`) and cross-compiles for Windows via mingw-w64 (`make build-win`, `build-win/`, `bin-win/clay.exe`, statically linked except for libcurl - see CODESTYLE.md). Both share the same `src/` tree; only `term.c` branches on `_WIN32`. `make run` builds and runs the native binary. `make test-openai` builds `bin/test_openai` from the same object tree minus `main.o`, plus `test_openai.o` - see "`src/providers/`" above.
