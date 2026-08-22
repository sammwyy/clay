#include "builtin.h"

#include <stdio.h>
#include <string.h>

static unsigned long long flag_bit(char flag) {
    if (flag >= 'a' && flag <= 'z') return 1ULL << (flag - 'a');
    if (flag >= 'A' && flag <= 'Z') return 1ULL << (26 + flag - 'A');
    return 0;
}

int clay_shell_option_seen(const ClayShellOptions *options, char flag) {
    return (options->flags & flag_bit(flag)) != 0;
}

int clay_shell_options(const ClayShellCommand *command, const char *short_flags,
                       const char *const *long_flags, ClayShellOptions *options) {
    memset(options, 0, sizeof(*options));
    size_t i = 1;
    for (; i < command->argc; i++) {
        const char *arg = command->argv[i];
        if (!strcmp(arg, "--")) { i++; break; }
        if (arg[0] != '-' || !arg[1]) break;
        if (arg[1] == '-') {
            const char *name = arg + 2;
            size_t index = 0;
            while (long_flags && long_flags[index] && strcmp(name, long_flags[index])) index++;
            if (!long_flags || !long_flags[index]) {
                fprintf(stderr, "%s: unrecognized option '--%s'\n", command->argv[0], name);
                return -1;
            }
            char flag = long_flags[index][0];
            options->flags |= flag_bit(flag);
            continue;
        }
        for (const char *flag = arg + 1; *flag; flag++) {
            if (!strchr(short_flags, *flag)) {
                fprintf(stderr, "%s: invalid option -- '%c'\n", command->argv[0], *flag);
                return -1;
            }
            options->flags |= flag_bit(*flag);
        }
    }
    options->first = i;
    return 0;
}
