#include "context.h"

#include <stdlib.h>

void clay_cmd_checkpoints(const char *args, void *user_data) {
    (void)args;
    ClayCommands *commands = user_data;
    if (!commands->chat) {
        clay_sayc(CLAY_GRAY, "No checkpoints yet - nothing has changed files in this chat.");
        return;
    }
    char *checkpoints_dir = clay_chat_checkpoints_dir(commands->chat);
    if (!checkpoints_dir) {
        clay_sayc(CLAY_RED, "Could not access this chat's checkpoints.");
        return;
    }

    ClayArray checkpoints;
    if (clay_checkpoint_list(checkpoints_dir, &checkpoints) != 0) {
        free(checkpoints_dir);
        clay_sayc(CLAY_RED, "Could not read checkpoints.");
        return;
    }
    if (checkpoints.count == 0) {
        clay_checkpoint_list_free(&checkpoints);
        free(checkpoints_dir);
        clay_sayc(CLAY_GRAY, "No checkpoints yet - nothing has changed files in this chat.");
        return;
    }

    ClayChoice choices[checkpoints.count];
    ClayStr titles[checkpoints.count];
    ClayStr descriptions[checkpoints.count];
    long long now = clay_time_now();
    for (size_t i = 0; i < checkpoints.count; i++) {
        ClayCheckpoint *checkpoint = clay_array_get(&checkpoints, i);
        char *relative = clay_time_relative(checkpoint->created_at, now);
        clay_str_init(&titles[i]);
        clay_str_init(&descriptions[i]);
        clay_str_push(&titles[i], checkpoint->label);
        clay_str_push(&descriptions[i], relative);
        free(relative);
        choices[i].title = titles[i].data;
        choices[i].desc = descriptions[i].data;
    }
    int index = clay_app_choice(commands->app, "Restore a checkpoint (overwrites files changed since):", choices,
                                (int)checkpoints.count, 0, NULL);
    for (size_t i = 0; i < checkpoints.count; i++) {
        clay_str_free(&titles[i]);
        clay_str_free(&descriptions[i]);
    }

    if (index >= 0 &&
        clay_app_confirm(commands->app, "Restore this checkpoint? Files changed since will be overwritten.", 0)) {
        ClayCheckpoint *checkpoint = clay_array_get(&checkpoints, (size_t)index);
        char *workspace_dir = clay_term_cwd();
        /* Snapshot the current state first, so restoring is itself undoable. */
        clay_checkpoint_save(checkpoints_dir, workspace_dir, "before restore");
        if (clay_checkpoint_restore(checkpoints_dir, workspace_dir, checkpoint->commit) == 0) {
            clay_sayc(CLAY_GREEN, "Restored: %s", checkpoint->label);
        } else {
            clay_sayc(CLAY_RED, "Could not restore that checkpoint.");
        }
        free(workspace_dir);
    }
    clay_checkpoint_list_free(&checkpoints);
    free(checkpoints_dir);
}
