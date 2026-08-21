#include "clay/cli.h"

#include "clay/array.h"
#include "clay/str.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *name;
    char *description;
    ClayCliValueType type;
    int boolean;
    char *string;
    double number;
} ClayCliOption;

struct ClayCli {
    char *program;
    char *description;
    ClayArray options;     /* ClayCliOption */
    ClayArray positional;  /* char * */
    ClayStr error;
};

static ClayCliOption *find_option(const ClayCli *cli, const char *name) {
    for (size_t i = 0; i < cli->options.count; i++) {
        ClayCliOption *option = clay_array_get((ClayArray *)&cli->options, i);
        if (strcmp(option->name, name) == 0) return option;
    }
    return NULL;
}

static void positional_clear(ClayCli *cli) {
    for (size_t i = 0; i < cli->positional.count; i++) {
        free(*(char **)clay_array_get(&cli->positional, i));
    }
    clay_array_clear(&cli->positional);
}

static void reset_values(ClayCli *cli) {
    clay_str_clear(&cli->error);
    positional_clear(cli);
    for (size_t i = 0; i < cli->options.count; i++) {
        ClayCliOption *option = clay_array_get(&cli->options, i);
        option->boolean = 0;
        free(option->string);
        option->string = NULL;
        option->number = 0;
    }
}

ClayCli *clay_cli_create(const char *program, const char *description) {
    ClayCli *cli = malloc(sizeof(ClayCli));
    cli->program = strdup(program ? program : "clay");
    cli->description = strdup(description ? description : "");
    clay_array_init(&cli->options, sizeof(ClayCliOption));
    clay_array_init(&cli->positional, sizeof(char *));
    clay_str_init(&cli->error);
    return cli;
}

void clay_cli_destroy(ClayCli *cli) {
    if (!cli) return;
    for (size_t i = 0; i < cli->options.count; i++) {
        ClayCliOption *option = clay_array_get(&cli->options, i);
        free(option->name);
        free(option->description);
        free(option->string);
    }
    positional_clear(cli);
    clay_array_free(&cli->options);
    clay_array_free(&cli->positional);
    clay_str_free(&cli->error);
    free(cli->program);
    free(cli->description);
    free(cli);
}

static void add_option(ClayCli *cli, const char *name, const char *description, ClayCliValueType type) {
    ClayCliOption option = {0};
    option.name = strdup(name);
    option.description = strdup(description ? description : "");
    option.type = type;
    clay_array_push_val(&cli->options, &option);
}

void clay_cli_add_bool(ClayCli *cli, const char *name, const char *description) {
    add_option(cli, name, description, CLAY_CLI_BOOL);
}

void clay_cli_add_string(ClayCli *cli, const char *name, const char *description) {
    add_option(cli, name, description, CLAY_CLI_STRING);
}

void clay_cli_add_number(ClayCli *cli, const char *name, const char *description) {
    add_option(cli, name, description, CLAY_CLI_NUMBER);
}

static int set_error(ClayCli *cli, const char *message, const char *value) {
    clay_str_printf(&cli->error, "%s%s", message, value ? value : "");
    return -1;
}

static int set_option(ClayCli *cli, ClayCliOption *option, const char *value) {
    if (option->type == CLAY_CLI_BOOL) {
        if (!value || strcmp(value, "true") == 0 || strcmp(value, "1") == 0) {
            option->boolean = 1;
            return 0;
        }
        if (strcmp(value, "false") == 0 || strcmp(value, "0") == 0) {
            option->boolean = 0;
            return 0;
        }
        return set_error(cli, "Invalid boolean value: ", value);
    }

    if (!value || !*value) return set_error(cli, "Missing value for --", option->name);
    if (option->type == CLAY_CLI_STRING) {
        free(option->string);
        option->string = strdup(value);
        return 0;
    }

    char *end = NULL;
    double number = strtod(value, &end);
    if (*end != '\0') return set_error(cli, "Invalid number: ", value);
    option->number = number;
    return 0;
}

int clay_cli_parse(ClayCli *cli, int argc, char **argv) {
    reset_values(cli);
    int positional_only = 0;

    for (int i = 1; i < argc; i++) {
        const char *argument = argv[i];
        if (!positional_only && strcmp(argument, "--") == 0) {
            positional_only = 1;
            continue;
        }
        if (positional_only || argument[0] != '-' || argument[1] == '\0') {
            char *copy = strdup(argument);
            clay_array_push_val(&cli->positional, &copy);
            continue;
        }

        const char *name = argument;
        while (*name == '-') name++;

        ClayStr name_buffer;
        clay_str_init(&name_buffer);
        const char *value = NULL;
        const char *equals = strchr(name, '=');
        if (equals) {
            clay_str_push_n(&name_buffer, name, (size_t)(equals - name));
            value = equals + 1;
        } else {
            clay_str_push(&name_buffer, name);
        }

        ClayCliOption *option = find_option(cli, name_buffer.data);
        int disable = 0;
        if (!option && strncmp(name_buffer.data, "no-", 3) == 0) {
            option = find_option(cli, name_buffer.data + 3);
            disable = 1;
        }
        if (!option) {
            clay_str_free(&name_buffer);
            return set_error(cli, "Unknown option: ", argument);
        }
        if (disable) {
            if (option->type != CLAY_CLI_BOOL || value) {
                clay_str_free(&name_buffer);
                return set_error(cli, "Invalid negated option: ", argument);
            }
            option->boolean = 0;
            clay_str_free(&name_buffer);
            continue;
        }

        if (option->type != CLAY_CLI_BOOL && !value) {
            if (i + 1 >= argc) {
                clay_str_free(&name_buffer);
                return set_error(cli, "Missing value for --", option->name);
            }
            value = argv[++i];
        }
        int rc = set_option(cli, option, value);
        clay_str_free(&name_buffer);
        if (rc != 0) return rc;
    }
    return 0;
}

const char *clay_cli_error(const ClayCli *cli) {
    return cli->error.data;
}

int clay_cli_bool(const ClayCli *cli, const char *name) {
    ClayCliOption *option = find_option(cli, name);
    return option && option->type == CLAY_CLI_BOOL ? option->boolean : 0;
}

const char *clay_cli_string(const ClayCli *cli, const char *name) {
    ClayCliOption *option = find_option(cli, name);
    return option && option->type == CLAY_CLI_STRING ? option->string : NULL;
}

double clay_cli_number(const ClayCli *cli, const char *name) {
    ClayCliOption *option = find_option(cli, name);
    return option && option->type == CLAY_CLI_NUMBER ? option->number : 0;
}

const char *clay_cli_command(const ClayCli *cli) {
    return clay_cli_argument_count(cli) > 0 ? clay_cli_argument_get(cli, 0) : NULL;
}

size_t clay_cli_argument_count(const ClayCli *cli) {
    return cli->positional.count;
}

const char *clay_cli_argument_get(const ClayCli *cli, size_t index) {
    if (index >= cli->positional.count) return NULL;
    return *(char **)clay_array_get((ClayArray *)&cli->positional, index);
}

static const char *type_hint(ClayCliValueType type) {
    switch (type) {
        case CLAY_CLI_STRING: return " <string>";
        case CLAY_CLI_NUMBER: return " <number>";
        case CLAY_CLI_BOOL:
        default: return "";
    }
}

void clay_cli_print_help(const ClayCli *cli) {
    printf("Usage: %s [options] [command] [arguments...]\n", cli->program);
    if (*cli->description) printf("\n%s\n", cli->description);
    if (cli->options.count == 0) return;

    size_t width = 0;
    for (size_t i = 0; i < cli->options.count; i++) {
        ClayCliOption *option = clay_array_get((ClayArray *)&cli->options, i);
        size_t current = strlen(option->name) + 2 + strlen(type_hint(option->type));
        if (current > width) width = current;
    }

    printf("\nOptions:\n");
    for (size_t i = 0; i < cli->options.count; i++) {
        ClayCliOption *option = clay_array_get((ClayArray *)&cli->options, i);
        printf("  --%s%s", option->name, type_hint(option->type));
        size_t current = strlen(option->name) + 2 + strlen(type_hint(option->type));
        for (size_t pad = current; pad < width + 2; pad++) fputc(' ', stdout);
        printf("%s\n", option->description);
    }
}
