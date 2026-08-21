#include "context.h"

#include <stdlib.h>

void clay_cmd_choice(const char *args, void *user_data) {
    (void)args;
    ClayCommands *commands = user_data;
    ClayChoice options[] = {
        {"Commit changes", "git commit the staged edits"},
        {"Show diff", "preview without applying"},
        {"Discard changes", "revert all edits, no undo"},
    };
    char *custom = NULL;
    int index = clay_app_choice(commands->app, "What next?", options, 3, 1, &custom);
    if (index >= 0) {
        clay_sayc(CLAY_CYAN, "You picked: %s", options[index].title);
    } else if (custom) {
        clay_sayc(CLAY_CYAN, "You typed: %s", custom);
        free(custom);
    }
}
