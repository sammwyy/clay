#include "clay/mcp.h"

#include "clay/str.h"
#include "clay/term.h"
#include "clay/time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLAY_MCP_PROTOCOL_VERSION "2024-11-05"
#define CLAY_MCP_CLIENT_VERSION "0.0.0"

struct ClayMcpServer {
    ClayProcess *process;
    char *name;
    long next_id;
    ClayArray tools; /* ClayMcpTool */
};

static void send_notification(ClayMcpServer *server, const char *method, ClayJson *params) {
    ClayJson *envelope = clay_json_object();
    clay_json_object_set(envelope, "jsonrpc", clay_json_string("2.0"));
    clay_json_object_set(envelope, "method", clay_json_string(method));
    clay_json_object_set(envelope, "params", params);
    ClayStr body;
    clay_str_init(&body);
    clay_json_stringify(envelope, &body);
    clay_json_free(envelope);
    clay_str_push_char(&body, '\n');
    clay_term_process_write(server->process, body.data, body.len);
    clay_str_free(&body);
}

/* Sends a request and blocks for its matching response, discarding any
   unsolicited notifications the server sends in between. Takes ownership
   of `params`. Returns the "result" value (caller frees), or NULL on a
   transport failure or a JSON-RPC error response. */
static ClayJson *send_request(ClayMcpServer *server, const char *method, ClayJson *params, long id) {
    ClayJson *envelope = clay_json_object();
    clay_json_object_set(envelope, "jsonrpc", clay_json_string("2.0"));
    clay_json_object_set(envelope, "id", clay_json_number((double)id));
    clay_json_object_set(envelope, "method", clay_json_string(method));
    clay_json_object_set(envelope, "params", params);
    ClayStr body;
    clay_str_init(&body);
    clay_json_stringify(envelope, &body);
    clay_json_free(envelope);
    clay_str_push_char(&body, '\n');
    int rc = clay_term_process_write(server->process, body.data, body.len);
    clay_str_free(&body);
    if (rc != 0) return NULL;

    for (;;) {
        char *line = clay_term_process_read_line(server->process);
        if (!line) return NULL;
        ClayJson *message = clay_json_parse(line, NULL);
        free(line);
        if (!message || clay_json_type(message) != CLAY_JSON_OBJECT) {
            clay_json_free(message);
            continue;
        }
        ClayJson *msg_id = clay_json_object_get(message, "id");
        if (clay_json_type(msg_id) != CLAY_JSON_NUMBER || (long)clay_json_number_value(msg_id) != id) {
            clay_json_free(message);
            continue;
        }
        if (clay_json_object_get(message, "error")) {
            clay_json_free(message);
            return NULL;
        }
        ClayJson *result = clay_json_object_get(message, "result");
        ClayJson *owned = result ? clay_json_clone(result) : clay_json_object();
        clay_json_free(message);
        return owned;
    }
}

static void tool_free(ClayMcpTool *tool) {
    free(tool->name);
    free(tool->description);
    clay_json_free(tool->input_schema);
}

ClayMcpServer *clay_mcp_connect(const char *name, const char *command, char *const argv[]) {
    ClayProcess *process = clay_term_process_start(command, argv);
    if (!process) return NULL;

    ClayMcpServer *server = calloc(1, sizeof(ClayMcpServer));
    server->process = process;
    server->name = strdup(name);
    server->next_id = 1;
    clay_array_init(&server->tools, sizeof(ClayMcpTool));

    ClayJson *client_info = clay_json_object();
    clay_json_object_set(client_info, "name", clay_json_string("clay"));
    clay_json_object_set(client_info, "version", clay_json_string(CLAY_MCP_CLIENT_VERSION));
    ClayJson *init_params = clay_json_object();
    clay_json_object_set(init_params, "protocolVersion", clay_json_string(CLAY_MCP_PROTOCOL_VERSION));
    clay_json_object_set(init_params, "capabilities", clay_json_object());
    clay_json_object_set(init_params, "clientInfo", client_info);

    ClayJson *init_result = send_request(server, "initialize", init_params, server->next_id++);
    if (!init_result) {
        clay_mcp_disconnect(server);
        return NULL;
    }
    clay_json_free(init_result);

    send_notification(server, "notifications/initialized", clay_json_object());

    ClayJson *list_result = send_request(server, "tools/list", clay_json_object(), server->next_id++);
    if (!list_result) {
        clay_mcp_disconnect(server);
        return NULL;
    }
    ClayJson *tools = clay_json_object_get(list_result, "tools");
    for (size_t i = 0; i < clay_json_array_count(tools); i++) {
        ClayJson *entry = clay_json_array_get(tools, i);
        const char *tool_name = clay_json_string_value(clay_json_object_get(entry, "name"));
        if (!tool_name || !*tool_name) continue;
        const char *description = clay_json_string_value(clay_json_object_get(entry, "description"));
        ClayJson *schema = clay_json_object_get(entry, "inputSchema");
        ClayMcpTool tool = {strdup(tool_name), strdup(description ? description : ""),
                            schema ? clay_json_clone(schema) : clay_json_object()};
        clay_array_push_val(&server->tools, &tool);
    }
    clay_json_free(list_result);
    return server;
}

void clay_mcp_disconnect(ClayMcpServer *server) {
    if (!server) return;
    clay_term_process_stop(server->process);
    free(server->name);
    for (size_t i = 0; i < server->tools.count; i++) tool_free(clay_array_get(&server->tools, i));
    clay_array_free(&server->tools);
    free(server);
}

const char *clay_mcp_server_name(const ClayMcpServer *server) {
    return server->name;
}

size_t clay_mcp_tool_count(const ClayMcpServer *server) {
    return server->tools.count;
}

const ClayMcpTool *clay_mcp_tool_at(const ClayMcpServer *server, size_t index) {
    return clay_array_get((ClayArray *)&server->tools, index);
}

const ClayMcpTool *clay_mcp_find_tool(const ClayMcpServer *server, const char *tool_name) {
    for (size_t i = 0; i < server->tools.count; i++) {
        ClayMcpTool *tool = clay_array_get((ClayArray *)&server->tools, i);
        if (strcmp(tool->name, tool_name) == 0) return tool;
    }
    return NULL;
}

char *clay_mcp_call(ClayMcpServer *server, const char *tool_name, const ClayJson *arguments, int *is_error) {
    ClayJson *params = clay_json_object();
    clay_json_object_set(params, "name", clay_json_string(tool_name));
    clay_json_object_set(params, "arguments", arguments ? clay_json_clone(arguments) : clay_json_object());
    ClayJson *result = send_request(server, "tools/call", params, server->next_id++);
    if (!result) return NULL;

    if (is_error) *is_error = clay_json_bool_value(clay_json_object_get(result, "isError"));
    ClayJson *content = clay_json_object_get(result, "content");
    ClayStr text;
    clay_str_init(&text);
    for (size_t i = 0; i < clay_json_array_count(content); i++) {
        ClayJson *block = clay_json_array_get(content, i);
        const char *type = clay_json_string_value(clay_json_object_get(block, "type"));
        if (type && strcmp(type, "text") == 0) {
            if (text.len > 0) clay_str_push_char(&text, '\n');
            clay_str_push(&text, clay_json_string_value(clay_json_object_get(block, "text")));
        }
    }
    clay_json_free(result);
    return text.data;
}

ClayJson *clay_mcp_tool_call_fn(const ClayJson *arguments, void *userdata) {
    ClayMcpToolBinding *binding = userdata;
    int is_error = 0;
    char *text = clay_mcp_call(binding->server, binding->tool_name, arguments, &is_error);
    ClayJson *result = clay_json_object();
    if (!text) {
        clay_json_object_set(result, "ok", clay_json_bool(0));
        clay_json_object_set(result, "error", clay_json_string("MCP request failed (server may have exited)"));
        return result;
    }
    clay_json_object_set(result, "ok", clay_json_bool(!is_error));
    clay_json_object_set(result, "output", clay_json_string(text));
    clay_json_object_set(result, "output_truncated", clay_json_bool(0));
    if (is_error) clay_json_object_set(result, "error", clay_json_string(text));
    free(text);
    return result;
}

static char *config_path(void) {
    char *home = clay_term_home_dir();
    if (!home) return NULL;
    ClayStr path;
    clay_str_init(&path);
    clay_str_printf(&path, "%s/.clay/mcp_servers.json", home);
    free(home);
    return path.data;
}

#define CLAY_MCP_CONFIG_FILE_LIMIT (4 * 1024 * 1024)

/* Array of server-config objects, or an empty array if missing/malformed. */
static ClayJson *load_config_root(void) {
    char *path = config_path();
    ClayStr text;
    int read_rc = path ? clay_term_read_file(path, CLAY_MCP_CONFIG_FILE_LIMIT, &text) : -1;
    free(path);
    ClayJson *root = read_rc == 0 ? clay_json_parse(text.data, NULL) : NULL;
    if (read_rc == 0) clay_str_free(&text);
    if (!root || clay_json_type(root) != CLAY_JSON_ARRAY) {
        clay_json_free(root);
        root = clay_json_array();
    }
    return root;
}

/* Takes ownership of root. 0 on success. */
static int save_config_root(ClayJson *root) {
    char *home = clay_term_home_dir();
    if (!home) {
        clay_json_free(root);
        return -1;
    }
    ClayStr dir;
    clay_str_init(&dir);
    clay_str_printf(&dir, "%s/.clay", home);
    free(home);
    clay_term_mkdir(dir.data);
    ClayStr path;
    clay_str_init(&path);
    clay_str_printf(&path, "%s/mcp_servers.json", dir.data);
    clay_str_free(&dir);

    ClayStr body;
    clay_str_init(&body);
    clay_json_stringify(root, &body);
    clay_json_free(root);
    FILE *file = fopen(path.data, "w");
    int ok = file && fwrite(body.data, 1, body.len, file) == body.len;
    if (file) fclose(file);
    clay_str_free(&path);
    clay_str_free(&body);
    return ok ? 0 : -1;
}

int clay_mcp_config_list(ClayArray *servers) {
    clay_array_init(servers, sizeof(ClayMcpServerConfig));
    ClayJson *root = load_config_root();
    for (size_t i = 0; i < clay_json_array_count(root); i++) {
        ClayJson *entry = clay_json_array_get(root, i);
        ClayMcpServerConfig config;
        config.name = strdup(clay_json_string_value(clay_json_object_get(entry, "name")));
        config.command = strdup(clay_json_string_value(clay_json_object_get(entry, "command")));
        clay_array_init(&config.args, sizeof(char *));
        ClayJson *args = clay_json_object_get(entry, "args");
        for (size_t j = 0; j < clay_json_array_count(args); j++) {
            char *arg = strdup(clay_json_string_value(clay_json_array_get(args, j)));
            clay_array_push_val(&config.args, &arg);
        }
        clay_array_push_val(servers, &config);
    }
    clay_json_free(root);
    return 0;
}

void clay_mcp_config_list_free(ClayArray *servers) {
    for (size_t i = 0; i < servers->count; i++) {
        ClayMcpServerConfig *config = clay_array_get(servers, i);
        free(config->name);
        free(config->command);
        for (size_t j = 0; j < config->args.count; j++) free(*(char **)clay_array_get(&config->args, j));
        clay_array_free(&config->args);
    }
    clay_array_free(servers);
}

int clay_mcp_config_add(const char *name, const char *command, char *const args[], size_t arg_count) {
    ClayJson *root = load_config_root();
    for (size_t i = 0; i < clay_json_array_count(root); i++) {
        ClayJson *entry = clay_json_array_get(root, i);
        if (strcmp(clay_json_string_value(clay_json_object_get(entry, "name")), name) == 0) {
            clay_json_array_remove(root, i);
            break;
        }
    }
    ClayJson *entry = clay_json_object();
    clay_json_object_set(entry, "name", clay_json_string(name));
    clay_json_object_set(entry, "command", clay_json_string(command));
    ClayJson *args_json = clay_json_array();
    for (size_t i = 0; i < arg_count; i++) clay_json_array_push(args_json, clay_json_string(args[i]));
    clay_json_object_set(entry, "args", args_json);
    clay_json_array_push(root, entry);
    return save_config_root(root);
}

int clay_mcp_config_remove(const char *name) {
    ClayJson *root = load_config_root();
    for (size_t i = 0; i < clay_json_array_count(root); i++) {
        ClayJson *entry = clay_json_array_get(root, i);
        if (strcmp(clay_json_string_value(clay_json_object_get(entry, "name")), name) == 0) {
            clay_json_array_remove(root, i);
            break;
        }
    }
    return save_config_root(root);
}
