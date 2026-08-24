#include "context.h"

#include "clay/skill.h"

#include <stdlib.h>
#include <string.h>

static void print_installed(void) {
    ClayArray entries;
    clay_skill_list(&entries);
    if (entries.count == 0) {
        clay_sayc(CLAY_GRAY, "No skills installed. Add one with install <path-to-SKILL.md-or-its-dir>.");
        clay_skill_entries_free(&entries);
        return;
    }
    clay_list_header("Installed skills:");
    for (size_t i = 0; i < entries.count; i++) {
        ClaySkillEntry *entry = clay_array_get(&entries, i);
        ClayStr line;
        clay_str_init(&line);
        clay_str_printf(&line, "%s%s", entry->description, entry->enabled ? "" : " (disabled)");
        clay_list_step((int)(i + 1), entry->name, line.data, NULL, 0);
        clay_str_free(&line);
    }
    clay_skill_entries_free(&entries);
}

/* Shared by the interactive /skill command and `clay skill` on the CLI.
   0 on success, 1 on a usage error or failed operation. */
static int skill_command(const char *args) {
    if (args && strncmp(args, "install ", 8) == 0) {
        const char *rest = args + 8;
        while (*rest == ' ') rest++;
        ClayStr path, name;
        clay_str_init(&path);
        clay_str_init(&name);
        const char *space = strchr(rest, ' ');
        if (space) {
            clay_str_push_n(&path, rest, (size_t)(space - rest));
            const char *name_start = space;
            while (*name_start == ' ') name_start++;
            clay_str_push(&name, name_start);
        } else {
            clay_str_push(&path, rest);
        }
        int ok;
        if (path.len == 0) {
            clay_sayc(CLAY_RED, "Usage: install <path-or-git-url> [name]");
            ok = 0;
        } else if (clay_skill_install(path.data, name.len > 0 ? name.data : NULL) == 0) {
            clay_sayc(CLAY_GREEN, "Installed skill from %s.", path.data);
            ok = 1;
        } else {
            clay_sayc(CLAY_RED, "Couldn't install that skill - check the path/URL has a SKILL.md with "
                                "name/description frontmatter (or pass a name explicitly), and that "
                                "git succeeded if it's a remote.");
            ok = 0;
        }
        clay_str_free(&path);
        clay_str_free(&name);
        return ok ? 0 : 1;
    }

    if (args && strncmp(args, "remove ", 7) == 0) {
        const char *name = args + 7;
        while (*name == ' ') name++;
        if (*name && clay_skill_remove(name) == 0) {
            clay_sayc(CLAY_GREEN, "Removed %s.", name);
            return 0;
        }
        clay_sayc(CLAY_RED, "Usage: remove <name>");
        return 1;
    }

    if (args && strncmp(args, "enable ", 7) == 0) {
        const char *name = args + 7;
        while (*name == ' ') name++;
        if (*name && clay_skill_set_enabled(name, 1) == 0) {
            clay_sayc(CLAY_GREEN, "Enabled %s.", name);
            return 0;
        }
        clay_sayc(CLAY_RED, "No skill named %s.", name);
        return 1;
    }

    if (args && strncmp(args, "disable ", 8) == 0) {
        const char *name = args + 8;
        while (*name == ' ') name++;
        if (*name && clay_skill_set_enabled(name, 0) == 0) {
            clay_sayc(CLAY_GREEN, "Disabled %s.", name);
            return 0;
        }
        clay_sayc(CLAY_RED, "No skill named %s.", name);
        return 1;
    }

    if (args && *args) {
        clay_sayc(CLAY_RED, "Usage: (list), install <path-or-git-url> [name], remove <name>, "
                            "enable|disable <name>");
        return 1;
    }

    print_installed();
    return 0;
}

void clay_cmd_skill(const char *args, void *user_data) {
    (void)user_data;
    skill_command(args);
}

int clay_skill_cli_main(int argc, char **argv) {
    ClayStr args;
    clay_str_init(&args);
    for (int i = 0; i < argc; i++) {
        if (i > 0) clay_str_push_char(&args, ' ');
        clay_str_push(&args, argv[i]);
    }
    int rc = skill_command(argc > 0 ? args.data : NULL);
    clay_str_free(&args);
    return rc;
}
