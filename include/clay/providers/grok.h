#ifndef CLAY_PROVIDERS_GROK_H
#define CLAY_PROVIDERS_GROK_H

#include "clay/array.h"
#include "clay/json.h"
#include "clay/oauth.h"
#include "clay/providers/openai.h"
#include "clay/str.h"

typedef struct ClayGrok ClayGrok;

/* xAI's public API uses the ordinary OpenAI-compatible client. These
   constants and the subscription protocol intentionally live here. */
#define CLAY_GROK_API_URL "https://api.x.ai/v1"
#define CLAY_GROK_SUBSCRIPTION_URL "https://cli-chat-proxy.grok.com/v1"

typedef struct {
  char *access_token;
  char *refresh_token;
  char *id_token;
  long long expires_at;
} ClayGrokCredentials;

const char *clay_grok_subscription_token_auth_header(void);
/* Default only; a model selected from the subscription catalog replaces it in
   the proxy's x-grok-model-override header. */
const char *clay_grok_subscription_model_override(void);
const char *clay_grok_subscription_client_version(void);

char *clay_grok_authorization_url(const char *redirect_uri,
                                  const char *challenge, const char *state,
                                  const char *nonce);

/* Parses one complete HTTP callback request. Return 0 for a completed
   callback, 1 for an accepted CORS preflight (keep listening), and -1 for a
   rejected request. `status_out` and `allow_cors_out` describe a safe reply. */
int clay_grok_oauth_handle_callback_request(const char *request,
                                            const char *expected_state,
                                            ClayOAuthCallback *out,
                                            int *status_out,
                                            int *allow_cors_out);

int clay_grok_authenticate(ClayGrokCredentials *out, ClayStr *error);
void clay_grok_credentials_free(ClayGrokCredentials *credentials);
/* Applies a successful standard OAuth token response. `preserve_refresh`
   permits a refresh response to omit refresh_token; a returned one replaces
   the saved token (rotation). */
int clay_grok_credentials_apply_token_response(ClayJson *response,
                                               ClayGrokCredentials *out,
                                               int preserve_refresh);

ClayGrok *clay_grok_create(const ClayGrokCredentials *credentials,
                           const char *model);
void clay_grok_destroy(ClayGrok *client);
void clay_grok_set_reasoning_effort(ClayGrok *client, const char *effort);
int clay_grok_list_models(ClayGrok *client, ClayArray *models);
int clay_grok_run(ClayGrok *client, ClayJson *messages, const ClayTool *tools,
                  size_t tool_count, int max_rounds,
                  const ClayOpenAICallbacks *callbacks);
void clay_grok_copy_credentials(const ClayGrok *client,
                                ClayGrokCredentials *out);
int clay_grok_authentication_invalid(const ClayGrok *client);
long clay_grok_last_status(const ClayGrok *client);

#endif /* CLAY_PROVIDERS_GROK_H */
