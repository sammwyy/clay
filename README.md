<div align="center">

# clay

### The focused AI coding agent for your terminal.

Bring your own model. Stay in your flow. Ship better work.

`C11` &nbsp;·&nbsp; `OpenAI-compatible` &nbsp;·&nbsp; `~46 KB compressed`

</div>

```text
  ┌────────────────────────────────────┐
  │  clay — your AI code agent          │
  └────────────────────────────────────┘

> review this project and find the rough edges
◆ clay  I’ll inspect the structure and trace the main flows first.
  ✓ 1.2s · your-model (your-provider) · ↑ 842  ↓ 126
```

Clay is a compact coding-agent harness built for people who prefer to stay in the terminal. It connects to the models you already use and keeps the work grounded in your workspace.

No giant framework to learn. No crowded dashboard. Just you, your project, and an agent that can help move the work forward.

## What is here today

| | |
| --- | --- |
| **Your models, your choice** | Connect OpenAI, OpenRouter, or any OpenAI-compatible endpoint. Browse each provider’s models without leaving the terminal. |
| **A real terminal partner** | Clay can inspect files, run project commands, make changes, and explain what it found as it works. |
| **A focused interface** | Streaming output, visible status, reasoning controls, token counts, and instant cancellation stay out of the way of the prompt. |

## Get to your first task

Start Clay in a safe playground:

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

To work in a real project instead:

```sh
make build
./bin/clay --cwd /path/to/your/project
```

## Designed around the terminal

**A model picker that respects your time.** Connected providers become tabs. Clay retrieves a provider’s models when you open it, then keeps that list available for the rest of the session.

**Reasoning when you need it.** Use `/effort` to match the model’s reasoning level to the job: quick answers for small tasks, deeper thought for harder ones.

**Your workspace is part of the conversation.** When the model needs evidence, it can use the terminal in the selected working directory instead of guessing.

**Cancel without losing your place.** Press `Esc` or `Ctrl-C` while Clay is generating. The generation stops, the chat is preserved, and you are back at the prompt.

## Command center

| Command | What it does |
| --- | --- |
| `/connect [openai\|openrouter\|custom]` | Connect a provider and enter its API key. |
| `/model [id]` | Browse models by provider, or set an id directly. |
| `/effort` | Set the model reasoning effort when supported. |
| `/new` | Start fresh. Your next message creates a new chat. |
| `/clear` | Alias for `/new`. |
| `/resume` | Return to any saved conversation. |
| `/history [n]` | Review recent messages from the active chat. |
| `/help` | See every available command. |
| `/exit` | Close Clay. |

### Keyboard shortcuts

| Shortcut | Action |
| --- | --- |
| `Ctrl-C` with text | Clear the current input. |
| `Ctrl-C` on an empty prompt | Exit Clay cleanly. |
| `Esc` or `Ctrl-C` while generating | Cancel the active response. |
| `Esc`, then `Esc` again | Clear the current input. |

## Roadmap

- [ ] Reusable skills
- [ ] Smoother use of multiple providers in one conversation
- [ ] Memory across chats
- [ ] A sandbox for more isolated tool execution
- [ ] A scratchpad for planning and working context
- [ ] Image support
- [ ] Audio support

## Build from source

Clay needs `make`, a C compiler, and libcurl.

```sh
make build
./bin/clay --help
```

## For contributors

- [Architecture](ARCHITECTURE.md)
- [Code style](CODESTYLE.md)
