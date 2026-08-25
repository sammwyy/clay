/* OpenAI Codex uses the ChatGPT subscription backend, not api.openai.com.
   Its undocumented compatibility details deliberately live in this file. */
#include "clay/providers/openai_codex.h"

#include "clay/encoding.h"
#include "clay/http.h"
#include "clay/oauth.h"
#include "clay/sse.h"
#include "clay/term.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CODEX_CLIENT_ID "app_EMoamEEZ73f0CkXaXp7hrann"
#define CODEX_AUTH_URL "https://auth.openai.com/oauth/authorize"
#define CODEX_TOKEN_URL "https://auth.openai.com/oauth/token"
#define CODEX_REDIRECT_URI "http://localhost:1455/auth/callback"
#define CODEX_BACKEND_URL "https://chatgpt.com/backend-api/codex"
#define CODEX_ORIGINATOR "clay"
/* The undocumented catalog endpoint currently rejects requests without this
   compatibility parameter. Keep it local to the Codex integration. */
#define CODEX_CLIENT_VERSION "0.149.0"
#define CODEX_MODELS_URL                                                       \
  CODEX_BACKEND_URL "/models?client_version=" CODEX_CLIENT_VERSION
#define CODEX_CALLBACK_TIMEOUT_SECONDS 180
#define CODEX_RESPONSE_LIMIT (16 * 1024 * 1024)
#define CODEX_MODELS_RESPONSE_LIMIT (16 * 1024 * 1024)
#define CODEX_SSE_LINE_LIMIT (256 * 1024)
#define CODEX_CONTENT_LIMIT (4 * 1024 * 1024)
#define CODEX_REASONING_LIMIT (4 * 1024 * 1024)
#define CODEX_TOOL_LIMIT 128
#define CODEX_FIELD_LIMIT (1024 * 1024)

struct ClayOpenAICodex {
  ClayCodexCredentials credentials;
  char *model;
  char *reasoning_effort;
  char *prompt_cache_key;
  int refresh_failed;
  int authentication_invalid;
  long last_status;
};

typedef struct {
  ClayStr id;
  ClayStr item_id;
  ClayStr name;
  ClayStr arguments;
} CodexToolCall;

typedef struct {
  ClayStr raw;
  ClayStr content;
  ClayStr reasoning;
  ClayArray calls; /* CodexToolCall */
  ClaySseParser *sse;
  const ClayOpenAICallbacks *callbacks;
  int cancelled;
  int malformed;
  int failed_event;
  long input_tokens;
  long output_tokens;
} CodexStream;

char *clay_openai_codex_authorization_url(const char *challenge,
                                          const char *state) {
  if (!challenge || !state)
    return NULL;
  char *redirect = clay_url_encode(CODEX_REDIRECT_URI),
       *scope = clay_url_encode("openid profile email offline_access");
  char *encoded_challenge = clay_url_encode(challenge),
       *encoded_state = clay_url_encode(state);
  ClayStr url;
  clay_str_init(&url);
  clay_str_printf(&url,
                  "%s?response_type=code&client_id=%s&redirect_uri=%s&scope=%s&"
                  "code_challenge=%s&code_challenge_method=S256&state=%s&id_"
                  "token_add_organizations=true&codex_cli_simplified_flow=true",
                  CODEX_AUTH_URL, CODEX_CLIENT_ID, redirect, scope,
                  encoded_challenge, encoded_state);
  free(redirect);
  free(scope);
  free(encoded_challenge);
  free(encoded_state);
  return url.data;
}

int clay_openai_codex_extract_account_id(const char *id_token,
                                         char **account_id_out) {
  if (account_id_out)
    *account_id_out = NULL;
  if (!id_token || !account_id_out)
    return -1;
  ClayJson *payload = clay_jwt_decode_payload(id_token);
  if (!payload)
    return -1;
  ClayJson *auth = clay_json_object_get(payload, "https://api.openai.com/auth");
  const char *account =
      clay_json_string_value(clay_json_object_get(auth, "chatgpt_account_id"));
  if (account && *account)
    *account_id_out = strdup(account);
  clay_json_free(payload);
  return *account_id_out ? 0 : -1;
}

static void append_error(ClayStr *error, const char *message) {
  if (error) {
    clay_str_clear(error);
    clay_str_push(error, message);
  }
}
static int wait_callback(ClayTermHttpServer *listener, const char *state,
                         ClayOAuthCallback *out, ClayStr *error) {
  time_t deadline = time(NULL) + CODEX_CALLBACK_TIMEOUT_SECONDS;
  ClayStr request;
  clay_str_init(&request);
  while (time(NULL) < deadline) {
    if (clay_term_take_interrupt()) {
      clay_str_free(&request);
      append_error(error, "Authentication cancelled.");
      return -1;
    }
    int ready = clay_term_http_server_receive(listener, &request, 1000);
    if (ready == 0)
      continue;
    if (ready < 0) {
      clay_str_free(&request);
      append_error(error, "Invalid OAuth callback.");
      return -1;
    }
    char *end = strstr(request.data, "\r\n");
    if (!end)
      end = strchr(request.data, '\n');
    if (end)
      *end = 0;
    char method[8], target[8192], version[16];
    int parsed =
        sscanf(request.data, "%7s %8191s %15s", method, target, version) == 3 &&
        strcmp(method, "GET") == 0;
    int ok = parsed && clay_oauth_parse_callback(target, "/auth/callback",
                                                 state, out) == 0;
    const char *body = ok ? "<!doctype html><title>Authentication "
                            "complete</title><p>Authentication complete. You "
                            "can return to the application.</p>"
                          : "<!doctype html><title>Authentication "
                            "failed</title><p>Authentication failed. You can "
                            "return to the application.</p>";
    clay_term_http_server_reply(listener, ok ? 200 : 400, body);
    clay_str_free(&request);
    if (ok)
      return 0;
    append_error(
        error, "ChatGPT authorization failed or returned an invalid callback.");
    return -1;
  }
  clay_str_free(&request);
  append_error(error, "Timed out waiting for ChatGPT authorization.");
  return -1;
}

static int token_request(const char *body, const char *content_type,
                         ClayJson **json_out) {
  ClayHttpHeader headers[] = {{"Content-Type", content_type},
                              {"Accept", "application/json"}};
  ClayHttpRequest req = {0};
  req.method = "POST";
  req.url = CODEX_TOKEN_URL;
  req.headers = headers;
  req.header_count = 2;
  req.body = body;
  req.body_len = strlen(body);
  req.timeout_seconds = 30;
  req.max_response_bytes = 1024 * 1024;
  ClayHttpResponse resp;
  int rc = clay_http_request(&req, &resp);
  if (rc || resp.status < 200 || resp.status >= 300) {
    clay_http_response_free(&resp);
    return -1;
  }
  *json_out = clay_json_parse(resp.body, NULL);
  clay_http_response_free(&resp);
  return *json_out ? 0 : -1;
}
static int credentials_from_token(ClayJson *root, ClayCodexCredentials *out,
                                  int require_id, int preserve_refresh) {
  if (!root || !out)
    return -1;
  const char *access =
      clay_json_string_value(clay_json_object_get(root, "access_token"));
  const char *refresh =
      clay_json_string_value(clay_json_object_get(root, "refresh_token"));
  const char *id =
      clay_json_string_value(clay_json_object_get(root, "id_token"));
  if (!access || !*access || (require_id && (!id || !*id)) ||
      ((!refresh || !*refresh) && !preserve_refresh))
    return -1;
  free(out->access_token);
  out->access_token = strdup(access);
  if (refresh && *refresh) {
    free(out->refresh_token);
    out->refresh_token = strdup(refresh);
  }
  if (id && *id) {
    free(out->id_token);
    out->id_token = strdup(id);
  }
  long expires =
      (long)clay_json_number_value(clay_json_object_get(root, "expires_in"));
  out->expires_at = (long long)time(NULL) + (expires > 0 ? expires : 3600);
  return 0;
}
int clay_openai_codex_authenticate(ClayCodexCredentials *out, ClayStr *error) {
  if (!out)
    return -1;
  memset(out, 0, sizeof(*out));
  char *verifier = NULL, *challenge = NULL, *state = NULL, *url = NULL;
  ClayTermHttpServer *listener = NULL;
  ClayOAuthPkce pkce = {0};
  ClayOAuthCallback callback = {0};
  ClayJson *token = NULL;
  int rc = -1;
  if (clay_oauth_pkce_create(&pkce) != 0) {
    append_error(error, "Could not create secure OAuth credentials.");
    goto done;
  }
  listener = clay_term_http_server_create(1455);
  if (!listener) {
    append_error(error, "Could not listen on 127.0.0.1:1455. Another "
                        "application may already be using the OAuth port.");
    goto done;
  }
  verifier = pkce.verifier;
  challenge = pkce.challenge;
  state = pkce.state;
  url = clay_openai_codex_authorization_url(challenge, state);
  if (!url) {
    append_error(error, "Could not create authorization URL.");
    goto done;
  }
  if (clay_term_open_browser(url) != 0)
    fprintf(stderr, "Could not open a browser. Open this URL manually:\n%s\n",
            url);
  if (wait_callback(listener, state, &callback, error) != 0)
    goto done;
  clay_term_http_server_destroy(listener);
  listener = NULL;
  char *code = clay_url_encode(callback.code),
       *redirect = clay_url_encode(CODEX_REDIRECT_URI),
       *v = clay_url_encode(verifier);
  ClayStr body;
  clay_str_init(&body);
  clay_str_printf(&body,
                  "grant_type=authorization_code&code=%s&redirect_uri=%s&"
                  "client_id=%s&code_verifier=%s",
                  code, redirect, CODEX_CLIENT_ID, v);
  free(code);
  free(redirect);
  free(v);
  if (token_request(body.data, "application/x-www-form-urlencoded", &token) !=
      0) {
    clay_str_free(&body);
    append_error(error, "Could not exchange the ChatGPT authorization code.");
    goto done;
  }
  clay_str_free(&body);
  if (credentials_from_token(token, out, 1, 0) != 0 ||
      clay_openai_codex_extract_account_id(out->id_token, &out->account_id) !=
          0) {
    append_error(error, "ChatGPT authorization response did not include usable "
                        "Codex account credentials.");
    clay_openai_codex_credentials_free(out);
    goto done;
  }
  rc = 0;
done:
  clay_term_http_server_destroy(listener);
  clay_json_free(token);
  clay_oauth_callback_free(&callback);
  clay_oauth_pkce_free(&pkce);
  free(url);
  return rc;
}
void clay_openai_codex_credentials_free(ClayCodexCredentials *c) {
  if (!c)
    return;
  free(c->access_token);
  free(c->refresh_token);
  free(c->id_token);
  free(c->account_id);
  memset(c, 0, sizeof(*c));
}

static int refresh(ClayOpenAICodex *client) {
  if (client->refresh_failed || !client->credentials.refresh_token)
    return -1;
  ClayJson *root = clay_json_object();
  clay_json_object_set(root, "client_id", clay_json_string(CODEX_CLIENT_ID));
  clay_json_object_set(root, "grant_type", clay_json_string("refresh_token"));
  clay_json_object_set(root, "refresh_token",
                       clay_json_string(client->credentials.refresh_token));
  ClayStr body;
  clay_str_init(&body);
  clay_json_stringify(root, &body);
  clay_json_free(root);
  ClayJson *response = NULL;
  int rc = token_request(body.data, "application/json", &response);
  clay_str_free(&body);
  if (rc || credentials_from_token(response, &client->credentials, 0, 1) != 0) {
    client->refresh_failed = 1;
    client->authentication_invalid = 1;
    clay_json_free(response);
    return -1;
  }
  const char *id =
      clay_json_string_value(clay_json_object_get(response, "id_token"));
  if (id && *id) {
    char *account = NULL;
    if (clay_openai_codex_extract_account_id(id, &account) == 0) {
      free(client->credentials.account_id);
      client->credentials.account_id = account;
    }
  }
  clay_json_free(response);
  return 0;
}
ClayOpenAICodex *clay_openai_codex_create(const ClayCodexCredentials *c,
                                          const char *model) {
  if (!c || !c->access_token || !c->refresh_token || !c->account_id)
    return NULL;
  ClayOpenAICodex *client = calloc(1, sizeof(*client));
  client->credentials.access_token = strdup(c->access_token);
  client->credentials.refresh_token = strdup(c->refresh_token);
  client->credentials.id_token = c->id_token ? strdup(c->id_token) : NULL;
  client->credentials.account_id = strdup(c->account_id);
  client->credentials.expires_at = c->expires_at;
  client->model = strdup(model ? model : "");
  return client;
}
void clay_openai_codex_destroy(ClayOpenAICodex *client) {
  if (!client)
    return;
  clay_openai_codex_credentials_free(&client->credentials);
  free(client->model);
  free(client->reasoning_effort);
  free(client->prompt_cache_key);
  free(client);
}
void clay_openai_codex_set_reasoning_effort(ClayOpenAICodex *client,
                                            const char *effort) {
  if (!client)
    return;
  free(client->reasoning_effort);
  client->reasoning_effort = effort ? strdup(effort) : NULL;
}
void clay_openai_codex_set_prompt_cache_key(ClayOpenAICodex *client,
                                            const char *key) {
  if (!client)
    return;
  char *copy = key && *key ? strdup(key) : NULL;
  free(client->prompt_cache_key);
  client->prompt_cache_key = copy;
}
void clay_openai_codex_copy_credentials(const ClayOpenAICodex *client,
                                        ClayCodexCredentials *out) {
  if (!client || !out)
    return;
  clay_openai_codex_credentials_free(out);
  out->access_token = strdup(client->credentials.access_token);
  out->refresh_token = strdup(client->credentials.refresh_token);
  out->id_token = client->credentials.id_token
                      ? strdup(client->credentials.id_token)
                      : NULL;
  out->account_id = strdup(client->credentials.account_id);
  out->expires_at = client->credentials.expires_at;
}
int clay_openai_codex_authentication_invalid(const ClayOpenAICodex *client) {
  return client && client->authentication_invalid;
}

static int append_limited(ClayStr *out, const char *text, size_t n,
                          size_t limit) {
  if (out->len > limit || n > limit - out->len)
    return -1;
  clay_str_push_n(out, text, n);
  return 0;
}
static const char *string_at(ClayJson *a, const char *name) {
  return clay_json_string_value(clay_json_object_get(a, name));
}
static CodexToolCall *call_for(CodexStream *s, const char *id) {
  if (!id || !*id)
    return NULL;
  for (size_t i = 0; i < s->calls.count; i++) {
    CodexToolCall *c = clay_array_get(&s->calls, i);
    if (strcmp(c->id.data, id) == 0 || strcmp(c->item_id.data, id) == 0)
      return c;
  }
  if (s->calls.count >= CODEX_TOOL_LIMIT)
    return NULL;
  CodexToolCall c;
  clay_str_init(&c.id);
  clay_str_init(&c.item_id);
  clay_str_init(&c.name);
  clay_str_init(&c.arguments);
  clay_str_push(&c.id, id);
  clay_array_push_val(&s->calls, &c);
  return clay_array_get(&s->calls, s->calls.count - 1);
}
static void process_item(CodexStream *s, ClayJson *item) {
  if (clay_json_type(item) != CLAY_JSON_OBJECT ||
      strcmp(string_at(item, "type") ? string_at(item, "type") : "",
             "function_call") != 0)
    return;
  const char *call_id = string_at(item, "call_id"),
             *item_id = string_at(item, "id");
  CodexToolCall *call = call_for(s, item_id ? item_id : call_id);
  if (!call)
    return;
  if (call_id && strcmp(call->id.data, call_id) != 0) {
    clay_str_clear(&call->id);
    clay_str_push(&call->id, call_id);
  }
  if (item_id && !*call->item_id.data)
    clay_str_push(&call->item_id, item_id);
  const char *name = string_at(item, "name"),
             *args = string_at(item, "arguments");
  if (name &&
      append_limited(&call->name, name, strlen(name), CODEX_FIELD_LIMIT))
    s->malformed = 1;
  if (args &&
      append_limited(&call->arguments, args, strlen(args), CODEX_FIELD_LIMIT))
    s->malformed = 1;
}
static void codex_sse_json(const char *text, void *userdata) {
  CodexStream *s = userdata;
  ClayJson *root = clay_json_parse(text, NULL);
  if (!root) {
    s->malformed = 1;
    return;
  }
  const char *type = string_at(root, "type");
  if (!type) {
    clay_json_free(root);
    return;
  }
  if (strcmp(type, "response.output_text.delta") == 0) {
    const char *delta = string_at(root, "delta");
    if (delta) {
      if (append_limited(&s->content, delta, strlen(delta),
                         CODEX_CONTENT_LIMIT))
        s->malformed = 1;
      else if (s->callbacks && s->callbacks->on_token)
        s->callbacks->on_token(delta, s->callbacks->userdata);
    }
  } else if (strcmp(type, "response.reasoning_summary_text.delta") == 0) {
    const char *delta = string_at(root, "delta");
    if (delta) {
      if (append_limited(&s->reasoning, delta, strlen(delta),
                         CODEX_REASONING_LIMIT))
        s->malformed = 1;
      else if (s->callbacks && s->callbacks->on_reasoning)
        s->callbacks->on_reasoning(delta, s->callbacks->userdata);
    }
  } else if (strcmp(type, "response.output_item.added") == 0) {
    process_item(s, clay_json_object_get(root, "item"));
  } else if (strcmp(type, "response.function_call_arguments.delta") == 0) {
    const char *id = string_at(root, "call_id");
    if (!id)
      id = string_at(root, "item_id");
    CodexToolCall *call = call_for(s, id);
    const char *delta = string_at(root, "delta");
    if (!call || (delta && append_limited(&call->arguments, delta,
                                          strlen(delta), CODEX_FIELD_LIMIT)))
      s->malformed = 1;
  } else if (strcmp(type, "response.output_item.done") == 0) {
    process_item(s, clay_json_object_get(root, "item"));
  } else if (strcmp(type, "response.completed") == 0) {
    ClayJson *response = clay_json_object_get(root, "response");
    ClayJson *usage = clay_json_object_get(response, "usage");
    if (clay_json_type(usage) == CLAY_JSON_OBJECT && s->callbacks &&
        (s->callbacks->on_usage || s->callbacks->on_usage_details)) {
      ClayTokenUsage parsed;
      clay_openai_usage_from_json(usage, &parsed);
      s->input_tokens = parsed.input_tokens;
      s->output_tokens = parsed.output_tokens;
      if (s->callbacks->on_usage)
        s->callbacks->on_usage(s->input_tokens, s->output_tokens,
                               s->callbacks->userdata);
      if (s->callbacks->on_usage_details)
        s->callbacks->on_usage_details(&parsed, s->callbacks->userdata);
    }
  } else if (strcmp(type, "response.failed") == 0 || strcmp(type, "error") == 0)
    s->failed_event = 1;
  clay_json_free(root);
}
static void stream_init(CodexStream *s, const ClayOpenAICallbacks *callbacks) {
  memset(s, 0, sizeof(*s));
  clay_str_init(&s->raw);
  clay_str_init(&s->content);
  clay_str_init(&s->reasoning);
  clay_array_init(&s->calls, sizeof(CodexToolCall));
  s->callbacks = callbacks;
  s->sse = clay_sse_create(CODEX_SSE_LINE_LIMIT, codex_sse_json, s);
}
static void stream_free(CodexStream *s) {
  clay_str_free(&s->raw);
  clay_str_free(&s->content);
  clay_str_free(&s->reasoning);
  for (size_t i = 0; i < s->calls.count; i++) {
    CodexToolCall *c = clay_array_get(&s->calls, i);
    clay_str_free(&c->id);
    clay_str_free(&c->item_id);
    clay_str_free(&c->name);
    clay_str_free(&c->arguments);
  }
  clay_array_free(&s->calls);
  clay_sse_destroy(s->sse);
}
static int stream_chunk(const char *data, size_t len, void *userdata) {
  CodexStream *s = userdata;
  if (s->raw.len < 65536) {
    size_t kept = 65536 - s->raw.len;
    if (kept > len)
      kept = len;
    clay_str_push_n(&s->raw, data, kept);
  }
  return clay_sse_feed(s->sse, data, len);
}
static int stream_abort(void *userdata) {
  CodexStream *s = userdata;
  s->cancelled = s->callbacks && s->callbacks->should_abort &&
                 s->callbacks->should_abort(s->callbacks->userdata);
  return s->cancelled;
}

static ClayJson *responses_input(const ClayJson *messages,
                                 ClayStr *instructions) {
  ClayJson *input = clay_json_array();
  for (size_t i = 0; i < clay_json_array_count(messages); i++) {
    ClayJson *m = clay_json_array_get(messages, i);
    const char *role = string_at(m, "role"), *content = string_at(m, "content");
    if (!role)
      continue;
    if (strcmp(role, "system") == 0) {
      if (content && *content) {
        if (instructions->len)
          clay_str_push(instructions, "\n\n");
        clay_str_push(instructions, content);
      }
      continue;
    }
    if (strcmp(role, "tool") == 0) {
      ClayJson *item = clay_json_object();
      clay_json_object_set(item, "type",
                           clay_json_string("function_call_output"));
      clay_json_object_set(
          item, "call_id",
          clay_json_clone(clay_json_object_get(m, "tool_call_id")));
      clay_json_object_set(item, "output",
                           clay_json_string(content ? content : ""));
      clay_json_array_push(input, item);
      continue;
    }
    ClayJson *calls = clay_json_object_get(m, "tool_calls");
    if (strcmp(role, "assistant") == 0 &&
        clay_json_type(calls) == CLAY_JSON_ARRAY) {
      if (content && *content) {
        ClayJson *item = clay_json_object(), *parts = clay_json_array(),
                 *part = clay_json_object();
        clay_json_object_set(part, "type", clay_json_string("output_text"));
        clay_json_object_set(part, "text", clay_json_string(content));
        clay_json_array_push(parts, part);
        clay_json_object_set(item, "type", clay_json_string("message"));
        clay_json_object_set(item, "role", clay_json_string("assistant"));
        clay_json_object_set(item, "content", parts);
        clay_json_array_push(input, item);
      }
      for (size_t j = 0; j < clay_json_array_count(calls); j++) {
        ClayJson *call = clay_json_array_get(calls, j),
                 *fn = clay_json_object_get(call, "function"),
                 *item = clay_json_object();
        clay_json_object_set(item, "type", clay_json_string("function_call"));
        clay_json_object_set(item, "call_id",
                             clay_json_clone(clay_json_object_get(call, "id")));
        clay_json_object_set(item, "name",
                             clay_json_clone(clay_json_object_get(fn, "name")));
        clay_json_object_set(
            item, "arguments",
            clay_json_clone(clay_json_object_get(fn, "arguments")));
        clay_json_array_push(input, item);
      }
      continue;
    }
    ClayJson *item = clay_json_object(), *parts = clay_json_array(),
             *part = clay_json_object();
    /* Responses input represents prior assistant text as output_text. Sending
       it as input_text works for the first turn but makes later turns 400. */
    clay_json_object_set(part, "type",
                         clay_json_string(strcmp(role, "assistant") == 0
                                              ? "output_text"
                                              : "input_text"));
    clay_json_object_set(part, "text",
                         clay_json_string(content ? content : ""));
    clay_json_array_push(parts, part);
    clay_json_object_set(item, "type", clay_json_string("message"));
    clay_json_object_set(item, "role", clay_json_string(role));
    clay_json_object_set(item, "content", parts);
    clay_json_array_push(input, item);
  }
  return input;
}
static ClayStr request_body(ClayOpenAICodex *client, const ClayJson *messages,
                            const ClayTool *tools, size_t count) {
  ClayJson *root = clay_json_object();
  ClayStr instructions;
  clay_str_init(&instructions);
  ClayJson *input = responses_input(messages, &instructions);
  clay_json_object_set(root, "model", clay_json_string(client->model));
  if (instructions.len)
    clay_json_object_set(root, "instructions",
                         clay_json_string(instructions.data));
  clay_json_object_set(root, "input", input);
  clay_json_object_set(root, "store", clay_json_bool(0));
  clay_json_object_set(root, "stream", clay_json_bool(1));
  if (client->prompt_cache_key)
    clay_json_object_set(root, "prompt_cache_key",
                         clay_json_string(client->prompt_cache_key));
  if (client->reasoning_effort) {
    ClayJson *reasoning = clay_json_object();
    clay_json_object_set(reasoning, "effort",
                         clay_json_string(client->reasoning_effort));
    clay_json_object_set(root, "reasoning", reasoning);
  }
  if (count) {
    ClayJson *out = clay_json_array();
    for (size_t i = 0; i < count; i++) {
      ClayJson *tool = clay_json_object();
      clay_json_object_set(tool, "type", clay_json_string("function"));
      clay_json_object_set(tool, "name", clay_json_string(tools[i].name));
      clay_json_object_set(
          tool, "description",
          clay_json_string(tools[i].description ? tools[i].description : ""));
      clay_json_object_set(tool, "parameters",
                           tools[i].parameters
                               ? clay_json_clone(tools[i].parameters)
                               : clay_json_object());
      clay_json_array_push(out, tool);
    }
    clay_json_object_set(root, "tools", out);
  }
  ClayStr body;
  clay_str_init(&body);
  clay_json_stringify(root, &body);
  clay_json_free(root);
  clay_str_free(&instructions);
  return body;
}
static void apply_calls(ClayJson *messages, const ClayTool *tools,
                        size_t tool_count, CodexStream *s,
                        const ClayOpenAICallbacks *callbacks) {
  ClayJson *assistant = clay_json_object(), *calls = clay_json_array();
  clay_json_object_set(assistant, "role", clay_json_string("assistant"));
  clay_json_object_set(assistant, "content",
                       s->content.len ? clay_json_string(s->content.data)
                                      : clay_json_null());
  for (size_t i = 0; i < s->calls.count; i++) {
    CodexToolCall *c = clay_array_get(&s->calls, i);
    ClayJson *fn = clay_json_object(), *call = clay_json_object();
    clay_json_object_set(fn, "name", clay_json_string(c->name.data));
    clay_json_object_set(fn, "arguments", clay_json_string(c->arguments.data));
    clay_json_object_set(call, "id", clay_json_string(c->id.data));
    clay_json_object_set(call, "type", clay_json_string("function"));
    clay_json_object_set(call, "function", fn);
    clay_json_array_push(calls, call);
  }
  clay_json_object_set(assistant, "tool_calls", calls);
  clay_json_array_push(messages, assistant);
  for (size_t i = 0; i < s->calls.count; i++) {
    CodexToolCall *c = clay_array_get(&s->calls, i);
    if (callbacks && callbacks->on_tool_call)
      callbacks->on_tool_call(c->name.data, c->arguments.data,
                              callbacks->userdata);
    const ClayTool *tool = NULL;
    for (size_t j = 0; j < tool_count; j++)
      if (strcmp(tools[j].name, c->name.data) == 0) {
        tool = &tools[j];
        break;
      }
    ClayJson *args = clay_json_parse(c->arguments.data, NULL);
    ClayJson *result = tool && tool->fn ? tool->fn(args, tool->userdata) : NULL;
    clay_json_free(args);
    if (!result)
      result = clay_json_object();
    if (callbacks && callbacks->on_tool_result)
      callbacks->on_tool_result(c->name.data, result, callbacks->userdata);
    ClayStr text;
    clay_str_init(&text);
    clay_json_stringify(result, &text);
    clay_json_free(result);
    ClayJson *tool_message = clay_json_object();
    clay_json_object_set(tool_message, "role", clay_json_string("tool"));
    clay_json_object_set(tool_message, "tool_call_id",
                         clay_json_string(c->id.data));
    clay_json_object_set(tool_message, "content", clay_json_string(text.data));
    clay_json_array_push(messages, tool_message);
    clay_str_free(&text);
  }
}
static int ensure_token(ClayOpenAICodex *client) {
  return client->credentials.expires_at > 0 &&
                 client->credentials.expires_at <= (long long)time(NULL) + 60
             ? refresh(client)
             : 0;
}
int clay_openai_codex_run(ClayOpenAICodex *client, ClayJson *messages,
                          const ClayTool *tools, size_t tool_count,
                          int max_rounds,
                          const ClayOpenAICallbacks *callbacks) {
  if (!client || ensure_token(client) != 0)
    return -1;
  for (int round = 0; round < max_rounds; round++) {
    int auth_retried = 0;
  retry_request:;
    ClayStr body = request_body(client, messages, tools, tool_count), auth;
    clay_str_init(&auth);
    clay_str_printf(&auth, "Bearer %s", client->credentials.access_token);
    ClayHttpHeader headers[] = {
        {"Authorization", auth.data},
        {"ChatGPT-Account-ID", client->credentials.account_id},
        {"Content-Type", "application/json"},
        {"Accept", "text/event-stream"},
        {"originator", CODEX_ORIGINATOR}};
    CodexStream stream;
    stream_init(&stream, callbacks);
    ClayHttpRequest req = {0};
    req.method = "POST";
    req.url = CODEX_BACKEND_URL "/responses";
    req.headers = headers;
    req.header_count = 5;
    req.body = body.data;
    req.body_len = body.len;
    req.on_chunk = stream_chunk;
    req.userdata = &stream;
    req.should_abort = stream_abort;
    req.abort_userdata = &stream;
    req.low_speed_limit = 1;
    req.low_speed_seconds = 60;
    req.max_response_bytes = CODEX_RESPONSE_LIMIT;
    ClayHttpResponse response;
    int transport = clay_http_request(&req, &response);
    clay_str_free(&body);
    clay_str_free(&auth);
    if (stream.cancelled) {
      clay_http_response_free(&response);
      stream_free(&stream);
      return 1;
    }
    if ((response.status == 401 || response.status == 403) && !auth_retried &&
        !client->refresh_failed && refresh(client) == 0) {
      auth_retried = 1;
      clay_http_response_free(&response);
      stream_free(&stream);
      goto retry_request;
    }
    if (transport || response.status < 200 || response.status >= 300 ||
        stream.malformed || stream.failed_event) {
      if (callbacks && callbacks->on_error)
        callbacks->on_error(response.status, stream.raw.data,
                            callbacks->userdata);
      clay_http_response_free(&response);
      stream_free(&stream);
      return -1;
    }
    clay_http_response_free(&response);
    if (!stream.calls.count) {
      ClayJson *reply = clay_json_object();
      clay_json_object_set(reply, "role", clay_json_string("assistant"));
      clay_json_object_set(reply, "content",
                           clay_json_string(stream.content.data));
      clay_json_array_push(messages, reply);
      stream_free(&stream);
      return 0;
    }
    apply_calls(messages, tools, tool_count, &stream, callbacks);
    stream_free(&stream);
  }
  /* Rounds spent with tool calls still coming: not a failure, just an
     unfinished turn the user can continue. */
  return CLAY_RUN_OUT_OF_ROUNDS;
}
static void append_model(ClayArray *models, ClayJson *item) {
  const char *id = NULL;
  if (clay_json_type(item) == CLAY_JSON_STRING) {
    id = clay_json_string_value(item);
  } else {
    static const char *keys[] = {"id", "slug", "model"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
      ClayJson *value = clay_json_object_get(item, keys[i]);
      if (clay_json_type(value) == CLAY_JSON_STRING &&
          *clay_json_string_value(value)) {
        id = clay_json_string_value(value);
        break;
      }
    }
  }
  if (!id || !*id)
    return;
  for (size_t i = 0; i < models->count; i++) {
    if (strcmp(*(char **)clay_array_get(models, i), id) == 0)
      return;
  }
  char *copy = strdup(id);
  clay_array_push_val(models, &copy);
}

int clay_openai_codex_list_models(ClayOpenAICodex *client, ClayArray *models) {
  if (!client || !models || ensure_token(client) != 0)
    return -1;
  int retried_auth = 0;
retry_models:;
  ClayStr auth;
  clay_str_init(&auth);
  clay_str_printf(&auth, "Bearer %s", client->credentials.access_token);
  ClayHttpHeader headers[] = {
      {"Authorization", auth.data},
      {"ChatGPT-Account-ID", client->credentials.account_id},
      {"Accept", "application/json"},
      {"originator", CODEX_ORIGINATOR}};
  ClayHttpRequest req = {0};
  req.method = "GET";
  req.url = CODEX_MODELS_URL;
  req.headers = headers;
  req.header_count = 4;
  req.timeout_seconds = 30;
  /* Codex's catalog includes per-model instructions and can exceed 2 MiB. */
  req.max_response_bytes = CODEX_MODELS_RESPONSE_LIMIT;
  ClayHttpResponse response;
  int rc = clay_http_request(&req, &response);
  clay_str_free(&auth);
  client->last_status = response.status;
  if ((response.status == 401 || response.status == 403) && !retried_auth &&
      !client->refresh_failed && refresh(client) == 0) {
    retried_auth = 1;
    clay_http_response_free(&response);
    goto retry_models;
  }
  if (rc || response.status < 200 || response.status >= 300) {
    clay_http_response_free(&response);
    return -1;
  }
  ClayJson *root = clay_json_parse(response.body, NULL);
  clay_http_response_free(&response);
  if (!root)
    return -1;
  ClayJson *items = clay_json_type(root) == CLAY_JSON_ARRAY
                        ? root
                        : clay_json_object_get(root, "data");
  if (clay_json_type(items) != CLAY_JSON_ARRAY)
    items = clay_json_object_get(root, "models");
  clay_array_init(models, sizeof(char *));
  for (size_t i = 0; i < clay_json_array_count(items); i++)
    append_model(models, clay_json_array_get(items, i));
  clay_json_free(root);
  return models->count ? 0 : -1;
}

long clay_openai_codex_last_status(const ClayOpenAICodex *client) {
  return client ? client->last_status : 0;
}
