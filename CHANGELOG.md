# Changelog

## Unreleased

### Added

- `ask_user` tool: the model can ask one question mid-turn and get the answer
  back in the same turn, through the same picker `/permissions` uses. Options
  come with optional one-line notes, and a "Type your own..." row is offered
  unless the model turns it off. Without a tty the call fails with an
  instruction to assume and continue, so `-p` and piped runs never hang.
- Subagents: a `subagent` tool that runs one step of a plan on a fresh agent
  with the same tools but no conversation history, and hands back a summary
  the caller passes into the next step. It cannot nest or ask the user, its
  file changes are checkpointed and permission-gated like any other tool, the
  spinner shows which tool it is running right now, and the whole run (prompt,
  every message, summary, timing) is written to
  `~/.clay/chats/<chat id>/subagents/<execution id>.json`. The system prompt
  keeps it optional: plan and delegate only when the job has several real
  parts, otherwise just do the work.
- Background tasks: `task_run`, `task_output`, `task_stop`, and `task_list`
  let the model start a blocking command (a dev server, a watcher) on its own
  thread and keep working - start it, curl it, read its log, stop it, all in
  one turn. Output is streamed into a 256 KB tail buffer, tasks are killed
  when clay exits, and a sandboxed task says up front that its network
  namespace is empty. A live "N bg" status shows how many are running.
- `shell_exec` takes `timeout_seconds` (default 120, max 3600) and reports
  `timed_out` when it kills a command, so a stuck command returns what it
  printed instead of stalling the session.

### Fixed

- Stopping a background task no longer hangs. A sandboxed command runs as PID
  1 of its own namespace, where the kernel drops SIGTERM, so it outlived the
  child clay was waiting on and kept the output pipe open - the reader spun
  forever and `task_stop` never returned, leaving the command running. The
  kill now escalates as soon as that child is reaped, output stops being
  drained once nothing is left to write it, and the poll loop always sleeps.
  The same hang could happen outside the sandbox with a command that
  backgrounds a child of its own.
- Sandboxed commands can now reach their own ports: an unshared network
  namespace leaves loopback down, so even `server & curl localhost` failed
  inside a single command. Loopback is brought up in the namespace, which
  still has no route anywhere else - a sandboxed command reaches its own
  servers and nothing beyond them.
- A stray terminal escape sequence no longer cancels a running turn. Anything
  clay has no binding for - a focus in/out report when you switch windows, a
  mouse report, Home/End/Delete, a reply to a terminal query - used to read as
  Escape and abort the request silently, mid-work. Only a real Escape cancels
  now.
- The workspace's file listing left the system prompt, where it was frozen for
  the life of a chat and could describe files deleted hours earlier, sending
  the model chasing them. It now rides in the conversation as a context
  message, appended only when the environment actually changes - the message
  array still only grows, so the provider's prefix cache keeps hitting (97%
  on a third turn here, against 0% for a block injected and removed each
  request). The chat's notes block moved to the same footing. The cached
  system prompt also carries a fingerprint of the build that wrote it and
  expires from when it was built rather than from its last use, so it can no
  longer outlive its contents.
- A turn that ends right after a tool call, with no closing message from the
  model, now says so instead of leaving a blank gap that looks like clay lost
  the answer.
- A permission prompt or question raised by a running tool no longer leaves a
  half-drawn spinner row behind it: the prompt takes that row over and the
  tool's result line lands underneath.
- The reasoning stream keeps a live spinner and clock in the status row under
  it, repainted by the animator once the stream goes quiet. A provider that
  stalls mid-answer now shows a climbing clock instead of a screen that looks
  frozen, and Escape still cancels the request while it is stalled.
- Aborting a turn stops the clock and the spinner instead of leaving them
  running under the next prompt.

### Changed

- One place now builds the tool list and one runs a request against the
  selected provider, so the message loop, `/compact`, and subagents no longer
  each carry their own copy of the OpenAI/Codex/Grok client plumbing.

- Shell execution is now two axes cycled together by Shift+Tab: Sandbox (Ask),
  Sandbox (Auto), Unleashed (Ask), Unleashed (Auto). Ask prompts for every
  tool call, Auto approves them; the status below the prompt colors Sandbox
  and Ask pink, Unleashed and Auto red. Configs holding the older `auto` value
  still read as Sandbox (Auto).
- Streamed answers and reasoning are word-wrapped and kept inside the block
  indent instead of breaking mid-word at column 0. Piped output (`-p`) is
  untouched, so redirected text keeps the model's own line breaks.
- The turn's spinner and elapsed time are the first thing in the status line,
  and the clock keeps running under the answer as it streams.
- Tool lines say what actually went wrong: a failed call shows its error
  instead of "exit 0", the message is no longer repeated underneath, tabs in
  tool output render as spaces, and a long command or output line is clipped
  to one row instead of wrapping.
- The working directory below the prompt is dropped whenever the status would
  no longer fit on one row.
- Rewrote the system prompt around engineering practice: evidence over
  guesswork, diagnose before editing, reuse before writing, verify what you
  changed (or hand the user the command to verify it), minimal comments that
  state facts rather than reasoning, plain accurate English in docs, and
  Conventional Commits for any commit the user asks for.

## 0.0.3

### Added

- Skills: `/skill` (and the non-interactive `clay skill` CLI subcommand) to
  install, remove, enable, and disable reusable instruction sets. Only each
  skill's name and one-line description sit in the system prompt; the model
  loads the full instructions itself, through a `skill` tool call, only when
  a task actually needs them - keeping the provider's prompt cache warm.
  Installs from a local path or a real `git clone`/`git pull` (including a
  GitHub folder link copy-pasted straight from the browser, e.g.
  `.../tree/<branch>/path/to/skill`), so skills written for other agents
  that use the same `SKILL.md` convention work unmodified.
- Provider-aware prompt caching strategy across providers.
- Cache hit ratio counter, as reported by the provider.
- System notifications when the agent finishes a turn, or needs permission/
  command approval.
- One-line install scripts for Linux and Windows.
- Live reasoning streaming, plus `Ctrl+O` to expand the previous turn's
  reasoning.

### Fixed

- The UI is now responsive to terminal resizes.
- Faster config loading and JSON parsing.
