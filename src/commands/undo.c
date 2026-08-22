#include "context.h"

#include "clay/storage.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLAY_UNDO_FILE_LIMIT (4 * 1024 * 1024)
#define CLAY_UNDO_HISTORY_LIMIT 16

static void undo_entry_free(ClayUndoEntry *entry) {
    if (!entry) return;
    free(entry->path);
    free(entry->before);
    free(entry->after);
    memset(entry, 0, sizeof(*entry));
}

/* Returns 0 for a readable file or a known-absent file, -1 for an unreadable
   or oversized file. A missing file is represented by exists == 0. */
static int read_snapshot(const char *path, char **data, size_t *len, int *exists) {
    ClayStr body;
    if (clay_storage_read_limited(path, CLAY_UNDO_FILE_LIMIT, &body) == 0) {
        *data = body.data;
        *len = body.len;
        *exists = 1;
        return 0;
    }
    if (errno == ENOENT) {
        *data = NULL;
        *len = 0;
        *exists = 0;
        return 0;
    }
    return -1;
}

static int snapshot_matches(const char *path, const char *expected,
                            size_t expected_len, int expected_exists) {
    char *current = NULL;
    size_t current_len = 0;
    int current_exists = 0;
    if (read_snapshot(path, &current, &current_len, &current_exists) != 0)
        return 0;
    int matches = current_exists == expected_exists &&
                  (!current_exists ||
                   (current_len == expected_len &&
                    memcmp(current, expected, expected_len) == 0));
    free(current);
    return matches;
}

int clay_commands_undo_prepare(ClayCommands *commands, const char *path) {
    if (!commands || !path || !*path) return 0;
    clay_commands_undo_discard(commands);

    char *workspace = clay_term_cwd();
    if (!workspace) return 0;
    ClayStr absolute;
    int resolved = clay_fs_resolve_workspace_path(workspace, path, &absolute);
    free(workspace);
    if (resolved != 0) {
        clay_str_free(&absolute);
        return 0;
    }

    ClayUndoEntry pending = {0};
    pending.path = strdup(absolute.data);
    int read_ok = pending.path &&
                  read_snapshot(absolute.data, &pending.before,
                                &pending.before_len,
                                &pending.before_exists) == 0;
    clay_str_free(&absolute);
    if (!read_ok) {
        undo_entry_free(&pending);
        return 0;
    }
    commands->undo_pending = pending;
    commands->undo_pending_valid = 1;
    return 1;
}

void clay_commands_undo_commit(ClayCommands *commands) {
    if (!commands || !commands->undo_pending_valid) return;
    ClayUndoEntry *pending = &commands->undo_pending;
    if (read_snapshot(pending->path, &pending->after, &pending->after_len,
                      &pending->after_exists) != 0) {
        clay_commands_undo_discard(commands);
        return;
    }
    while (commands->undo_history.count >= CLAY_UNDO_HISTORY_LIMIT) {
        ClayUndoEntry *oldest = clay_array_get(&commands->undo_history, 0);
        undo_entry_free(oldest);
        clay_array_remove(&commands->undo_history, 0);
    }
    clay_array_push_val(&commands->undo_history, pending);
    memset(pending, 0, sizeof(*pending));
    commands->undo_pending_valid = 0;
}

void clay_commands_undo_discard(ClayCommands *commands) {
    if (!commands) return;
    if (commands->undo_pending_valid || commands->undo_pending.path)
        undo_entry_free(&commands->undo_pending);
    commands->undo_pending_valid = 0;
}

void clay_commands_undo_destroy(ClayCommands *commands) {
    if (!commands) return;
    clay_commands_undo_discard(commands);
    for (size_t i = 0; i < commands->undo_history.count; i++)
        undo_entry_free(clay_array_get(&commands->undo_history, i));
    clay_array_free(&commands->undo_history);
}

void clay_cmd_undo(const char *args, void *user_data) {
    (void)args;
    ClayCommands *commands = user_data;
    if (commands->mode == CLAY_MODE_PLAN) {
        clay_sayc(CLAY_YELLOW,
                  "Undo is disabled in Plan mode; switch to Act mode with /plan.");
        return;
    }
    if (commands->undo_history.count == 0) {
        clay_sayc(CLAY_GRAY, "Nothing to undo from this session.");
        return;
    }

    ClayUndoEntry *entry = clay_array_get(&commands->undo_history,
                                          commands->undo_history.count - 1);
    if (!snapshot_matches(entry->path, entry->after, entry->after_len,
                           entry->after_exists)) {
        clay_sayc(CLAY_RED,
                  "Cannot undo %s: it changed after the last write/edit.",
                  entry->path);
        return;
    }

    int rc;
    if (entry->before_exists) {
        rc = clay_storage_write_atomic_private(entry->path, entry->before,
                                               entry->before_len);
    } else {
        rc = remove(entry->path);
    }
    if (rc != 0) {
        clay_sayc(CLAY_RED, "Could not undo %s: %s", entry->path,
                  strerror(errno));
        return;
    }

    clay_sayc(CLAY_GREEN, "Undid last edit: %s", entry->path);
    undo_entry_free(entry);
    clay_array_remove(&commands->undo_history,
                      commands->undo_history.count - 1);
}
