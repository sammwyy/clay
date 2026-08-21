#include "context.h"

#include <stdlib.h>
#include <string.h>

void clay_commands_connect_mcp_servers(ClayCommands *commands) {
    if (commands->mcp_connect_attempted) return;
    commands->mcp_connect_attempted = 1;

    ClayArray configs;
    if (clay_mcp_config_list(&configs) != 0 || configs.count == 0) {
        clay_mcp_config_list_free(&configs);
        return;
    }

    for (size_t i = 0; i < configs.count; i++) {
        ClayMcpServerConfig *config = clay_array_get(&configs, i);
        ClayTask *task = clay_app_task_start(commands->app, "Connecting to MCP server: %s", config->name);

        ClayArray argv_storage;
        clay_array_init(&argv_storage, sizeof(char *));
        clay_array_push_val(&argv_storage, &config->command);
        for (size_t j = 0; j < config->args.count; j++) {
            clay_array_push_val(&argv_storage, clay_array_get(&config->args, j));
        }
        char *null_terminator = NULL;
        clay_array_push_val(&argv_storage, &null_terminator);

        ClayMcpServer *server = clay_mcp_connect(config->name, config->command, argv_storage.data);
        clay_array_free(&argv_storage);

        if (!server) {
            clay_app_task_fail(commands->app, task, "could not connect");
            continue;
        }
        size_t tool_count = clay_mcp_tool_count(server);
        clay_app_task_success(commands->app, task, "%zu tool%s", tool_count, tool_count == 1 ? "" : "s");
        clay_array_push_val(&commands->mcp_servers, &server);

        for (size_t j = 0; j < tool_count; j++) {
            const ClayMcpTool *tool = clay_mcp_tool_at(server, j);
            ClayStr exposed;
            clay_str_init(&exposed);
            clay_str_printf(&exposed, "mcp__%s__%s", config->name, tool->name);
            ClayMcpToolBinding binding = {server, strdup(tool->name), exposed.data};
            clay_array_push_val(&commands->mcp_bindings, &binding);
        }
    }
    clay_mcp_config_list_free(&configs);
}

static void split_whitespace(const char *text, ClayArray *tokens) {
    clay_array_init(tokens, sizeof(char *));
    const char *p = text;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ' ') p++;
        ClayStr token;
        clay_str_init(&token);
        clay_str_push_n(&token, start, (size_t)(p - start));
        clay_array_push_val(tokens, &token.data);
    }
}

static void free_tokens(ClayArray *tokens) {
    for (size_t i = 0; i < tokens->count; i++) free(*(char **)clay_array_get(tokens, i));
    clay_array_free(tokens);
}

static void print_configured(void) {
    ClayArray configs;
    clay_mcp_config_list(&configs);
    if (configs.count == 0) {
        clay_sayc(CLAY_GRAY, "No MCP servers configured. Add one with /mcp add <name> <command> [args...].");
        clay_mcp_config_list_free(&configs);
        return;
    }
    clay_list_header("Configured MCP servers:");
    for (size_t i = 0; i < configs.count; i++) {
        ClayMcpServerConfig *config = clay_array_get(&configs, i);
        ClayStr line;
        clay_str_init(&line);
        clay_str_push(&line, config->command);
        for (size_t j = 0; j < config->args.count; j++) {
            clay_str_printf(&line, " %s", *(char **)clay_array_get(&config->args, j));
        }
        clay_list_step((int)(i + 1), config->name, line.data, NULL, 0);
        clay_str_free(&line);
    }
    clay_mcp_config_list_free(&configs);
}

void clay_cmd_mcp(const char *args, void *user_data) {
    ClayCommands *commands = user_data;

    if (args && strncmp(args, "add ", 4) == 0) {
        ClayArray tokens;
        split_whitespace(args + 4, &tokens);
        if (tokens.count < 2) {
            clay_sayc(CLAY_RED, "Usage: /mcp add <name> <command> [args...]");
        } else {
            char *name = *(char **)clay_array_get(&tokens, 0);
            char *command = *(char **)clay_array_get(&tokens, 1);
            size_t arg_count = tokens.count - 2;
            char **argv_tail = arg_count > 0 ? clay_array_get(&tokens, 2) : NULL;
            if (clay_mcp_config_add(name, command, argv_tail, arg_count) == 0) {
                clay_sayc(CLAY_GREEN, "Added MCP server %s - it connects the next time you send a message.", name);
            } else {
                clay_sayc(CLAY_RED, "Could not save that MCP server.");
            }
        }
        free_tokens(&tokens);
        return;
    }

    if (args && strncmp(args, "remove ", 7) == 0) {
        const char *name = args + 7;
        while (*name == ' ') name++;
        if (*name && clay_mcp_config_remove(name) == 0) clay_sayc(CLAY_GREEN, "Removed %s.", name);
        else clay_sayc(CLAY_RED, "Usage: /mcp remove <name>");
        return;
    }

    if (args && *args) {
        clay_sayc(CLAY_RED, "Usage: /mcp, /mcp add <name> <command> [args...], /mcp remove <name>");
        return;
    }

    if (commands->mcp_connect_attempted) {
        if (commands->mcp_servers.count == 0) {
            clay_sayc(CLAY_GRAY, "No MCP servers connected this session.");
        } else {
            for (size_t i = 0; i < commands->mcp_servers.count; i++) {
                ClayMcpServer *server = *(ClayMcpServer **)clay_array_get(&commands->mcp_servers, i);
                size_t tool_count = clay_mcp_tool_count(server);
                clay_sayc(CLAY_GREEN, "%s: connected, %zu tool%s.", clay_mcp_server_name(server), tool_count,
                         tool_count == 1 ? "" : "s");
            }
        }
    }
    print_configured();
}
