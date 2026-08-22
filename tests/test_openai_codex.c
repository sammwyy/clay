#include "clay/json.h"
#include "clay/providers/openai_codex.h"
#include "clay/str.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void collect_delta(const char *text, void *userdata) {
  ClayJson *root = clay_json_parse(text, NULL);
  if (root && strcmp(clay_json_string_value(clay_json_object_get(root, "type")),
                     "response.output_text.delta") == 0) {
    clay_str_push(userdata,
                  clay_json_string_value(clay_json_object_get(root, "delta")));
  }
  clay_json_free(root);
}

static void test_pkce_and_url(void) {
  char *challenge = clay_openai_codex_pkce_challenge(
      "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk");
  assert(strcmp(challenge, "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM") == 0);
  free(challenge);

  char *verifier = NULL, *generated_challenge = NULL, *state = NULL;
  assert(clay_openai_codex_create_pkce(&verifier, &generated_challenge,
                                       &state) == 0);
  assert(strlen(verifier) >= 43 && strchr(verifier, '=') == NULL);
  assert(strlen(generated_challenge) == 43 &&
         strchr(generated_challenge, '=') == NULL);
  char *url = clay_openai_codex_authorization_url(generated_challenge, state);
  assert(strstr(url, "client_id=app_EMoamEEZ73f0CkXaXp7hrann"));
  assert(strstr(
      url, "redirect_uri=http%3A%2F%2Flocalhost%3A1455%2Fauth%2Fcallback"));
  assert(strstr(url, "scope=openid%20profile%20email%20offline_access"));
  assert(strstr(url, "code_challenge_method=S256"));
  assert(strstr(url, "state="));
  free(url);
  clay_openai_codex_pkce_free(verifier, generated_challenge, state);
}

static void test_callback(void) {
  ClayCodexCallback callback;
  assert(clay_openai_codex_parse_callback(
             "/auth/callback?code=test-code&state=state", "state", &callback) ==
         0);
  assert(callback.ok && strcmp(callback.code, "test-code") == 0);
  clay_openai_codex_callback_free(&callback);
  assert(clay_openai_codex_parse_callback("/auth/callback?code=x&state=wrong",
                                          "state", &callback) != 0);
  assert(clay_openai_codex_parse_callback(
             "/auth/callback?error=access_denied&state=state", "state",
             &callback) != 0);
  assert(clay_openai_codex_parse_callback("/auth/callback?state=state", "state",
                                          &callback) != 0);
  assert(clay_openai_codex_parse_callback("/unexpected?code=x&state=state",
                                          "state", &callback) != 0);
}

static void test_account_id(void) {
  const char *jwt = "header."
                    "eyJodHRwczovL2FwaS5vcGVuYWkuY29tL2F1dGgiOnsiY2hhdGdwdF9hY2"
                    "NvdW50X2lkIjoiYWNjb3VudC10ZXN0In19.signature";
  char *account = NULL;
  assert(clay_openai_codex_extract_account_id(jwt, &account) == 0);
  assert(strcmp(account, "account-test") == 0);
  free(account);
}

static void test_split_sse(void) {
  ClayStr text;
  clay_str_init(&text);
  ClayCodexSseParser *parser =
      clay_openai_codex_sse_create(collect_delta, &text);
  const char *first = "data: {\"type\":\"response.output";
  const char *second = "_text.delta\",\"delta\":\"hel";
  const char *third = "lo\"}\n\n";
  assert(clay_openai_codex_sse_feed(parser, first, strlen(first)) == 0);
  assert(clay_openai_codex_sse_feed(parser, second, strlen(second)) == 0);
  assert(clay_openai_codex_sse_feed(parser, third, strlen(third)) == 0);
  assert(strcmp(text.data, "hello") == 0);
  clay_openai_codex_sse_destroy(parser);
  clay_str_free(&text);
}

int main(void) {
  test_pkce_and_url();
  test_callback();
  test_account_id();
  test_split_sse();
  puts("openai-codex tests passed");
  return 0;
}
