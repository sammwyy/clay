#include "context.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_entry(const char *slug, const char *content) {
    clay_sayc(CLAY_CYAN, "%s:", slug);
    if (content && *content) printf("%s\n", content);
    else clay_list_bullet("(empty)");
}

void clay_cmd_memory(const char *args, void *user_data) {
    ClayCommands *commands = user_data;

    if (args && strncmp(args, "forget ", 7) == 0) {
        const char *slug = args + 7;
        while (*slug == ' ') slug++;
        if (clay_memory_delete(slug) == 0) clay_sayc(CLAY_GREEN, "Forgot %s.", slug);
        else clay_sayc(CLAY_RED, "%s isn't a valid memory slug.", slug);
        return;
    }

    if (args && *args) {
        char *content = clay_memory_read(args);
        if (!content) {
            clay_sayc(CLAY_RED, "No memory entry named %s.", args);
            return;
        }
        print_entry(args, content);
        free(content);
        return;
    }

    ClayArray entries;
    clay_memory_list(&entries);
    if (entries.count == 0) {
        clay_sayc(CLAY_GRAY, "No long-term memory yet.");
        clay_memory_entries_free(&entries);
        return;
    }

    ClayChoice choices[entries.count];
    ClayStr titles[entries.count];
    ClayStr descriptions[entries.count];
    long long now = clay_time_now();
    for (size_t i = 0; i < entries.count; i++) {
        ClayMemoryEntry *entry = clay_array_get(&entries, i);
        char *relative = clay_time_relative(entry->updated_at, now);
        clay_str_init(&titles[i]);
        clay_str_init(&descriptions[i]);
        clay_str_printf(&titles[i], "%s [%s]", entry->slug, entry->type);
        clay_str_printf(&descriptions[i], "%s \xc2\xb7 %s", entry->summary, relative);
        free(relative);
        choices[i].title = titles[i].data;
        choices[i].desc = descriptions[i].data;
    }
    int index = clay_app_choice(commands->app, "Long-term memory (/memory forget <slug> to delete one):", choices,
                                (int)entries.count, 0, NULL);
    for (size_t i = 0; i < entries.count; i++) {
        clay_str_free(&titles[i]);
        clay_str_free(&descriptions[i]);
    }
    if (index >= 0) {
        ClayMemoryEntry *entry = clay_array_get(&entries, (size_t)index);
        char *content = clay_memory_read(entry->slug);
        print_entry(entry->slug, content);
        free(content);
    }
    clay_memory_entries_free(&entries);
}
