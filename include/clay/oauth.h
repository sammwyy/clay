#ifndef CLAY_OAUTH_H
#define CLAY_OAUTH_H

#include "clay/json.h"

typedef struct {
  char *verifier;
  char *challenge;
  char *state;
} ClayOAuthPkce;

typedef struct {
  char *code;
} ClayOAuthCallback;

int clay_oauth_pkce_create(ClayOAuthPkce *out);
void clay_oauth_pkce_free(ClayOAuthPkce *pkce);
char *clay_oauth_pkce_challenge(const char *verifier);

/* Validates the expected callback path and state, rejecting OAuth errors and
   missing authorization codes. */
int clay_oauth_parse_callback(const char *target, const char *expected_path,
                              const char *expected_state,
                              ClayOAuthCallback *out);
void clay_oauth_callback_free(ClayOAuthCallback *callback);

/* Decodes (but does not cryptographically verify) a JWT payload. */
ClayJson *clay_jwt_decode_payload(const char *token);

#endif /* CLAY_OAUTH_H */
