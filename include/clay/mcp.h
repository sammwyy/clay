#ifndef CLAY_MCP_H
#define CLAY_MCP_H

#include "clay/array.h"
#include "clay/json.h"

/* A minimal Model Context Protocol client, stdio transport only: fork/exec
   a configured server, speak newline-delimited JSON-RPC 2.0 over its
   stdin/stdout, and expose whatever tools it advertises. No SSE/HTTP
   transport, no resources/prompts/sampling - just tools. */

typedef struct ClayMcpServer ClayMcpServer;

typedef struct {
    char *name;
    char *description;
    ClayJson *input_schema; /* owned by the ClayMcpServer */
} ClayMcpTool;

/* Starts `command` (found via PATH) with `argv` (NULL-terminated, argv[0]
   conventionally the program name), performs the MCP initialize handshake,
   and fetches its tool list. NULL if the process didn't start or didn't
   complete the handshake. */
ClayMcpServer *clay_mcp_connect(const char *name, const char *command, char *const argv[]);
void clay_mcp_disconnect(ClayMcpServer *server);

const char *clay_mcp_server_name(const ClayMcpServer *server);
size_t clay_mcp_tool_count(const ClayMcpServer *server);
const ClayMcpTool *clay_mcp_tool_at(const ClayMcpServer *server, size_t index);
const ClayMcpTool *clay_mcp_find_tool(const ClayMcpServer *server, const char *tool_name);

/* Calls `tool_name` with `arguments` (borrowed), returning its concatenated
   text content. *is_error is set to 1 if the server reported the call as a
   tool-level error, and left unset otherwise. Malloc'd; NULL only on a
   transport failure (the process died, or sent something unparseable). */
char *clay_mcp_call(ClayMcpServer *server, const char *tool_name, const ClayJson *arguments, int *is_error);

/* Binds one server's tool to a ClayTool-shaped entry point: `exposed_name`
   is what's registered with the provider ("mcp__<server>__<tool>", to keep
   names unique across servers), `tool_name` is what's sent back to the
   server on tools/call. */
typedef struct {
    ClayMcpServer *server; /* borrowed */
    char *tool_name;
    char *exposed_name;
} ClayMcpToolBinding;

/* Generic ClayToolFn (see clay/providers/openai.h) - userdata is a
   ClayMcpToolBinding*. */
ClayJson *clay_mcp_tool_call_fn(const ClayJson *arguments, void *userdata);

/* Saved server configs, ~/.clay/mcp_servers.json. */
typedef struct {
    char *name;
    char *command;
    ClayArray args; /* char* */
} ClayMcpServerConfig;

/* Every configured server. Caller frees with clay_mcp_config_list_free. */
int clay_mcp_config_list(ClayArray *servers);
void clay_mcp_config_list_free(ClayArray *servers);
/* Adds a server, or replaces the existing one with the same name. 0 on
   success. */
int clay_mcp_config_add(const char *name, const char *command, char *const args[], size_t arg_count);
/* 0 on success (including if `name` wasn't configured). */
int clay_mcp_config_remove(const char *name);

#endif /* CLAY_MCP_H */
