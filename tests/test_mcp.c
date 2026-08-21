#include "clay/mcp.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    /* Config persistence, isolated from the real ~/.clay. */
    char template[] = "/tmp/clay_test_mcp_XXXXXX";
    char *home = mkdtemp(template);
    assert(home);
    assert(setenv("HOME", home, 1) == 0);

    ClayArray servers;
    assert(clay_mcp_config_list(&servers) == 0);
    assert(servers.count == 0);
    clay_mcp_config_list_free(&servers);

    char *args[] = {"tests/fixtures/fake_mcp_server.py"};
    assert(clay_mcp_config_add("fake", "python3", args, 1) == 0);
    assert(clay_mcp_config_list(&servers) == 0);
    assert(servers.count == 1);
    ClayMcpServerConfig *config = clay_array_get(&servers, 0);
    assert(strcmp(config->name, "fake") == 0);
    assert(strcmp(config->command, "python3") == 0);
    assert(config->args.count == 1);
    clay_mcp_config_list_free(&servers);

    /* Adding again with the same name replaces, not duplicates. */
    assert(clay_mcp_config_add("fake", "python3", args, 1) == 0);
    assert(clay_mcp_config_list(&servers) == 0);
    assert(servers.count == 1);
    clay_mcp_config_list_free(&servers);

    assert(clay_mcp_config_remove("fake") == 0);
    assert(clay_mcp_config_list(&servers) == 0);
    assert(servers.count == 0);
    clay_mcp_config_list_free(&servers);

    /* Live protocol round-trip against a real (fake) MCP server. */
    char *server_argv[] = {"python3", "tests/fixtures/fake_mcp_server.py", NULL};
    ClayMcpServer *server = clay_mcp_connect("fake", "python3", server_argv);
    assert(server);
    assert(strcmp(clay_mcp_server_name(server), "fake") == 0);
    assert(clay_mcp_tool_count(server) == 2);

    const ClayMcpTool *echo = clay_mcp_find_tool(server, "echo");
    assert(echo);
    assert(strcmp(echo->description, "Echoes back its input.") == 0);
    assert(clay_json_type(echo->input_schema) == CLAY_JSON_OBJECT);
    assert(clay_mcp_find_tool(server, "nope") == NULL);

    ClayJson *echo_args = clay_json_object();
    clay_json_object_set(echo_args, "text", clay_json_string("hello from clay"));
    int is_error = 0;
    char *text = clay_mcp_call(server, "echo", echo_args, &is_error);
    clay_json_free(echo_args);
    assert(text);
    assert(!is_error);
    assert(strcmp(text, "hello from clay") == 0);
    free(text);

    is_error = 0;
    ClayJson *no_args = clay_json_object();
    text = clay_mcp_call(server, "fail", no_args, &is_error);
    clay_json_free(no_args);
    assert(text);
    assert(is_error);
    assert(strcmp(text, "it broke") == 0);
    free(text);

    /* Through the generic ClayToolFn adapter used by the tool loop. */
    ClayMcpToolBinding binding = {server, "echo", "mcp__fake__echo"};
    ClayJson *binding_args = clay_json_object();
    clay_json_object_set(binding_args, "text", clay_json_string("via binding"));
    ClayJson *result = clay_mcp_tool_call_fn(binding_args, &binding);
    clay_json_free(binding_args);
    assert(clay_json_bool_value(clay_json_object_get(result, "ok")));
    assert(strcmp(clay_json_string_value(clay_json_object_get(result, "output")), "via binding") == 0);
    clay_json_free(result);

    clay_mcp_disconnect(server);

    /* Connecting to a nonexistent program fails cleanly. */
    char *bad_argv[] = {"clay-does-not-exist-anywhere", NULL};
    ClayMcpServer *bad = clay_mcp_connect("bad", "clay-does-not-exist-anywhere", bad_argv);
    assert(bad == NULL);

    printf("mcp tests passed\n");
    return 0;
}
