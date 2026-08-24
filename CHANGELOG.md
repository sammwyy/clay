# Changelog

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
