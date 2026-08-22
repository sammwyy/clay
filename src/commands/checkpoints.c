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

    ClayArray choices, titles, descriptions;
    clay_array_init(&choices, sizeof(ClayChoice));
    clay_array_init(&titles, sizeof(ClayStr));
    clay_array_init(&descriptions, sizeof(ClayStr));
    long long now = clay_time_now();
    for (size_t i = 0; i < checkpoints.count; i++) {
        ClayCheckpoint *checkpoint = clay_array_get(&checkpoints, i);
        char *relative = clay_time_relative(checkpoint->created_at, now);
        ClayStr title, description;
        clay_str_init(&title);
        clay_str_init(&description);
        clay_str_push(&title, checkpoint->label);
        clay_str_push(&description, relative);
        free(relative);
        clay_array_push_val(&titles, &title);
        clay_array_push_val(&descriptions, &description);
        ClayChoice choice = {title.data, description.data};
        clay_array_push_val(&choices, &choice);
    }
    int index = clay_app_choice(commands->app, "Restore a checkpoint (overwrites files changed since):", choices.data,
                                (int)choices.count, 0, NULL);
    for (size_t i = 0; i < titles.count; i++) {
        clay_str_free(clay_array_get(&titles, i));
        clay_str_free(clay_array_get(&descriptions, i));
    }
    clay_array_free(&choices);
    clay_array_free(&titles);
    clay_array_free(&descriptions);

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
