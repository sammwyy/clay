<div align="center">

# clay

### The focused AI coding agent for your terminal.

Bring your own model. Stay in your flow. Ship better work.

[![Release](https://img.shields.io/github/v/release/sammwyy/clay?style=flat-square&color=orange&label=release&sort=semver)](https://github.com/sammwyy/clay/releases/latest)
![Language](https://img.shields.io/badge/language-C11-blue?style=flat-square)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Windows-informational?style=flat-square)
![Size](https://img.shields.io/badge/binary-~90%20KB%20compressed-success?style=flat-square)

</div>

```text
◆ Clay  v0.0.3
  /help for commands  ·  Ctrl+O reasoning

› review this project and find the rough edges

◆ Agent (your-model)
  Reasoning finished in 1.2s
  I’ll inspect the structure and trace the main flows first.

  ~/your-project · your-model    ↑ 842 ↓ 126 · ✓ 1.2s
```

Clay is a compact coding-agent harness built for people who prefer to stay in the terminal. It connects to the models you already use, works directly on your files, keeps a paper trail of what it changed, and remembers what matters between sessions.

No giant framework to learn. No crowded dashboard. A single self-contained binary, no third-party dependency besides libcurl for HTTPS.

## What is here today

| | |
| --- | --- |
| **Your models, your choice** | Connect OpenAI, OpenAI Codex (ChatGPT Plus/Pro), Grok, OpenRouter, Groq, DeepSeek, Mistral, Together AI, or any OpenAI-compatible endpoint. Browse each provider's models without leaving the terminal. |
| **Dedicated file tools** | `read`, `write`, `edit`, `glob`, `grep` operate directly on the workspace — `edit` requires an exact, unique text match (no fuzzy diffing), and every path is checked against escaping the workspace root. |
| **Sandboxed by default** | On Linux, shell commands run with an isolated filesystem, no network, and resource limits — `/workspace` is your project and `/scratch`/`/tmp` is conversation scratch space. Opt out with `/sandbox`. |
| **Permissions & Plan mode** | `/permissions` sets which tool categories (read, edit, safe commands, all commands) run without asking; anything else prompts once, with an "always this session" option. `/plan` goes further and refuses writes/edits and mutating shell commands outright, so you can discuss an approach before anything changes. |
| **Checkpoints, not just trust** | Every `write`/`edit`/`shell_exec` call snapshots the workspace into a real git repo behind the scenes first (no libgit2 — `git` is shelled out). `/checkpoints` lists them and restores any point, even if your project isn't a git repo itself. `/undo` quickly reverses the last successful file edit without restoring the whole workspace. |
| **Memory that lasts** | Long-term memory (`/memory`) survives across every chat; a short-term scratchpad rides along with the active conversation as it grows. |
| **Context that doesn't run away** | Old tool output is collapsed to short previews automatically once a request nears the token budget — no extra model call. `/compact` goes further: it asks the model itself to write a summary (with tool output stripped first) and replaces the conversation with it. |
| **Already knows the room** | An `AGENTS.md`/`CLAY.md` in the workspace is folded into the system prompt automatically; the OS, working directory, git branch, and a top-level file listing ride along too, so the model doesn't spend a turn on `pwd`/`ls`/`git branch`. |
| **A visible plan** | `todowrite` keeps a live task checklist for multi-step work; `repo_map` gives a ranked overview of the codebase's top-level definitions (via `ctags` if it's installed, otherwise a small built-in heuristic for C, Python, JS/TS, Go, and Rust) before the model starts reading files one by one. |
| **Bring your own tools** | `/mcp add <name> <command> [args...]` connects any Model Context Protocol server over stdio — its tools show up next to clay's own. stdio transport only: no SSE/HTTP, no OAuth. |
| **Skills, loaded on demand** | `/skill install <path>` registers any `SKILL.md`-based skill directory in place (no copying) — including ones written for other agents that use the same convention. Only each skill's name and one-line description sit in the system prompt; the model loads the full instructions itself, through a `skill` tool call, the moment a task actually needs them. |
| **Tests run themselves** | `/autotest <command>` runs your test or lint command after a successful edit (confirmed once per session) and hands any failure straight back to the model. |
| **Provider-aware token caching** | Normalizes input, output, and cached-input usage when the provider reports it; the live status shows `(cache: N%)`, while unknown cache support stays distinct from a real zero. |
| **Persistent reasoning and usage** | Reasoning logs and token totals are stored with the chat journal, restored by `/resume`, and the latest completed reasoning can be expanded with `Ctrl+O`. |
| **A focused interface** | Streaming output, grouped tool activity, modular live status, reasoning controls, token counts, and instant cancellation stay out of the way of the prompt. |
| **HTTPS on Windows** | The Windows build uses libcurl with the native Windows certificate store, so HTTPS requests work without shipping a separate CA bundle. |

## Get to your first task

Grab a prebuilt binary from the [latest release](https://github.com/sammwyy/clay/releases/latest), or build from source and start in a safe playground:

```sh
make run
```

Then, inside Clay:

```text
/connect
/model
> help me understand this codebase
```

That is the whole loop. Connect a provider, choose a model, and describe the outcome you want.

### Install the latest release

On Linux, download and install the latest binary under
`~/.local/bin`:

```sh
curl -fsSL https://raw.githubusercontent.com/sammwyy/clay/main/install.sh | sh
```

On Windows, run the PowerShell installer and choose whether to install Clay
for the current user or all users:

```powershell
irm https://raw.githubusercontent.com/sammwyy/clay/main/install.ps1 | iex
```

Per-user installs go under `%USERPROFILE%\.clay\bin`; all-user installs go
under `Program Files\Clay`. The selected user or machine `PATH` is updated
automatically.

To work in a real project instead:

```sh
make build
./bin/clay --cwd /path/to/your/project
```

For CI and scripting, send one prompt and exit without opening the interactive
prompt or attempting an OAuth login:

```sh
OPENAI_API_KEY=... OPENAI_MODEL=gpt-4o-mini \
  ./bin/clay --prompt "Run the test suite and summarize failures"
# -p is an alias for --prompt
```

`clay skill install/remove/enable/disable/list` manages skills directly,
without starting the agent - no provider, no HTTP, no chat:

```sh
clay skill install https://github.com/example/some-skill.git
clay skill install ./local-skill-dir
clay skill disable some-skill
clay skill              # list installed skills
```

It's the same code as the interactive `/skill` command; see "Extensibility"
below for what each subcommand does.

The environment is an in-memory override; API keys are not written to
`~/.clay`. `CLAY_PROVIDER` and `CLAY_MODEL` can be used for explicit provider
and model selection. Provider-specific credentials are:

| Provider | Variables |
| --- | --- |
| OpenAI | `OPENAI_API_KEY`, optional `OPENAI_BASE_URL`, `OPENAI_MODEL` |
| OpenRouter | `OPENROUTER_API_KEY`, optional `OPENROUTER_BASE_URL`, `OPENROUTER_MODEL` |
| Groq | `GROQ_API_KEY`, optional `GROQ_BASE_URL`, `GROQ_MODEL` |
| DeepSeek | `DEEPSEEK_API_KEY`, optional `DEEPSEEK_BASE_URL`, `DEEPSEEK_MODEL` |
| Mistral | `MISTRAL_API_KEY`, optional `MISTRAL_BASE_URL`, `MISTRAL_MODEL` |
| Together AI | `TOGETHER_API_KEY`, optional `TOGETHER_BASE_URL`, `TOGETHER_MODEL` |
| xAI/Grok API | `XAI_API_KEY` (or `GROK_API_KEY`), `GROK_MODEL` |
| Custom OpenAI-compatible | `CLAY_PROVIDER=custom`, `CLAY_API_KEY`, `CLAY_BASE_URL`, `CUSTOM_MODEL` |

When there is no saved provider selection, Clay infers the provider from the
credential variable. All provider URLs must use HTTPS. A saved OpenAI Codex
session can be used by `-p` as-is; headless Codex sessions may provide
`OPENAI_CODEX_ACCESS_TOKEN`, `OPENAI_CODEX_REFRESH_TOKEN`,
`OPENAI_CODEX_ACCOUNT_ID`, optional `OPENAI_CODEX_ID_TOKEN` and
`OPENAI_CODEX_EXPIRES_AT` instead.

## Designed around the terminal

**A model picker that respects your time.** Connected providers become tabs. Clay retrieves a provider's models when you open it, then keeps that list available for the rest of the session.

**Reasoning when you need it.** Use `/effort` to match the model's reasoning level to the job: quick answers for small tasks, deeper thought for harder ones.

**Your workspace is part of the conversation.** When the model needs evidence, it reads, searches, and edits files directly instead of guessing — `shell_exec` is still there for everything else (builds, tests, git, other programs).

**Isolated by default, unleashed when you want.** On Linux, `shell_exec` runs in its own user/mount/pid/network namespaces: `/workspace` maps to your project, `/scratch` and `/tmp` are a per-conversation scratch dir, host paths and inherited environment variables are unavailable, and CPU, memory, process, file-size, and wall-clock limits apply. `/sandbox` switches to Unleashed (no sandbox). Windows runs Unleashed only, for now.

**Explicit consent, not endless prompts.** Reads, edits, and a curated set of safe commands (`ls`, `cat`, `git status`, ...) are approved by default; anything else — arbitrary shell commands, mainly — asks the first time and can be remembered for the rest of the session via `/permissions`. `/plan` is the stricter mode: no writes, no edits, and a blocklist keeps `rm`/`mv`/`cp` and mutating `git` subcommands from running at all, regardless of what `/permissions` allows.

**Remembers on purpose, not by accident.** The agent saves a long-term memory entry after real decisions and bug fixes, and reads one back up in a future chat — you stay in control with `/memory`, which browses, reads, or deletes any entry.

**Cancel without losing your place.** Press `Esc` or `Ctrl-C` while Clay is generating. The generation stops, the chat is preserved, and you are back at the prompt.

**Caching without provider logic in the harness.** Clay keeps the stable system
prompt and conversation in the chat journal, then lets each provider adapter
opt into its own cache mechanism. OpenAI and Codex use a stable per-chat prompt
cache key; Grok uses the chat id for stable routing; generic OpenAI-compatible
endpoints receive normal requests unless they explicitly report cache usage.
The status line exposes only normalized usage, so `(cache: N%)` appears when
the provider actually reports cached input tokens.

**Reasoning that stays with the chat.** Supported providers stream reasoning
before the normal assistant response. When generation ends, Clay replaces the
stream with a compact elapsed-time summary; `Ctrl+O` expands the latest saved
reasoning log. The reasoning text and its duration are serialized in the chat
JSON and restored with `/resume`, alongside input, output, and cached-token
usage.

**A modular status line.** The workspace/model block, token counts, cache
percentage, sandbox mode, and elapsed time are live modules that can be
enabled, disabled, reordered, and updated independently. The display groups
them with consistent separators while keeping the prompt and streamed output
separate.

## Command center

### Providers & models

| Command | What it does |
| --- | --- |
| `/connect [openai\|openai-codex\|grok\|openrouter\|groq\|deepseek\|mistral\|together\|custom]` (`/login`, `/provider`, `/providers`) | Connect a provider. OpenAI Codex uses ChatGPT Plus/Pro OAuth; Groq, DeepSeek, Mistral, and Together AI use their OpenAI-compatible APIs with preset base URLs. |
| `/logout` | Choose a connected provider and remove its saved session. |
| `/model [id]` (`/models`) | Browse models by provider, or set an id directly. |
| `/effort` | Set the model reasoning effort when supported. |

### Safety & control

| Command | What it does |
| --- | --- |
| `/sandbox` | Switch between restricted Sandbox and Unleashed execution. |
| `/permissions` | Toggle auto-approval for read / edit / safe-command / all-command tool calls. |
| `/plan` | Toggle Plan mode (blocks writes/edits and mutating shell commands) vs. Act mode. |
| `/checkpoints` | Browse and restore this chat's workspace checkpoints. |
| `/undo` | Undo the last successful `write` or `edit` without restoring a full checkpoint. |
| `/exec <command>` | Run a shell command through the sandbox directly, without going through the model. |

### Extensibility

| Command | What it does |
| --- | --- |
| `/mcp` | List configured MCP servers, or `/mcp add <name> <command> [args...]` / `/mcp remove <name>`. |
| `/skill` | List installed skills, or `/skill install <path-or-git-url> [name]` / `/skill remove <name>` / `/skill enable\|disable <name>`. A git URL is cloned into `~/.clay/skills/sources/` (`git pull`s on reinstall). |
| `/autotest [<command>\|clear]` | Set (or clear) the command that runs after a successful edit. |

### Conversation

| Command | What it does |
| --- | --- |
| `/compact` | Replace the conversation with an LLM-written summary. |
| `/memory [slug]` | Browse long-term memory, or read one entry directly. `/memory forget <slug>` deletes it. |
| `/new` | Start fresh. Your next message creates a new chat. |
| `/clear` | Alias for `/new`. |
| `/resume` | Return to any saved conversation. |
| `/history [n]` | Review recent messages from the active chat. |

### General

| Command | What it does |
| --- | --- |
| `/help` | See every available command. |
| `/exit` | Close Clay. |

The repository also includes shell completions in `completions/` and a man
page in `man/clay.1`. Run `make completions man-pages` to validate those
distribution files.

### Keyboard shortcuts

| Shortcut | Action |
| --- | --- |
| `Ctrl-C` with text | Clear the current input. |
| `Ctrl-C` on an empty prompt | Exit Clay cleanly. |
| `Ctrl-L` | Clear the terminal and redraw the current prompt. |
| `Ctrl-R` | Search backward through prompt history; press again for older matches. |
| `Ctrl-O` | Expand the latest completed reasoning log. |
| `Esc` or `Ctrl-C` while generating | Cancel the active response. |
| `Esc`, then `Esc` again | Clear the current input. |

## Roadmap

- [ ] Reusable skills
- [ ] Smoother use of multiple providers in one conversation
- [x] Memory across chats
- [ ] A Windows sandbox (Unleashed only today)
- [x] Command intent detection (Plan mode blocks `rm`/`mv`/`cp` and mutating `git` subcommands before they run)
- [ ] Image support
- [ ] Audio support

## Build from source

Clay needs `make`, a C compiler, and libcurl. `ctags` is optional — `repo_map` uses it when it's on `PATH` and falls back to a built-in heuristic when it isn't.

```sh
make build
./bin/clay --help
```

For a smaller Linux release binary without LTO, use the separate release
profile. It enables `-Os`, section garbage collection, linker stripping, and
keeps libcurl dynamically linked:

```sh
make release
./bin-release/clay --help
```

For maximum executable compression, strip first and pack with UPX:

```sh
make compress-release
```

The regular `make compress` target applies the same strip-before-UPX sequence
to `bin/clay`. UPX is optional and only affects on-disk size; it can increase
startup time and trigger false positives in some antivirus products.

Run the local unit-test suite (the network-backed OpenAI test remains an
explicit integration target):

```sh
make test
```

## For contributors

- [Architecture](ARCHITECTURE.md)
- [Code style](CODESTYLE.md)
