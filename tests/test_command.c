#include "clay/command.h"

#include <assert.h>
#include <string.h>

typedef struct {
    int groups;
    int connect_seen;
} GroupCheck;

static void handler(const char *args, void *user_data) {
    (void)args;
    (void)user_data;
}

static void check_group(const ClayCommandGroup *group, void *user_data) {
    GroupCheck *check = user_data;
    check->groups++;
    if (strcmp(group->name, "connect") != 0) return;

    check->connect_seen = 1;
    assert(strcmp(group->description, "Connect a provider") == 0);
    assert(group->alias_count == 2);
    assert(strcmp(group->aliases[0], "provider") == 0);
    assert(strcmp(group->aliases[1], "login") == 0);
}

int main(void) {
    ClayCommandRegistry *commands = clay_command_registry_create();
    int connect_context = 0;
    int help_context = 0;

    /* Alias registration may precede its visible command in the real
       registry, so grouping must not depend on registration order. */
    clay_command_register_alias(commands, "provider", handler, &connect_context);
    clay_command_register(commands, "connect", "Connect a provider", handler, &connect_context);
    clay_command_register_alias(commands, "login", handler, &connect_context);
    clay_command_register(commands, "help", "Show help", handler, &help_context);

    GroupCheck check = {0};
    clay_command_foreach_group(commands, check_group, &check);
    assert(check.groups == 2);
    assert(check.connect_seen);

    clay_command_registry_destroy(commands);
    return 0;
}
