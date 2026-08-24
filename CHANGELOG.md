# Changelog

## Unreleased

### Added

- `ask_user` tool: the model can ask one question mid-turn and get the answer
  back in the same turn, through the same picker `/permissions` uses. Options
  come with optional one-line notes, and a "Type your own..." row is offered
  unless the model turns it off. Without a tty the call fails with an
  instruction to assume and continue, so `-p` and piped runs never hang.

### Changed

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
