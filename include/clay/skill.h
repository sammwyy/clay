#ifndef CLAY_SKILL_H
#define CLAY_SKILL_H

#include "clay/array.h"

/* Skills: reusable instruction sets loaded on demand, not copied into
   ~/.clay - installing one just records where its SKILL.md already lives
   (~/.clay/skills/index.json), so a directory written for Claude Code (or
   any other agent using the same SKILL.md + YAML frontmatter convention)
   works here unmodified. Only name+description sit in every system prompt
   (clay_skill_index); the full body loads into context lazily, through the
   "skill" tool, the first time a turn actually calls it. */

/* Lowercase letters, digits, and hyphens only, 1-64 chars. */
int clay_skill_valid_name(const char *name);

/* Resolves `path` to a SKILL.md (path itself, or path/SKILL.md) and
   records it in the index; `name` overrides the frontmatter name when
   non-NULL/non-empty. If `path` looks like a git remote (http(s)/git/ssh/
   file URL, "git@host:...", or a ".git" suffix), it's `git clone`d (or
   `git pull`d, if already cloned before) into ~/.clay/skills/sources/ first
   - real git, shelled out, same as clay/checkpoint.h. A github.com "tree"/
   "blob" web URL (e.g. copy-pasted from browsing a folder in a multi-skill
   repo, .../tree/<branch>/path/to/skill) is recognized too: it clones the
   repo at that branch and looks for SKILL.md under that subpath, not the
   repo root. 0 on success; -1 if the clone/file/frontmatter is missing,
   the name is invalid, or the index can't be written. */
int clay_skill_install(const char *path, const char *name);

/* Removes the manifest entry (the SKILL.md itself is untouched, since it
   was never copied). 0 on success (including if `name` wasn't installed),
   -1 on an invalid name or I/O failure. */
int clay_skill_remove(const char *name);

/* 0 on success (including redundant enable/disable), -1 if `name` isn't
   installed. */
int clay_skill_set_enabled(const char *name, int enabled);

/* Malloc'd index text, one "- name: description" line per *enabled*
   entry; empty string if there are none. For the system prompt. */
char *clay_skill_index(void);

/* Malloc'd "Skill directory: <dir>\n\n<body>" (frontmatter stripped) for
   the named skill, or NULL if it isn't installed, is disabled, or its
   SKILL.md went missing since install. Caller frees. */
char *clay_skill_read(const char *name);

typedef struct {
    char *name;
    char *description;
    char *path; /* directory containing SKILL.md */
    int enabled;
} ClaySkillEntry;

/* Every installed skill, in install order, enabled and disabled alike.
   Caller frees with clay_skill_entries_free. */
void clay_skill_list(ClayArray *entries);
void clay_skill_entries_free(ClayArray *entries);

/* `clay skill ...` CLI entry point: the same install/remove/enable/
   disable/list operations as the interactive /skill command, without
   starting the agent - no provider connection, no chat, no ClayApp. argv
   excludes "clay skill" itself; argc == 0 lists installed skills, same as
   bare /skill. Returns a process exit status (0 success, 1 otherwise). */
int clay_skill_cli_main(int argc, char **argv);

#endif /* CLAY_SKILL_H */
