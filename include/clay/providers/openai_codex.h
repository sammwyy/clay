#ifndef CLAY_PROVIDERS_OPENAI_CODEX_H
#define CLAY_PROVIDERS_OPENAI_CODEX_H

#include "clay/array.h"
#include "clay/json.h"
#include "clay/providers/openai.h"
#include "clay/sse.h"
#include "clay/str.h"

typedef struct ClayOpenAICodex ClayOpenAICodex;

/* ChatGPT/Codex subscription OAuth credentials. They are intentionally
   separate from an OpenAI Platform API key. */
typedef struct {
  char *access_token;
  char *refresh_token;
  char *id_token;
  char *account_id;
  long long expires_at;
} ClayCodexCredentials;

typedef struct {
  char *code;
  int ok;
} ClayCodexCallback;

/* Generates a PKCE verifier/challenge and state. Every returned string is
   malloc'd and must be freed by clay_openai_codex_pkce_free. */
int clay_openai_codex_create_pkce(char **verifier, char **challenge,
                                  char **state);
char *clay_openai_codex_pkce_challenge(const char *verifier);
void clay_openai_codex_pkce_free(char *verifier, char *challenge, char *state);
char *clay_openai_codex_authorization_url(const char *challenge,
                                          const char *state);
int clay_openai_codex_extract_account_id(const char *id_token,
                                         char **account_id_out);
/* Parses one callback target for tests and the loopback listener. */
int clay_openai_codex_parse_callback(const char *target,
                                     const char *expected_state,
                                     ClayCodexCallback *out);
void clay_openai_codex_callback_free(ClayCodexCallback *callback);

/* Starts browser OAuth and returns a fully populated credential set. The
   error text is safe to show (it never includes secrets). */
int clay_openai_codex_authenticate(ClayCodexCredentials *out, ClayStr *error);
void clay_openai_codex_credentials_free(ClayCodexCredentials *credentials);

ClayOpenAICodex *
clay_openai_codex_create(const ClayCodexCredentials *credentials,
                         const char *model);
void clay_openai_codex_destroy(ClayOpenAICodex *client);
/* NULL omits reasoning from Responses requests and uses the model default. */
void clay_openai_codex_set_reasoning_effort(ClayOpenAICodex *client,
                                            const char *effort);
int clay_openai_codex_list_models(ClayOpenAICodex *client, ClayArray *models);
long clay_openai_codex_last_status(const ClayOpenAICodex *client);
/* Caller saves a copied result to its existing credential store after this
   call, which persists refresh-token rotation. */
void clay_openai_codex_copy_credentials(const ClayOpenAICodex *client,
                                        ClayCodexCredentials *out);
int clay_openai_codex_authentication_invalid(const ClayOpenAICodex *client);
int clay_openai_codex_run(ClayOpenAICodex *client, ClayJson *messages,
                          const ClayTool *tools, size_t tool_count,
                          int max_rounds, const ClayOpenAICallbacks *callbacks);

/* Small incremental SSE helper used by the provider and focused tests. */
typedef ClaySseParser ClayCodexSseParser;
ClayCodexSseParser *clay_openai_codex_sse_create(void (*on_json)(const char *,
                                                                 void *),
                                                 void *userdata);
int clay_openai_codex_sse_feed(ClayCodexSseParser *parser, const char *data,
                               size_t len);
void clay_openai_codex_sse_destroy(ClayCodexSseParser *parser);

#endif
