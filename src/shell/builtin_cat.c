#include "builtin.h"

#ifdef _WIN32
#include <stdio.h>
int clay_shell_builtin_cat(ClayShellCommand *command) { (void)command; fputs("cat: unavailable in this backend\n", stderr); return 127; }
#else
#include <stdio.h>
#include <string.h>

static int copy_stream(FILE *file, int numbered, unsigned long *line) {
    int ch, start = 1;
    while ((ch = fgetc(file)) != EOF) {
        if (numbered && start) printf("%6lu\t", (*line)++);
        fputc(ch, stdout);
        start = ch == '\n';
    }
    return ferror(file) ? 1 : 0;
}

int clay_shell_builtin_cat(ClayShellCommand *command) {
    static const char *const LONG[] = {"number", NULL};
    ClayShellOptions options;
    if (clay_shell_options(command, "n", LONG, &options)) return 1;
    int numbered = clay_shell_option_seen(&options, 'n'), status = 0;
    unsigned long line = 1;
    if (options.first == command->argc) return copy_stream(stdin, numbered, &line);
    for (size_t i = options.first; i < command->argc; i++) {
        FILE *file = !strcmp(command->argv[i], "-") ? stdin : fopen(command->argv[i], "rb");
        if (!file) { perror(command->argv[i]); status = 1; continue; }
        if (copy_stream(file, numbered, &line)) status = 1;
        if (file != stdin) fclose(file);
    }
    return status;
}
#endif
