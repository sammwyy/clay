#include "clay/json.h"
#include "clay/oauth.h"
#include "clay/providers/grok.h"
#include "clay/providers/openai.h"
#include "clay/sse.h"
#include "clay/str.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void collect_openai_delta(const char *text, void *userdata) {
  ClayJson *root = clay_json_parse(text, NULL);
  ClayJson *choice = clay_json_array_get(clay_json_object_get(root, "choices"), 0);
  const char *content = clay_json_string_value(
      clay_json_object_get(clay_json_object_get(choice, "delta"), "content"));
  if (content)
    clay_str_push(userdata, content);
  clay_json_free(root);
}

static void test_pkce_and_authorization_url(void) {
  char *challenge = clay_oauth_pkce_challenge(
      "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk");
  assert(strcmp(challenge, "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM") == 0);
  free(challenge);
  ClayOAuthPkce pkce = {0};
  assert(clay_oauth_pkce_create(&pkce) == 0);
  assert(strlen(pkce.verifier) >= 43 && strchr(pkce.verifier, '=') == NULL);
  assert(strlen(pkce.challenge) == 43 && strchr(pkce.challenge, '=') == NULL);
  assert(pkce.nonce && strchr(pkce.nonce, '=') == NULL);
  char *url = clay_grok_authorization_url("http://127.0.0.1:4567/callback",
                                          pkce.challenge, pkce.state, pkce.nonce);
  assert(strstr(url, "response_type=code"));
  assert(strstr(url, "client_id=b1a00492-073a-47ea-816f-4c329264a828"));
  assert(strstr(url, "redirect_uri=http%3A%2F%2F127.0.0.1%3A4567%2Fcallback"));
  assert(strstr(url, "scope=openid%20profile%20email%20offline_access"));
  assert(strstr(url, "code_challenge="));
  assert(strstr(url, "code_challenge_method=S256"));
  assert(strstr(url, "state="));
  assert(strstr(url, "nonce="));
  assert(strstr(url, "referrer=grok-build"));
  free(url);
  clay_oauth_pkce_free(&pkce);
}

static void test_callback_and_cors(void) {
  ClayOAuthCallback callback = {0};
  int status = 0, cors = 0;
  assert(clay_grok_oauth_handle_callback_request(
             "GET /callback?code=test-code&state=state HTTP/1.1\r\nHost: localhost\r\n\r\n",
             "state", &callback, &status, &cors) == 0);
  assert(status == 200 && !cors && strcmp(callback.code, "test-code") == 0);
  clay_oauth_callback_free(&callback);
  assert(clay_grok_oauth_handle_callback_request(
             "POST /callback?code=test-code&state=state HTTP/1.1\r\nOrigin: https://accounts.x.ai\r\n\r\n",
             "state", &callback, &status, &cors) == 0);
  assert(status == 200 && cors && strcmp(callback.code, "test-code") == 0);
  clay_oauth_callback_free(&callback);
  assert(clay_grok_oauth_handle_callback_request(
             "GET /callback?code=x&state=wrong HTTP/1.1\r\n\r\n", "state",
             &callback, &status, &cors) == -1 && status == 400);
  assert(clay_grok_oauth_handle_callback_request(
             "GET /callback?state=state HTTP/1.1\r\n\r\n", "state",
             &callback, &status, &cors) == -1 && status == 400);
  assert(clay_grok_oauth_handle_callback_request(
             "GET /callback?error=access_denied&state=state HTTP/1.1\r\n\r\n",
             "state", &callback, &status, &cors) == -1 && status == 400);
  assert(clay_grok_oauth_handle_callback_request(
             "GET /other?code=x&state=state HTTP/1.1\r\n\r\n", "state",
             &callback, &status, &cors) == -1 && status == 404);
  assert(clay_grok_oauth_handle_callback_request(
             "OPTIONS /callback HTTP/1.1\r\nOrigin: https://accounts.x.ai\r\n\r\n",
             "state", &callback, &status, &cors) == 1 && status == 204 && cors);
  assert(clay_grok_oauth_handle_callback_request(
             "OPTIONS /callback HTTP/1.1\r\nOrigin: https://example.test\r\n\r\n",
             "state", &callback, &status, &cors) == -1 && status == 403 && !cors);
}

static void test_refresh_rotation_and_routing(void) {
  ClayGrokCredentials credentials = {strdup("old-access"), strdup("old-refresh"),
                                     NULL, 1};
  ClayJson *token = clay_json_parse(
      "{\"access_token\":\"new-access\",\"refresh_token\":\"new-refresh\",\"expires_in\":21600}",
      NULL);
  assert(clay_grok_credentials_apply_token_response(token, &credentials, 1) == 0);
  assert(strcmp(credentials.access_token, "new-access") == 0);
  assert(strcmp(credentials.refresh_token, "new-refresh") == 0);
  assert(credentials.expires_at > 1);
  clay_json_free(token);
  clay_grok_credentials_free(&credentials);
  assert(strcmp(CLAY_GROK_API_URL, "https://api.x.ai/v1") == 0);
  assert(strcmp(CLAY_GROK_SUBSCRIPTION_URL,
                "https://cli-chat-proxy.grok.com/v1") == 0);
  assert(strcmp(clay_grok_subscription_token_auth_header(), "xai-grok-cli") == 0);
  assert(strcmp(clay_grok_subscription_model_override(), "grok-build") == 0);
  assert(strcmp(clay_grok_subscription_client_version(), "0.2.101") == 0);
}

static void test_fragmented_openai_sse(void) {
  ClayStr output;
  clay_str_init(&output);
  ClaySseParser *parser = clay_sse_create(256 * 1024, collect_openai_delta, &output);
  const char *first = "data: {\"choices\":[{\"delta\":{\"cont";
  assert(clay_sse_feed(parser, first, strlen(first)) == 0);
  const char *second = "ent\":\"hel\"}}]}\n\n";
  const char *third = "data: {\"choices\":[{\"delta\":{\"content\":\"lo\"}}]}\r\n\r\n";
  assert(clay_sse_feed(parser, second, strlen(second)) == 0);
  assert(clay_sse_feed(parser, third, strlen(third)) == 0);
  assert(strcmp(output.data, "hello") == 0);
  clay_sse_destroy(parser);
  clay_str_free(&output);
}

static void test_usage_cache_details(void) {
  ClayJson *chat = clay_json_parse(
      "{\"prompt_tokens\":125,\"completion_tokens\":48,"
      "\"prompt_tokens_details\":{\"cached_tokens\":98}}",
      NULL);
  ClayTokenUsage usage;
  clay_openai_usage_from_json(chat, &usage);
  assert(usage.input_tokens == 125 && usage.output_tokens == 48);
  assert(usage.cached_input_tokens_known && usage.cached_input_tokens == 98);
  clay_json_free(chat);

  ClayJson *responses = clay_json_parse(
      "{\"input_tokens\":125,\"output_tokens\":48,"
      "\"input_tokens_details\":{\"cached_tokens\":0}}",
      NULL);
  clay_openai_usage_from_json(responses, &usage);
  assert(usage.input_tokens == 125 && usage.output_tokens == 48);
  assert(usage.cached_input_tokens_known && usage.cached_input_tokens == 0);
  clay_json_free(responses);

  ClayJson *unknown = clay_json_object();
  clay_openai_usage_from_json(unknown, &usage);
  clay_json_free(unknown);
  assert(!usage.cached_input_tokens_known);
}

static void test_openai_url_schemes(void) {
  assert(clay_openai_url_is_supported("https://api.example.test/v1"));
  assert(clay_openai_url_is_supported("http://127.0.0.1:8080/v1"));
  assert(!clay_openai_url_is_supported("ftp://api.example.test/v1"));
  assert(!clay_openai_url_is_supported("http://"));
}

int main(void) {
  test_pkce_and_authorization_url();
  test_callback_and_cors();
  test_refresh_rotation_and_routing();
  test_fragmented_openai_sse();
  test_usage_cache_details();
  test_openai_url_schemes();
  puts("grok tests passed");
  return 0;
}
