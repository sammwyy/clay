#ifndef CLAY_CLI_H
#define CLAY_CLI_H

#include <stddef.h>

typedef struct ClayCli ClayCli;

typedef enum {
    CLAY_CLI_BOOL,
    CLAY_CLI_STRING,
    CLAY_CLI_NUMBER
} ClayCliValueType;

/* Program and description are copied. */
ClayCli *clay_cli_create(const char *program, const char *description);
void clay_cli_destroy(ClayCli *cli);

/* Names omit leading dashes. Each option is initialized to false, NULL,
   or zero. */
void clay_cli_add_bool(ClayCli *cli, const char *name, const char *description);
void clay_cli_add_string(ClayCli *cli, const char *name, const char *description);
void clay_cli_add_number(ClayCli *cli, const char *name, const char *description);

/* Parses named options from argv. Both --name and -name are accepted.
   Boolean options become true when present; strings and numbers require
   a value. Returns 0 on success, -1 on invalid input. */
int clay_cli_parse(ClayCli *cli, int argc, char **argv);
const char *clay_cli_error(const ClayCli *cli);

int clay_cli_bool(const ClayCli *cli, const char *name);
const char *clay_cli_string(const ClayCli *cli, const char *name);
double clay_cli_number(const ClayCli *cli, const char *name);

/* Positional values are retained in order: the first is the command,
   followed by its arguments. */
const char *clay_cli_command(const ClayCli *cli);
size_t clay_cli_argument_count(const ClayCli *cli);
const char *clay_cli_argument_get(const ClayCli *cli, size_t index);

void clay_cli_print_help(const ClayCli *cli);

/* Registers clay's process-level options, prints help or version when
   requested, and applies the selected working directory. Returns 0 when
   the agent should start, 1 after a successful early exit, or -1 on error. */
int clay_cli_startup(int argc, char **argv, const char *version);

/* As above, also returns a malloc'd one-shot prompt when -p/--prompt was
   supplied. The caller owns *prompt_out. */
int clay_cli_startup_with_prompt(int argc, char **argv, const char *version,
                                 char **prompt_out);

#endif /* CLAY_CLI_H */
