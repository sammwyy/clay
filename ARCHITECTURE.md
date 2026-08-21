# Architecture

clay is a minimalist C render engine and CLI toolkit for building a Claude Code-style terminal agent. It has a render/UI layer, a command/session layer, and a backend layer (`json.c`, `http.c`, `providers/`, `config.c`) for talking to LLM APIs and remembering how to reach them. `/connect` saves provider credentials; `/model` uses those live connections to retrieve and select their available models.

## Layers

```
src/mm/            growable primitives: arena, str, array, map
src/json.c          minimal JSON value tree: parse, build, stringify
src/http.c          HTTP client, built on mm + libcurl
src/providers/      LLM provider clients, built on http + json (openai.c, ...)
src/config.c         saved provider credentials and model selection (~/.clay), built on mm + json + term
src/chat.c           versioned JSON chat journals, built on mm + json + term
src/time.c           wall-clock and relative-time formatting
src/cli/             typed process argument registry and clay startup options
src/render/         drawing primitives, built on mm + term
src/render/modals/  purpose-built composite interactive widgets, built on render
src/commands/       command registry, handlers, and agent session state
src/main.c          process bootstrap and input loop
tests/test_*.c      standalone test harnesses
```

Each layer only depends on the layers listed above it. `mm` depends on nothing else in the project. `json` depends only on `mm`. `http` depends on `mm` and libcurl, not on `json` - it's a generic transport, agnostic of what's riding on it. `providers/` depends on `mm`, `json`, and `http`. `config` and `chat` depend on `mm`, `json`, and `term` (for the home directory and file permissions - see below), not on `http`/`providers` - they only persist local data. `cli/` depends on `mm` and `term`. `render` depends on `mm` and `term`. `commands` is the integration layer over render/config/chat/providers. `main.c` only owns process startup and the input loop; `tests/test_openai.c` is a separate provider/http harness.

### `src/mm/` — memory

- `arena.c` — bump allocator for scoped batch allocations.
- `str.c` (`ClayStr`) — growable string buffer; the default choice for anything of unknown length.
- `array.c` (`ClayArray`) — growable, type-erased dynamic array (doubles on growth), with `clay_array_insert`/`clay_array_remove` for mid-array edits.
- `map.c` (`ClayMap`) — string-keyed hash map, separate chaining, used for the command registry's name → handler lookup.

These have no knowledge of terminals or rendering. They exist so nothing above this layer needs a fixed-size buffer for caller-supplied data.

### `src/json.c` — JSON

An opaque `ClayJson` value tree (null/bool/number/string/array/object), built on `ClayStr`/`ClayArray`. Constructors (`clay_json_object`, `clay_json_string`, ...) and mutators (`clay_json_object_set`, `clay_json_array_push`) take ownership of what's passed in; `clay_json_clone` deep-copies a value the caller wants to keep borrowing (e.g. a tool schema reused across several requests) into an independently-owned tree. `clay_json_parse` is a plain recursive-descent parser; `clay_json_stringify` serializes compact JSON. No schema validation, no comments/trailing-comma leniency - it only needs to round-trip what the providers below send and receive.

### `src/http.c` — HTTP client

A thin wrapper over libcurl's easy interface - the only place in the project that links a third-party library, and only dynamically (`-lcurl`, never bundled/static; see CODESTYLE.md). `clay_http_request` runs synchronously on the calling thread; a streaming response is read via a caller-supplied `on_chunk` callback (raw bytes, not line-buffered) instead of being accumulated whole, since a chat completion can stay open for a while. Its optional progress callback can abort an in-flight transfer. Callers needing whole lines (SSE) do their own buffering - see `providers/openai.c`.

### `src/providers/` — LLM provider clients

- `openai.c` — talks to any OpenAI-compatible endpoint: `GET /models` returns every model id for the connected account, while `/chat/completions` handles streaming responses (SSE, parsed line-by-line as chunks arrive in `on_http_chunk`, since a chunk boundary can land mid-line), final input/output token usage when supplied by the provider, JSON tool calls (accumulated across streamed deltas by their `index`, since the API sends id/name/argument-fragments as separate events), and the tool-call loop (`clay_openai_run` appends the assistant's tool-call message and each tool's result to `messages`, then resends the conversation, up to `max_rounds`, until the model answers with plain content and no further tool calls).

`tests/test_openai.c` is a standalone binary (`bin/test_openai`, the Makefile's `test-openai` target) that exercises streaming and tool calls against a real endpoint. It reads `OPENAI_BASE_URL`/`OPENAI_API_KEY`/`OPENAI_MODEL` from the environment so a token never ends up in argv or committed code. Test sources are outside the production source tree and are never linked into the main `clay` binary.

### `src/config.c` — saved provider credentials

Persists a `ClayProviderConfig {id, apikey, base_url}` per provider as `~/.clay/providers/<id>.json`, restricted to the owner (`clay_term_restrict_file`, POSIX 0600) since it holds a plaintext API key. `~/.clay/config.json` separately stores the selected `provider`, `model`, and `history_preview_count` (default 4). It never stores an active chat id. Doesn't know about provider *types* (openai vs. openrouter vs. a custom endpoint) - that mapping lives in `commands/context.c`'s `PROVIDER_TYPES` table, since they're all the same OpenAI-compatible wire format and only differ by default `base_url`. `id` doubles as both the config filename and the provider type it was connected as.

### `src/chat.c` — chat journals

Each chat lives at `~/.clay/chats/<uuid-v4>/chat.json`. A CLI starts without an active chat and creates one only when its first message is sent; `/new` (or `/clear`) discards the active chat from the session without deleting its journal, so the following message creates the next chat; `/resume` lists saved chats by id and relative update time, then explicitly loads one. IDs are UUID v4 values from the operating system's secure random source, so processes can create journals independently without coordinating counters. The journal is a versioned, provider-neutral JSON document with turns in `pending`, `completed`, `aborted`, `network_error`, or `provider_error` states. Its message records use `role`, `text`, `calls`, and `call_id` rather than the OpenAI wire shape. Before a request, `clay_chat_openai_messages` converts completed records into the provider-compatible array; the current system prompt is added by the command session and is never persisted in the chat. A turn is saved as `pending` before its request, then rewritten with the final response/tool records and state, preserving interrupted work for future retry/continue flows. `/resume` prints the configured number of recent messages, while `/history [n]` prints them on demand.

### `src/cli/` — process arguments

`cli.c` owns a dynamic registry of typed process options: booleans, strings, and numbers. It accepts `--name` and `-name`, values separated by a space or `=`, and retains positional command arguments for future subcommands. `startup.c` keeps clay's process-level behavior out of the agent loop: it registers `--help`, `--version`, `--no-color`, and `--cwd`; applies color/cwd choices; and exits before initialization for help or version.

### `src/render/` — drawing primitives

- `term.c` — the only platform-aware file. Cursor movement, raw mode, key reading (`ClayKey`, including timed Escape parsing), color enable/disable (`clay_color`, `NO_COLOR` handling), UTF-8-aware width, OSC 8 hyperlinks, `clay_term_row_enter` (the shared safe-redraw primitive, see CODESTYLE.md), bounded shell execution with dynamically collected output, and the small OS-specific primitives other layers need (`clay_term_home_dir`, `clay_term_mkdir`, `clay_term_restrict_file`) rather than letting them spread their own `#ifdef _WIN32`.
- `box.c` — bordered boxes with per-line text/border colors.
- `banner.c` — the startup banner, built on `box.c`.
- `list.c` — the `◆ clay` response prefix (`clay_say`/`clay_sayc`), incremental streamed assistant replies (`clay_response_begin`/`write`/`end`), plan/list rendering (`clay_list_step`), bullets.
- `task.c` — spinner-driven task lines (`clay_task_start/success/fail`), each on its own background thread that animates until stopped.
- `prompt.c` — the main `>` input line (history-aware, arrow-key recall, paste detection) and the general-purpose interactive prompts: `clay_prompt_select` (horizontal, left/right, no free text; transient and cleared after confirmation), `clay_prompt_choice` (vertical, up/down, optional free-text fallback row), and `clay_prompt_secret` (masked, for API keys).
- `below.c` — the status-module system: named modules registered via `clay_below_add`, rendered inline on one row under the prompt (`Idle · Model: ... · Tokens: ... · Provider: ...`), each with an optional icon state (`NONE`/`LOADING`/`FINISHED`/`IDLE`). Owns a background animator thread that only redraws while `clay_below_set_editing(1)` is active, so it never clobbers unrelated output. `clay_prompt_line` renders the normal prompt-plus-status block; a conversation can retain that same status-only row under streamed output, pushing it down before each response newline and restoring the next prompt below one blank separator row afterwards.

### `src/render/modals/` — composite widgets

Purpose-built interactive widgets that combine several `render/` primitives into one specific UI, when a need doesn't fit `select`/`choice`. Not meant to be generic — each file is its own thing.

- `model_select.c` — one active provider control (left/right) and its fixed-width search input share the header row. A filterable, scrollable six-row model area sits below it, bracketed by stable blank-or-count rows for models above and below. Each provider supplies a lazy `fetch(ctx, out)` callback; the selector retains each tab for the current modal, while the command session retains fetched results for the life of the CLI.

### `src/commands/` — registry, handlers, and session state

- `command.c` — parses one line of input into a command (`/name args...`, dispatched by name via `ClayMap`) or a plain message (`ClayInput`), and holds the registry of registered command handlers.
- `app.c` — `ClayApp`: a small state machine (`IDLE` / `BUSY` / `PROMPTING` / `EXITING`) with a listener hook. Command handlers never call `render/` functions directly — they call `clay_app_say`, `clay_app_task_start`, `clay_app_select`, etc., which update `ClayApp`'s state *and then* delegate to the matching render function. This keeps "what's happening" (state) and "how it looks" (ANSI output) in sync by construction, and is the seam a future agent daemon would drive through instead of writing to the terminal directly.
- `context.c` — owns the command session: saved/connected providers, model and reasoning selection, the active chat journal, token counters, and the below-prompt modules. `message.c` owns the normal-message OpenAI streaming/tool loop against that state, including Escape-driven request cancellation.
- `register.c` is the one registry wiring point. Every slash-command handler lives in its own file (`help.c`, `exit.c`, `connect.c`, `model.c`, `effort.c`, `demo.c`, and the remaining command-named files), so a command's UI and behavior are changed together without growing `main.c`.

### `src/main.c`

The process bootstrap. It starts the terminal/HTTP layers, builds the command session, registers the command handlers, and runs the read-parse-dispatch loop. Provider/model selection, conversation state, and command behavior live in `src/commands/` rather than here.

`/connect [type]` is the command that reaches into the backend stack: with no argument it's a `clay_app_choice` picker over `PROVIDER_TYPES` (`openai`, `openrouter`, `custom`), each row's title getting a green checkmark appended when `clay_config_exists` finds a saved config; with an argument it skips straight to that type. Either way it prompts for a base URL (skipped - `type->default_base_url` is used - unless the type is `custom`) and an API key (`clay_prompt_secret`), then `clay_config_save`s the result and refreshes that session's client/cache. `/model` shows only connected providers as tabs, errors immediately when none are connected, and writes its selected provider/model to the global config. No `openrouter.c`/`openai_custom.c` exist - every entry in `PROVIDER_TYPES` is the same `providers/openai.c` client underneath, just a different `id`/default `base_url`.

## Key design decisions

**Interactive vs. fallback duality.** Every interactive widget (`clay_prompt_line`, `clay_prompt_select`, `clay_prompt_choice`, `clay_model_select`) checks `clay_term_is_interactive()` first. On a real tty it uses raw mode and arrow keys; otherwise it falls back to buffered line reads (numbers for choices, plain text for input). This is what keeps the binary usable when piped or scripted, and is exercised by feeding it through a pipe rather than a pty.

**The row-tracking primitive.** Any widget that redraws more than one line in place (`below.c`'s status block, the `choice` picker, `model_select`) shares the same mechanism: track a resting cursor position between redraws, use `clay_term_cursor_down` to revisit rows that already exist (never scrolls) and a real `'\n'` only the first time a row is created (scrolls if needed). Getting this wrong is the main historical bug class in this codebase — it makes a widget visually "walk" up the screen and duplicate itself once the terminal is full, because a pure cursor-down escape can't scroll into new territory but a `'\n'` always can. `clay_term_row_enter` centralizes the decision.

**Color is a single switch.** `clay_color(CODE)` returns `CODE` or `""` depending on one process-wide flag, initialized from `NO_COLOR` and overridable via `--no-color`. Every ANSI constant is routed through it, so no code path can leak color when it's supposed to be off.

**Ownership follows the codebase's `_create`/`_destroy` and `_init`/`_free` convention throughout** — `ClayApp`, `ClayCommandRegistry`, `ClayStr`, `ClayTask`, `ClayModelSelection` all pair a constructor with an explicit teardown; nothing relies on process exit to clean up.

## Build

`Makefile` builds natively (`make build`, objects in `build/`, binary in `bin/clay`) and cross-compiles for Windows via mingw-w64 (`make build-win`, `build-win/`, `bin-win/clay.exe`, statically linked except for libcurl - see CODESTYLE.md). Both share the same `src/` tree; only `term.c` branches on `_WIN32`. `make run` creates the ignored `.playground` directory and starts the native binary with `--cwd .playground`, so tool calls do not use the repository root. `make compress` packs the native build in place with UPX using `--best --lzma`; `make compress-win` does the same for `bin-win/clay.exe`. Both targets unpack an already-compressed build first so they are repeatable. `make test-cli` exercises typed option parsing, `make test-chat` verifies journal persistence/reconstruction, `make test-uuid` verifies UUID v4 shape and uniqueness, and `make test-openai` builds `bin/test_openai` from the same object tree minus `main.o`, plus `test_openai.o` - see "`src/providers/`" above. `.github/workflows/release.yml` runs only for newly created tags, builds and UPX-compresses the Linux and Windows releases, and publishes `clay-linux-x86_64` and `clay-win-x86_64.exe` on the matching GitHub Release.
