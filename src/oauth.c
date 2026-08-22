#include "clay/oauth.h"

#include "clay/crypto.h"
#include "clay/encoding.h"
#include "clay/term.h"

#include <stdlib.h>
#include <string.h>

int clay_oauth_pkce_create(ClayOAuthPkce *out) {
  unsigned char verifier[48], state[32], nonce[32], digest[32];
  if (!out || clay_term_random_bytes(verifier, sizeof(verifier)) ||
      clay_term_random_bytes(state, sizeof(state)) ||
      clay_term_random_bytes(nonce, sizeof(nonce)))
    return -1;
  memset(out, 0, sizeof(*out));
  out->verifier = clay_base64url_encode(verifier, sizeof(verifier));
  out->state = clay_base64url_encode(state, sizeof(state));
  out->nonce = clay_base64url_encode(nonce, sizeof(nonce));
  if (!out->verifier || !out->state || !out->nonce)
    goto fail;
  clay_sha256(out->verifier, strlen(out->verifier), digest);
  out->challenge = clay_base64url_encode(digest, sizeof(digest));
  if (out->challenge)
    return 0;
fail:
  clay_oauth_pkce_free(out);
  return -1;
}
void clay_oauth_pkce_free(ClayOAuthPkce *pkce) {
  if (!pkce)
    return;
  free(pkce->verifier);
  free(pkce->challenge);
  free(pkce->state);
  free(pkce->nonce);
  memset(pkce, 0, sizeof(*pkce));
}
char *clay_oauth_pkce_challenge(const char *verifier) {
  if (!verifier)
    return NULL;
  unsigned char digest[32];
  clay_sha256(verifier, strlen(verifier), digest);
  return clay_base64url_encode(digest, sizeof(digest));
}
int clay_oauth_parse_callback(const char *target, const char *expected_path,
                              const char *expected_state,
                              ClayOAuthCallback *out) {
  if (out)
    memset(out, 0, sizeof(*out));
  if (!target || !expected_path || !expected_state || !out)
    return -1;
  const char *query = strchr(target, '?');
  size_t path_len = query ? (size_t)(query - target) : strlen(target);
  if (path_len != strlen(expected_path) ||
      strncmp(target, expected_path, path_len) != 0)
    return -1;
  char *code = NULL, *state = NULL;
  int oauth_error = 0;
  for (const char *part = query ? query + 1 : NULL; part && *part;) {
    const char *amp = strchr(part, '&');
    const char *end = amp ? amp : part + strlen(part);
    const char *equals = memchr(part, '=', (size_t)(end - part));
    size_t key_len = equals ? (size_t)(equals - part) : (size_t)(end - part);
    char *value = clay_form_url_decode(
        equals ? equals + 1 : end, (size_t)(end - (equals ? equals + 1 : end)));
    if (key_len == 4 && strncmp(part, "code", 4) == 0) {
      free(code);
      code = value;
      value = NULL;
    } else if (key_len == 5 && strncmp(part, "state", 5) == 0) {
      free(state);
      state = value;
      value = NULL;
    } else if (key_len == 5 && strncmp(part, "error", 5) == 0) {
      oauth_error = 1;
    }
    free(value);
    part = amp ? amp + 1 : NULL;
  }
  if (!oauth_error && code && *code && state &&
      strcmp(state, expected_state) == 0) {
    out->code = code;
    free(state);
    return 0;
  }
  free(code);
  free(state);
  return -1;
}
void clay_oauth_callback_free(ClayOAuthCallback *callback) {
  if (!callback)
    return;
  free(callback->code);
  memset(callback, 0, sizeof(*callback));
}
ClayJson *clay_jwt_decode_payload(const char *token) {
  if (!token)
    return NULL;
  const char *first = strchr(token, '.');
  const char *last = first ? strchr(first + 1, '.') : NULL;
  if (!first || !last)
    return NULL;
  size_t len = (size_t)(last - first - 1);
  char *segment = malloc(len + 1);
  if (!segment)
    return NULL;
  memcpy(segment, first + 1, len);
  segment[len] = '\0';
  char *payload = clay_base64url_decode(segment);
  free(segment);
  if (!payload)
    return NULL;
  ClayJson *json = clay_json_parse(payload, NULL);
  free(payload);
  return json;
}
