#include "clay/checkpoint.h"

#include "clay/str.h"
#include "clay/term.h"

#include <stdlib.h>
#include <string.h>

/* strndup is a POSIX/GNU extension mingw doesn't provide. */
static char *dup_n(const char *text, size_t len) {
    char *copy = malloc(len + 1);
    memcpy(copy, text, len);
    copy[len] = '\0';
    return copy;
}

#define CLAY_CHECKPOINT_OUTPUT_LIMIT (16 * 1024)
#define CLAY_CHECKPOINT_REF "refs/heads/checkpoints"

/* Runs one git subcommand against `checkpoints_dir` (--git-dir), optionally
   against `workspace_dir` (--work-tree, for the two commands that touch
   workspace files), with a fixed commit identity so commit-tree never
   depends on the host's git config. Trims one trailing newline from
   `output`, matching what `git rev-parse`/`write-tree`/`commit-tree` print. */
static int run_git(const char *checkpoints_dir, const char *workspace_dir, const char *args, ClayStr *output,
                   int *exit_code) {
    clay_term_setenv("GIT_AUTHOR_NAME", "clay");
    clay_term_setenv("GIT_AUTHOR_EMAIL", "clay@localhost");
    clay_term_setenv("GIT_COMMITTER_NAME", "clay");
    clay_term_setenv("GIT_COMMITTER_EMAIL", "clay@localhost");

    ClayStr command;
    clay_str_init(&command);
    clay_str_push(&command, "git --git-dir=");
    clay_term_shell_quote(&command, checkpoints_dir);
    if (workspace_dir) {
        clay_str_push(&command, " --work-tree=");
        clay_term_shell_quote(&command, workspace_dir);
    }
    clay_str_push_char(&command, ' ');
    clay_str_push(&command, args);

    clay_str_clear(output);
    int truncated = 0;
    int rc = clay_term_shell_exec(command.data, output, CLAY_CHECKPOINT_OUTPUT_LIMIT, exit_code, &truncated);
    clay_str_free(&command);

    clay_term_setenv("GIT_AUTHOR_NAME", NULL);
    clay_term_setenv("GIT_AUTHOR_EMAIL", NULL);
    clay_term_setenv("GIT_COMMITTER_NAME", NULL);
    clay_term_setenv("GIT_COMMITTER_EMAIL", NULL);

    while (output->len > 0 && output->data[output->len - 1] == '\n') clay_str_remove_n(output, output->len - 1, 1);
    return rc;
}

/* `git init --bare <dir>` ignores --git-dir and creates the repo at its own
   argument, so this can't go through run_git (which always sets
   --git-dir=checkpoints_dir before checkpoints_dir necessarily exists). */
static int ensure_bare_repo(const char *checkpoints_dir) {
    ClayStr command;
    clay_str_init(&command);
    clay_str_push(&command, "git init --bare --quiet ");
    clay_term_shell_quote(&command, checkpoints_dir);
    ClayStr output;
    clay_str_init(&output);
    int exit_code = -1;
    int truncated = 0;
    int rc = clay_term_shell_exec(command.data, &output, CLAY_CHECKPOINT_OUTPUT_LIMIT, &exit_code, &truncated);
    clay_str_free(&command);
    clay_str_free(&output);
    return rc == 0 && exit_code == 0 ? 0 : -1;
}

int clay_checkpoint_save(const char *checkpoints_dir, const char *workspace_dir, const char *label) {
    if (ensure_bare_repo(checkpoints_dir) != 0) return -1;

    ClayStr output;
    clay_str_init(&output);
    int exit_code = -1;

    if (run_git(checkpoints_dir, workspace_dir, "add -A", &output, &exit_code) != 0 || exit_code != 0) {
        clay_str_free(&output);
        return -1;
    }

    if (run_git(checkpoints_dir, workspace_dir, "write-tree", &output, &exit_code) != 0 || exit_code != 0) {
        clay_str_free(&output);
        return -1;
    }
    char *tree = strdup(output.data);

    char *parent = NULL;
    if (run_git(checkpoints_dir, NULL, "rev-parse -q --verify " CLAY_CHECKPOINT_REF, &output, &exit_code) == 0 &&
        exit_code == 0) {
        parent = strdup(output.data);
        ClayStr parent_tree;
        clay_str_init(&parent_tree);
        clay_str_push(&parent_tree, "rev-parse -q --verify ");
        clay_str_push(&parent_tree, parent);
        clay_str_push(&parent_tree, "^{tree}");
        int same = run_git(checkpoints_dir, NULL, parent_tree.data, &output, &exit_code) == 0 && exit_code == 0 &&
                   strcmp(output.data, tree) == 0;
        clay_str_free(&parent_tree);
        if (same) {
            free(tree);
            free(parent);
            clay_str_free(&output);
            return 0;
        }
    }

    ClayStr sanitized_label;
    clay_str_init(&sanitized_label);
    for (const char *p = label && *label ? label : "checkpoint"; *p; p++) {
        clay_str_push_char(&sanitized_label, *p == '\n' || *p == '\r' ? ' ' : *p);
    }

    ClayStr commit_args;
    clay_str_init(&commit_args);
    clay_str_push(&commit_args, "commit-tree ");
    clay_str_push(&commit_args, tree);
    if (parent) {
        clay_str_push(&commit_args, " -p ");
        clay_str_push(&commit_args, parent);
    }
    clay_str_push(&commit_args, " -m ");
    clay_term_shell_quote(&commit_args, sanitized_label.data);
    clay_str_free(&sanitized_label);
    free(tree);
    free(parent);

    int ok = run_git(checkpoints_dir, NULL, commit_args.data, &output, &exit_code) == 0 && exit_code == 0;
    clay_str_free(&commit_args);
    if (!ok) {
        clay_str_free(&output);
        return -1;
    }
    char *commit = strdup(output.data);

    ClayStr update_ref;
    clay_str_init(&update_ref);
    clay_str_push(&update_ref, "update-ref " CLAY_CHECKPOINT_REF " ");
    clay_str_push(&update_ref, commit);
    ok = run_git(checkpoints_dir, NULL, update_ref.data, &output, &exit_code) == 0 && exit_code == 0;
    clay_str_free(&update_ref);
    free(commit);
    clay_str_free(&output);
    return ok ? 0 : -1;
}

void clay_checkpoint_list_free(ClayArray *checkpoints) {
    for (size_t i = 0; i < checkpoints->count; i++) {
        ClayCheckpoint *entry = clay_array_get(checkpoints, i);
        free(entry->commit);
        free(entry->label);
    }
    clay_array_free(checkpoints);
}

int clay_checkpoint_list(const char *checkpoints_dir, ClayArray *checkpoints) {
    clay_array_init(checkpoints, sizeof(ClayCheckpoint));
    ClayStr output;
    clay_str_init(&output);
    int exit_code = -1;
    /* %x1f: a field separator that can't appear in a commit subject. */
    int rc = run_git(checkpoints_dir, NULL, "log " CLAY_CHECKPOINT_REF " --format=%H%x1f%ct%x1f%s", &output,
                     &exit_code);
    if (rc != 0) {
        clay_str_free(&output);
        return -1;
    }
    if (exit_code != 0) {
        /* No checkpoints yet (unborn ref) - not an error. */
        clay_str_free(&output);
        return 0;
    }

    const char *line = output.data;
    while (*line) {
        const char *newline = strchr(line, '\n');
        size_t line_len = newline ? (size_t)(newline - line) : strlen(line);
        const char *sep1 = memchr(line, '\x1f', line_len);
        const char *sep2 = sep1 ? memchr(sep1 + 1, '\x1f', line_len - (size_t)(sep1 + 1 - line)) : NULL;
        if (sep1 && sep2) {
            ClayCheckpoint entry;
            entry.commit = dup_n(line, (size_t)(sep1 - line));
            entry.created_at = strtoll(sep1 + 1, NULL, 10);
            entry.label = dup_n(sep2 + 1, (size_t)(line + line_len - (sep2 + 1)));
            clay_array_push_val(checkpoints, &entry);
        }
        if (!newline) break;
        line = newline + 1;
    }
    clay_str_free(&output);
    return 0;
}

int clay_checkpoint_restore(const char *checkpoints_dir, const char *workspace_dir, const char *commit) {
    ClayStr output;
    clay_str_init(&output);
    int exit_code = -1;

    ClayStr read_tree_args;
    clay_str_init(&read_tree_args);
    clay_str_push(&read_tree_args, "read-tree ");
    clay_str_push(&read_tree_args, commit);
    int ok = run_git(checkpoints_dir, workspace_dir, read_tree_args.data, &output, &exit_code) == 0 &&
             exit_code == 0;
    clay_str_free(&read_tree_args);
    if (!ok) {
        clay_str_free(&output);
        return -1;
    }

    ok = run_git(checkpoints_dir, workspace_dir, "checkout-index -a -f", &output, &exit_code) == 0 && exit_code == 0;
    clay_str_free(&output);
    return ok ? 0 : -1;
}
