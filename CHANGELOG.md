# Changelog

## Unreleased

### Added

- `/tasks` lists running background commands in an interactive picker and can
  stop a selected task after confirmation.
- `ask_user` tool: the model can ask one question mid-turn and get the answer
  back in the same turn, through the same picker `/permissions` uses. Options
  come with optional one-line notes, and a "Type your own..." row is offered
  unless the model turns it off. Without a tty the call fails with an
  instruction to assume and continue, so `-p` and piped runs never hang.
- Subagents: a `subagent` tool that forks up to four branches of a job at
  once, each on a fresh agent with the same tools but no conversation
  history, and returns every summary when the last one lands. Two branches
  that each took 45s finished in 46s of wall clock here. Each plans its own
  work with its own private `todowrite` checklist, none of them can nest or
  ask the user, and their file writes, approvals and checkpoints serialize on
  one session lock so parallel branches cannot interleave inside a change.
  The spinner reports branches done, tool calls spent and which one is still
  running; every run (prompt, messages, summary, timing) is written to
  `~/.clay/chats/<chat id>/subagents/<execution id>.json`. The system prompt
  keeps it optional and says delegating is how you do a step, never a step of
  its own.
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

- A turn no longer stops dead in the middle of a job. The tool-call budget
  was 8 rounds - a scaffold plus a build plus a verification pass spends that
  long before it starts - and hitting it returned the same code as a network
  failure, which the interactive path then reported as nothing at all: the
  transcript just ended and a fresh prompt appeared. The budget is now 64
  rounds (32 for a subagent), running out is its own outcome that keeps every
  tool result in the conversation so the next message continues the job, and
  the turn always says how it ended - out of rounds, provider error, or the
  model stopping after its last tool call.
- Reasoning streams on a single self-rewriting row instead of pouring into
  the transcript. A minute of thinking - which for a coding model often means
  whole drafts of the code it is about to write - used to cost a screenful of
  scroll before collapsing to one line. Now it costs one row while it runs,
  collapses to `Reasoning finished in Ns`, and `Ctrl+O` still expands the
  whole text.
- A running tool row shows its elapsed time, not just a spinner, so a long
  call (a subagent, a slow build) reads as working rather than stuck.
- The plan reads like a checklist instead of a wall of text. `todowrite` no
  longer prints a spinner row and a dump of every step on each call: the
  first plan prints once, and later calls print only the steps that actually
  moved, as `✓ done` / `→ running` / `· pending` lines. The step in flight
  also shows in the status row with its own spinner and a `2/3` counter, and
  clears when the plan finishes.
- Stopping a background task no longer hangs. A sandboxed command runs as PID
  1 of its own namespace, where the kernel drops SIGTERM, so it outlived the
  child clay was waiting on and kept the output pipe open - the reader spun
  forever and `task_stop` never returned, leaving the command running. The
  kill now escalates as soon as that child is reaped, output stops being
  drained once nothing is left to write it, and the poll loop always sleeps.
  The same hang could happen outside the sandbox with a command that
  backgrounds a child of its own.
- Sandboxed commands can reach each other's ports. Loopback was left down in
  the unshared network namespace, so even `server & curl localhost` failed
  inside a single command, and every command got its own namespace besides -
  a server started by `task_run` was unreachable from the next `shell_exec`.
  Loopback is now brought up, and the session's sandboxed commands join one
  user and network namespace (filesystem, pids and ipc stay private per
  command). A server started in the sandbox answers the next command over
  localhost and stays unreachable from the host: verified at 200 across
  commands with an outside request still blocked.
- Buffered terminal output could be flushed into a command's own pipe by the
  fork that ran it and come back as part of its output. The flush now happens
  before the fork.
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

- The system prompt now says the checklist is the plan the user reads: no
  restating it in prose, no printing a tree of files about to be created, no
  announcing each step before taking it.

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
