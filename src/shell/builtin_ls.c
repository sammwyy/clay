#include "builtin.h"

#ifdef _WIN32
#include <stdio.h>
int clay_shell_builtin_ls(ClayShellCommand *command) { (void)command; fputs("ls: unavailable in this backend\n", stderr); return 127; }
#else
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int list_path(const char *path, int show_all, int long_format) {
    DIR *directory = opendir(path);
    if (!directory) { perror(path); return 1; }
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (!show_all && entry->d_name[0] == '.') continue;
        if (!long_format) { puts(entry->d_name); continue; }
        size_t len = strlen(path) + strlen(entry->d_name) + 2;
        char *child = malloc(len);
        if (!child) { closedir(directory); return 1; }
        snprintf(child, len, "%s/%s", path, entry->d_name);
        struct stat info;
        if (!stat(child, &info))
            printf("%c %8lld %s\n", S_ISDIR(info.st_mode) ? 'd' : '-',
                   (long long)info.st_size, entry->d_name);
        free(child);
    }
    closedir(directory);
    return 0;
}

int clay_shell_builtin_ls(ClayShellCommand *command) {
    static const char *const LONG[] = {"all", "long", NULL};
    ClayShellOptions options;
    if (clay_shell_options(command, "al", LONG, &options)) return 1;
    int status = 0;
    if (options.first == command->argc)
        return list_path(".", clay_shell_option_seen(&options, 'a'), clay_shell_option_seen(&options, 'l'));
    for (size_t i = options.first; i < command->argc; i++)
        if (list_path(command->argv[i], clay_shell_option_seen(&options, 'a'),
                      clay_shell_option_seen(&options, 'l'))) status = 1;
    return status;
}
#endif
