#include "builtin.h"

#ifdef _WIN32
#include <stdio.h>
int clay_shell_copy_file(const char *a, const char *b, int c) { (void)a; (void)b; (void)c; return 127; }
int clay_shell_remove_path(const char *a, int b, int c) { (void)a; (void)b; (void)c; return 127; }
int clay_shell_builtin_rm(ClayShellCommand *c) { (void)c; return 127; }
int clay_shell_builtin_cp(ClayShellCommand *c) { (void)c; return 127; }
int clay_shell_builtin_mv(ClayShellCommand *c) { (void)c; return 127; }
int clay_shell_builtin_mkdir(ClayShellCommand *c) { (void)c; return 127; }
int clay_shell_builtin_touch(ClayShellCommand *c) { (void)c; return 127; }
#else
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <utime.h>
#include <unistd.h>

int clay_shell_copy_file(const char *source, const char *destination, int no_clobber) {
    if (no_clobber) { FILE *existing = fopen(destination, "rb"); if (existing) { fclose(existing); return 0; } }
    FILE *in = fopen(source, "rb");
    if (!in) { perror(source); return 1; }
    FILE *out = fopen(destination, "wb");
    if (!out) { perror(destination); fclose(in); return 1; }
    char buffer[32768]; size_t count; int status = 0;
    while ((count = fread(buffer, 1, sizeof(buffer), in)) && fwrite(buffer, 1, count, out) != count) { perror(destination); status = 1; break; }
    if (ferror(in)) { perror(source); status = 1; }
    fclose(in); if (fclose(out)) status = 1;
    return status;
}

int clay_shell_remove_path(const char *path, int recursive, int force) {
    struct stat info;
    if (lstat(path, &info)) { if (!force) perror(path); return force && errno == ENOENT ? 0 : 1; }
    if (!S_ISDIR(info.st_mode)) { if (unlink(path)) { if (!force) perror(path); return 1; } return 0; }
    if (!recursive) { fprintf(stderr, "rm: cannot remove '%s': Is a directory\n", path); return 1; }
    DIR *directory = opendir(path); if (!directory) { perror(path); return 1; }
    int status = 0; struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        size_t len = strlen(path) + strlen(entry->d_name) + 2; char *child = malloc(len);
        if (!child) { status = 1; break; }
        snprintf(child, len, "%s/%s", path, entry->d_name);
        if (clay_shell_remove_path(child, 1, force)) status = 1;
        free(child);
    }
    closedir(directory);
    if (!status && rmdir(path)) { if (!force) perror(path); status = 1; }
    return status;
}

int clay_shell_builtin_rm(ClayShellCommand *command) {
    static const char *const LONG[] = {"recursive", "force", NULL};
    ClayShellOptions options;
    if (clay_shell_options(command, "rfR", LONG, &options)) return 1;
    if (options.first == command->argc) { fputs("rm: missing operand\n", stderr); return 1; }
    int recursive = clay_shell_option_seen(&options, 'r') || clay_shell_option_seen(&options, 'R');
    int force = clay_shell_option_seen(&options, 'f'), status = 0;
    for (size_t i = options.first; i < command->argc; i++) {
        const char *path = command->argv[i];
        if (!strcmp(path, "/") || !strcmp(path, ".") || !strcmp(path, "..")) { fprintf(stderr, "rm: refusing dangerous target '%s'\n", path); status = 1; continue; }
        if (clay_shell_remove_path(path, recursive, force)) status = 1;
    }
    return status;
}

static int two_paths(ClayShellCommand *command, ClayShellOptions *options, const char *flags,
                     const char *const *long_flags, const char **source, const char **destination) {
    if (clay_shell_options(command, flags, long_flags, options)) return 0;
    if (command->argc - options->first != 2) { fprintf(stderr, "%s: expected SOURCE DESTINATION\n", command->argv[0]); return 0; }
    *source = command->argv[options->first]; *destination = command->argv[options->first + 1]; return 1;
}

int clay_shell_builtin_cp(ClayShellCommand *command) {
    static const char *const LONG[] = {"no-clobber", NULL}; ClayShellOptions options; const char *source, *destination;
    return two_paths(command, &options, "fn", LONG, &source, &destination) ? clay_shell_copy_file(source, destination, clay_shell_option_seen(&options, 'n')) : 1;
}
int clay_shell_builtin_mv(ClayShellCommand *command) {
    static const char *const LONG[] = {"no-clobber", NULL}; ClayShellOptions options; const char *source, *destination;
    if (!two_paths(command, &options, "fn", LONG, &source, &destination)) return 1;
    if (clay_shell_option_seen(&options, 'n')) { FILE *existing = fopen(destination, "rb"); if (existing) { fclose(existing); return 0; } }
    if (!rename(source, destination)) return 0;
    if (clay_shell_copy_file(source, destination, 0)) return 1;
    return clay_shell_remove_path(source, 0, 0);
}
static int make_directory(const char *path, int parents) {
    if (!parents) return mkdir(path, 0777) == 0 ? 0 : -1;
    char *partial = strdup(path);
    if (!partial) return -1;
    for (char *p = partial + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (*partial && mkdir(partial, 0777) && errno != EEXIST) { *p = '/'; free(partial); return -1; }
        *p = '/';
    }
    int rc = mkdir(partial, 0777);
    free(partial);
    return rc == 0 || errno == EEXIST ? 0 : -1;
}
int clay_shell_builtin_mkdir(ClayShellCommand *command) {
    static const char *const LONG[] = {"parents", NULL}; ClayShellOptions options;
    if (clay_shell_options(command, "p", LONG, &options)) return 1;
    if (options.first == command->argc) { fputs("mkdir: missing operand\n", stderr); return 1; }
    int status = 0, parents = clay_shell_option_seen(&options, 'p');
    for (size_t i = options.first; i < command->argc; i++)
        if (make_directory(command->argv[i], parents)) { perror(command->argv[i]); status = 1; }
    return status;
}
int clay_shell_builtin_touch(ClayShellCommand *command) {
    ClayShellOptions options;
    if (clay_shell_options(command, "", NULL, &options)) return 1;
    if (options.first == command->argc) { fputs("touch: missing file operand\n", stderr); return 1; }
    int status = 0;
    for (size_t i = options.first; i < command->argc; i++) { FILE *file = fopen(command->argv[i], "ab"); if (!file) { perror(command->argv[i]); status = 1; continue; } fclose(file); if (utime(command->argv[i], NULL)) { perror(command->argv[i]); status = 1; } }
    return status;
}
#endif
