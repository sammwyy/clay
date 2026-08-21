#include "clay/command.h"

#include "clay/array.h"
#include "clay/map.h"
#include "clay/str.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *name;
    char *description;
    ClayCommandHandler handler;
    void *user_data;
} ClayCommandEntry;

struct ClayCommandRegistry {
    ClayMap *by_name;
    ClayArray order; /* ClayCommandEntry*, kept in registration order for listings */
};

ClayCommandRegistry *clay_command_registry_create(void) {
    ClayCommandRegistry *reg = malloc(sizeof(ClayCommandRegistry));
    reg->by_name = clay_map_create();
    clay_array_init(&reg->order, sizeof(ClayCommandEntry *));
    return reg;
}

void clay_command_registry_destroy(ClayCommandRegistry *reg) {
    if (!reg) return;

    for (size_t i = 0; i < reg->order.count; i++) {
        ClayCommandEntry *entry = *(ClayCommandEntry **)clay_array_get(&reg->order, i);
        free(entry->name);
        free(entry->description);
        free(entry);
    }
    clay_array_free(&reg->order);
    clay_map_destroy(reg->by_name);
    free(reg);
}

void clay_command_register(ClayCommandRegistry *reg, const char *name, const char *description,
                            ClayCommandHandler handler, void *user_data) {
    ClayCommandEntry *entry = malloc(sizeof(ClayCommandEntry));
    entry->name = strdup(name);
    entry->description = strdup(description ? description : "");
    entry->handler = handler;
    entry->user_data = user_data;

    clay_map_set(reg->by_name, name, entry);
    clay_array_push_val(&reg->order, &entry);
}

void clay_command_foreach(ClayCommandRegistry *reg, ClayCommandVisitor visitor, void *ctx) {
    for (size_t i = 0; i < reg->order.count; i++) {
        ClayCommandEntry *entry = *(ClayCommandEntry **)clay_array_get(&reg->order, i);
        visitor(entry->name, entry->description, ctx);
    }
}

ClayInput clay_input_parse(const char *line) {
    ClayInput input = {0};
    while (isspace((unsigned char)*line)) line++;

    if (*line == '\0') {
        input.kind = CLAY_INPUT_EMPTY;
        return input;
    }

    if (*line == '/') {
        line++;
        ClayStr name;
        clay_str_init(&name);
        while (*line && !isspace((unsigned char)*line)) {
            clay_str_push_char(&name, *line++);
        }
        while (isspace((unsigned char)*line)) line++;

        input.kind = CLAY_INPUT_COMMAND;
        input.command = name.data;
        input.args = strdup(line);
    } else {
        input.kind = CLAY_INPUT_MESSAGE;
        input.raw = strdup(line);
    }

    return input;
}

int clay_command_dispatch(ClayCommandRegistry *reg, const ClayInput *input) {
    if (input->kind != CLAY_INPUT_COMMAND) return 0;

    ClayCommandEntry *entry = clay_map_get(reg->by_name, input->command);
    if (!entry) return 0;

    entry->handler(input->args, entry->user_data);
    return 1;
}

void clay_input_free(ClayInput *input) {
    free(input->command);
    free(input->args);
    free(input->raw);
    input->command = input->args = input->raw = NULL;
}
