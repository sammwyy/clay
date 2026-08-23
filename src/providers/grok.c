/* Grok account authentication and its Build-compatible proxy details live
   here. Normal xAI API-key traffic remains the generic OpenAI client. */
#include "clay/providers/grok.h"

#include "clay/encoding.h"
#include "clay/http.h"
#include "clay/oauth.h"
#include "clay/term.h"

#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define GROK_ISSUER "https://auth.x.ai"
#define GROK_CLIENT_ID "b1a00492-073a-47ea-816f-4c329264a828"
#define GROK_AUTH_URL GROK_ISSUER "/oauth2/authorize"
#define GROK_TOKEN_URL GROK_ISSUER "/oauth2/token"
#define GROK_SCOPES                                                            \
  "openid profile email offline_access grok-cli:access api:access "            \
  "conversations:read conversations:write workspaces:read workspaces:write"
#define GROK_CALLBACK_PATH "/callback"
#define GROK_CALLBACK_TIMEOUT_SECONDS 180
#define GROK_ALLOWED_ORIGIN "https://accounts.x.ai"
#define GROK_TOKEN_AUTH "xai-grok-cli"
#define GROK_MODEL_OVERRIDE "grok-build"
/* cli-chat-proxy rejects a missing client version with HTTP 426. Keep this
   compatibility value with the other Build proxy details. */
#define GROK_CLIENT_VERSION "0.2.101"
#define GROK_REFRESH_EARLY_SECONDS 60

struct ClayGrok {
  ClayGrokCredentials credentials;
  ClayOpenAI *openai;
  int refresh_failed;
  int authentication_invalid;
};

const char *clay_grok_subscription_token_auth_header(void) {
  return GROK_TOKEN_AUTH;
}
const char *clay_grok_subscription_model_override(void) {
  return GROK_MODEL_OVERRIDE;
}
const char *clay_grok_subscription_client_version(void) {
  return GROK_CLIENT_VERSION;
}

static void set_error(ClayStr *error, const char *text) {
  if (!error)
    return;
  clay_str_clear(error);
  clay_str_push(error, text);
}

char *clay_grok_authorization_url(const char *redirect_uri,
                                  const char *challenge, const char *state,
                                  const char *nonce) {
  if (!redirect_uri || !challenge || !state || !nonce)
    return NULL;
  char *redirect = clay_url_encode(redirect_uri);
  char *scope = clay_url_encode(GROK_SCOPES);
  char *encoded_challenge = clay_url_encode(challenge);
  char *encoded_state = clay_url_encode(state);
  char *encoded_nonce = clay_url_encode(nonce);
  if (!redirect || !scope || !encoded_challenge || !encoded_state ||
      !encoded_nonce) {
    free(redirect); free(scope); free(encoded_challenge); free(encoded_state);
    free(encoded_nonce);
    return NULL;
  }
  ClayStr url;
  clay_str_init(&url);
  clay_str_printf(&url,
      "%s?response_type=code&client_id=%s&redirect_uri=%s&scope=%s&"
      "code_challenge=%s&code_challenge_method=S256&state=%s&nonce=%s&"
      "referrer=grok-build",
      GROK_AUTH_URL, GROK_CLIENT_ID, redirect, scope, encoded_challenge,
      encoded_state, encoded_nonce);
  free(redirect); free(scope); free(encoded_challenge); free(encoded_state);
  free(encoded_nonce);
  return url.data;
}

static int header_value_is(const char *request, const char *name,
                           const char *expected) {
  size_t name_len = strlen(name);
  const char *line = request;
  while (line && *line) {
    const char *end = strstr(line, "\r\n");
    if (!end)
      end = strchr(line, '\n');
    if (!end)
      end = line + strlen(line);
    int name_matches = (size_t)(end - line) > name_len + 1;
    for (size_t i = 0; name_matches && i < name_len; i++)
      name_matches = tolower((unsigned char)line[i]) ==
                     tolower((unsigned char)name[i]);
    if (name_matches && line[name_len] == ':') {
      const char *value = line + name_len + 1;
      while (value < end && (*value == ' ' || *value == '\t'))
        value++;
      return strlen(expected) == (size_t)(end - value) &&
             strncmp(value, expected, (size_t)(end - value)) == 0;
    }
    line = *end ? end + (end[0] == '\r' && end[1] == '\n' ? 2 : 1) : NULL;
  }
  return 0;
}

int clay_grok_oauth_handle_callback_request(const char *request,
                                            const char *expected_state,
                                            ClayOAuthCallback *out,
                                            int *status_out,
                                            int *allow_cors_out) {
  if (status_out) *status_out = 400;
  if (allow_cors_out) *allow_cors_out = 0;
  if (out) memset(out, 0, sizeof(*out));
  if (!request || !expected_state || !out)
    return -1;
  char method[8] = {0}, target[8192] = {0}, version[16] = {0};
  if (sscanf(request, "%7s %8191s %15s", method, target, version) != 3) {
    if (status_out) *status_out = 400;
    return -1;
  }
  int cors = header_value_is(request, "Origin", GROK_ALLOWED_ORIGIN);
  if (allow_cors_out) *allow_cors_out = cors;
  const char *query = strchr(target, '?');
  size_t path_len = query ? (size_t)(query - target) : strlen(target);
  if (path_len != strlen(GROK_CALLBACK_PATH) ||
      strncmp(target, GROK_CALLBACK_PATH, path_len) != 0) {
    if (status_out) *status_out = 404;
    return -1;
  }
  if (strcmp(method, "OPTIONS") == 0) {
    if (!cors) {
      if (status_out) *status_out = 403;
      return -1;
    }
    if (status_out) *status_out = 204;
    return 1;
  }
  /* accounts.x.ai can forward the callback through a browser fetch. The
     authorization parameters still live in the callback URL, so accept the
     normal redirect GET and this equivalent POST without parsing a body. */
  if (strcmp(method, "GET") != 0 && strcmp(method, "POST") != 0) {
    if (status_out) *status_out = 405;
    return -1;
  }
  if (clay_oauth_parse_callback(target, GROK_CALLBACK_PATH, expected_state,
                                out) != 0) {
    if (status_out) *status_out = 400;
    return -1;
  }
  if (status_out) *status_out = 200;
  return 0;
}

static const char *cors_headers(int allow_cors) {
  return allow_cors
             ? "Access-Control-Allow-Origin: https://accounts.x.ai\r\n"
               "Access-Control-Allow-Methods: GET, OPTIONS\r\n"
               "Access-Control-Allow-Headers: Content-Type\r\n"
             : NULL;
}

static int wait_callback(ClayTermHttpServer *listener, const char *state,
                         ClayOAuthCallback *out, ClayStr *error) {
  time_t deadline = time(NULL) + GROK_CALLBACK_TIMEOUT_SECONDS;
  ClayStr request;
  clay_str_init(&request);
  while (time(NULL) < deadline) {
    if (clay_term_take_interrupt()) {
      set_error(error, "Authentication cancelled.");
      clay_str_free(&request);
      return -1;
    }
    int ready = clay_term_http_server_receive(listener, &request, 1000);
    if (ready == 0)
      continue;
    if (ready < 0) {
      /* Browsers may open and close a speculative loopback connection before
         the real redirect. It carries no callback data, so do not turn it
         into a failed login. */
      if (request.len == 0)
        continue;
      set_error(error, "Invalid OAuth callback.");
      clay_str_free(&request);
      return -1;
    }
    int status = 400, allow_cors = 0;
    int result = clay_grok_oauth_handle_callback_request(
        request.data, state, out, &status, &allow_cors);
    const char *body = result == 0
        ? "<!doctype html><title>Authentication complete</title><p>Authentication complete. You can return to the application.</p>"
        : "<!doctype html><title>Authentication failed</title><p>Authentication failed. You can return to the application.</p>";
    clay_term_http_server_reply_with_headers(listener, status, body,
                                             cors_headers(allow_cors));
    if (result == 0) {
      clay_str_free(&request);
      return 0;
    }
    if (result < 0) {
      set_error(error, "Grok authorization failed or returned an invalid callback.");
      clay_str_free(&request);
      return -1;
    }
  }
  set_error(error, "Timed out waiting for Grok authorization.");
  clay_str_free(&request);
  return -1;
}

static int token_request(const char *body, ClayJson **json_out) {
  ClayHttpHeader headers[] = {{"Content-Type", "application/x-www-form-urlencoded"},
                              {"Accept", "application/json"}};
  ClayHttpRequest request = {0};
  request.method = "POST";
  request.url = GROK_TOKEN_URL;
  request.headers = headers;
  request.header_count = 2;
  request.body = body;
  request.body_len = strlen(body);
  request.timeout_seconds = 30;
  request.max_response_bytes = 1024 * 1024;
  ClayHttpResponse response;
  int rc = clay_http_request(&request, &response);
  if (rc || response.status < 200 || response.status >= 300) {
    clay_http_response_free(&response);
    return -1;
  }
  *json_out = clay_json_parse(response.body, NULL);
  clay_http_response_free(&response);
  return *json_out ? 0 : -1;
}

int clay_grok_credentials_apply_token_response(ClayJson *root,
                                               ClayGrokCredentials *out,
                                               int preserve_refresh) {
  if (!root || !out)
    return -1;
  const char *access = clay_json_string_value(
      clay_json_object_get(root, "access_token"));
  const char *refresh = clay_json_string_value(
      clay_json_object_get(root, "refresh_token"));
  const char *id = clay_json_string_value(clay_json_object_get(root, "id_token"));
  long expires = (long)clay_json_number_value(
      clay_json_object_get(root, "expires_in"));
  if (!access || !*access || expires <= 0 ||
      ((!refresh || !*refresh) && !preserve_refresh))
    return -1;
  char *access_copy = strdup(access);
  char *refresh_copy = refresh && *refresh ? strdup(refresh) : NULL;
  char *id_copy = id && *id ? strdup(id) : NULL;
  if (!access_copy || (refresh && *refresh && !refresh_copy) ||
      (id && *id && !id_copy)) {
    free(access_copy); free(refresh_copy); free(id_copy);
    return -1;
  }
  free(out->access_token);
  out->access_token = access_copy;
  if (refresh_copy) {
    free(out->refresh_token);
    out->refresh_token = refresh_copy;
  }
  if (id_copy) {
    free(out->id_token);
    out->id_token = id_copy;
  }
  out->expires_at = (long long)time(NULL) + expires;
  return 0;
}

int clay_grok_authenticate(ClayGrokCredentials *out, ClayStr *error) {
  if (!out) return -1;
  memset(out, 0, sizeof(*out));
  ClayOAuthPkce pkce = {0};
  ClayOAuthCallback callback = {0};
  ClayTermHttpServer *listener = NULL;
  ClayJson *token = NULL;
  char *url = NULL;
  int rc = -1;
  if (clay_oauth_pkce_create(&pkce) != 0) {
    set_error(error, "Could not create secure OAuth credentials.");
    goto done;
  }
  listener = clay_term_http_server_create(0);
  if (!listener) {
    set_error(error, "Could not listen on the loopback OAuth callback port.");
    goto done;
  }
  ClayStr redirect;
  clay_str_init(&redirect);
  clay_str_printf(&redirect, "http://127.0.0.1:%u%s",
                  clay_term_http_server_port(listener), GROK_CALLBACK_PATH);
  url = clay_grok_authorization_url(redirect.data, pkce.challenge, pkce.state,
                                    pkce.nonce);
  if (!url) {
    clay_str_free(&redirect);
    set_error(error, "Could not create authorization URL.");
    goto done;
  }
  if (clay_term_open_browser(url) != 0)
    fprintf(stderr, "Could not open a browser. Open this URL manually:\n%s\n", url);
  if (wait_callback(listener, pkce.state, &callback, error) != 0) {
    clay_str_free(&redirect);
    goto done;
  }
  clay_term_http_server_destroy(listener);
  listener = NULL;
  char *code = clay_url_encode(callback.code);
  char *encoded_redirect = clay_url_encode(redirect.data);
  char *verifier = clay_url_encode(pkce.verifier);
  clay_str_free(&redirect);
  if (!code || !encoded_redirect || !verifier) {
    free(code); free(encoded_redirect); free(verifier);
    set_error(error, "Could not prepare Grok authorization exchange.");
    goto done;
  }
  ClayStr body;
  clay_str_init(&body);
  clay_str_printf(&body,
      "grant_type=authorization_code&client_id=%s&code=%s&redirect_uri=%s&code_verifier=%s",
      GROK_CLIENT_ID, code, encoded_redirect, verifier);
  free(code); free(encoded_redirect); free(verifier);
  if (token_request(body.data, &token) != 0) {
    clay_str_free(&body);
    set_error(error, "Could not exchange the Grok authorization code.");
    goto done;
  }
  clay_str_free(&body);
  if (clay_grok_credentials_apply_token_response(token, out, 0) != 0) {
    set_error(error, "Grok authorization response did not include usable credentials.");
    clay_grok_credentials_free(out);
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

void clay_grok_credentials_free(ClayGrokCredentials *credentials) {
  if (!credentials) return;
  free(credentials->access_token);
  free(credentials->refresh_token);
  free(credentials->id_token);
  memset(credentials, 0, sizeof(*credentials));
}

static int refresh(ClayGrok *client) {
  if (!client || client->refresh_failed || !client->credentials.refresh_token)
    return -1;
  char *token = clay_url_encode(client->credentials.refresh_token);
  if (!token) return -1;
  ClayStr body;
  clay_str_init(&body);
  clay_str_printf(&body, "grant_type=refresh_token&client_id=%s&refresh_token=%s",
                  GROK_CLIENT_ID, token);
  free(token);
  ClayJson *response = NULL;
  int rc = token_request(body.data, &response);
  clay_str_free(&body);
  if (rc || clay_grok_credentials_apply_token_response(
                response, &client->credentials, 1) != 0) {
    client->refresh_failed = 1;
    client->authentication_invalid = 1;
    clay_json_free(response);
    return -1;
  }
  clay_openai_set_api_key(client->openai, client->credentials.access_token);
  clay_json_free(response);
  return 0;
}

static int ensure_access_token(ClayGrok *client) {
  if (!client || !client->credentials.access_token)
    return -1;
  if ((long long)time(NULL) < client->credentials.expires_at - GROK_REFRESH_EARLY_SECONDS)
    return 0;
  return refresh(client);
}

ClayGrok *clay_grok_create(const ClayGrokCredentials *credentials,
                           const char *model) {
  if (!credentials || !credentials->access_token || !credentials->refresh_token)
    return NULL;
  ClayGrok *client = calloc(1, sizeof(*client));
  if (!client) return NULL;
  client->credentials.access_token = strdup(credentials->access_token);
  client->credentials.refresh_token = strdup(credentials->refresh_token);
  client->credentials.id_token = credentials->id_token ? strdup(credentials->id_token) : NULL;
  client->credentials.expires_at = credentials->expires_at;
  const char *model_override = model && *model ? model : GROK_MODEL_OVERRIDE;
  ClayOpenAIHeader headers[] = {{"X-XAI-Token-Auth", GROK_TOKEN_AUTH},
                                {"x-grok-model-override", model_override},
                                {"x-grok-client-version", GROK_CLIENT_VERSION}};
  client->openai = clay_openai_create_with_headers(
      CLAY_GROK_SUBSCRIPTION_URL, client->credentials.access_token,
      model_override, headers, 3);
  if (!client->credentials.access_token || !client->credentials.refresh_token ||
      !client->openai) {
    clay_grok_destroy(client);
    return NULL;
  }
  return client;
}

void clay_grok_destroy(ClayGrok *client) {
  if (!client) return;
  clay_openai_destroy(client->openai);
  clay_grok_credentials_free(&client->credentials);
  free(client);
}
void clay_grok_set_reasoning_effort(ClayGrok *client, const char *effort) {
  if (client) clay_openai_set_reasoning_effort(client->openai, effort);
}
void clay_grok_set_conversation_id(ClayGrok *client, const char *id) {
  if (client)
    clay_openai_set_extra_header(client->openai, "x-grok-conv-id", id);
}
int clay_grok_list_models(ClayGrok *client, ClayArray *models) {
  if (!client || ensure_access_token(client) != 0) return -1;
  /* The proxy catalog is subscription/account-specific. Do not seed it with
     public API model names; the default Grok Build route is used only when
     no model has been selected yet. */
  ClayOpenAIHeader headers[] = {{"X-XAI-Token-Auth", GROK_TOKEN_AUTH},
                                {"x-grok-client-version", GROK_CLIENT_VERSION}};
  ClayOpenAI *catalog = clay_openai_create_with_headers(
      CLAY_GROK_SUBSCRIPTION_URL, client->credentials.access_token, "", headers,
      2);
  if (!catalog)
    return -1;
  int rc = clay_openai_list_models(catalog, models);
  clay_openai_destroy(catalog);
  return rc;
}
int clay_grok_run(ClayGrok *client, ClayJson *messages, const ClayTool *tools,
                  size_t tool_count, int max_rounds,
                  const ClayOpenAICallbacks *callbacks) {
  if (ensure_access_token(client) != 0) return -1;
  int rc = clay_openai_run(client->openai, messages, tools, tool_count,
                           max_rounds, callbacks);
  /* A token can be revoked before its advertised expiry. One refresh/retry is
     enough; never turn an authentication failure into a retry loop. */
  if (rc == -1 && clay_openai_last_status(client->openai) == 401 &&
      refresh(client) == 0)
    rc = clay_openai_run(client->openai, messages, tools, tool_count,
                         max_rounds, callbacks);
  return rc;
}
void clay_grok_copy_credentials(const ClayGrok *client, ClayGrokCredentials *out) {
  if (!client || !out) return;
  clay_grok_credentials_free(out);
  out->access_token = strdup(client->credentials.access_token);
  out->refresh_token = strdup(client->credentials.refresh_token);
  out->id_token = client->credentials.id_token ? strdup(client->credentials.id_token) : NULL;
  out->expires_at = client->credentials.expires_at;
}
int clay_grok_authentication_invalid(const ClayGrok *client) {
  return client && client->authentication_invalid;
}
long clay_grok_last_status(const ClayGrok *client) {
  return client ? clay_openai_last_status(client->openai) : 0;
}
