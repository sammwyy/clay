/* Standalone harness for providers/openai.c against a real endpoint.
   Not part of the `clay` binary; see the Makefile's test-openai target. */
#include "clay/http.h"
#include "clay/json.h"
#include "clay/providers/openai.h"
#include "clay/str.h"

#include <stdio.h>
#include <stdlib.h>

static ClayJson *tool_get_weather(const ClayJson *arguments, void *userdata) {
    (void)userdata;
    const char *location = clay_json_string_value(clay_json_object_get(arguments, "location"));
    fprintf(stderr, "\n[running get_weather(location=\"%s\")]\n", location);

    ClayJson *result = clay_json_object();
    clay_json_object_set(result, "location", clay_json_string(location));
    clay_json_object_set(result, "temperature_c", clay_json_number(22));
    clay_json_object_set(result, "condition", clay_json_string("sunny"));
    return result;
}

static void on_token(const char *text, void *userdata) {
    (void)userdata;
    fputs(text, stdout);
    fflush(stdout);
}

static void on_tool_call(const char *name, const char *arguments_json, void *userdata) {
    (void)userdata;
    fprintf(stderr, "\n[tool call] %s(%s)\n", name, arguments_json);
}

static void on_tool_result(const char *name, const ClayJson *result, void *userdata) {
    (void)userdata;
    ClayStr s;
    clay_str_init(&s);
    clay_json_stringify(result, &s);
    fprintf(stderr, "[tool result] %s -> %s\n", name, s.data);
    clay_str_free(&s);
}

static void on_error(long status, const char *body, void *userdata) {
    (void)userdata;
    fprintf(stderr, "\n[http error] status=%ld body=%s\n", status, body ? body : "");
}

int main(int argc, char **argv) {
    const char *base_url = getenv("OPENAI_BASE_URL");
    const char *api_key = getenv("OPENAI_API_KEY");
    const char *model = getenv("OPENAI_MODEL");

    if (!base_url || !api_key || !model) {
        fprintf(stderr,
                "usage: OPENAI_BASE_URL=... OPENAI_API_KEY=... OPENAI_MODEL=... %s [prompt]\n"
                "  OPENAI_BASE_URL: e.g. https://api.openai.com/v1 (no trailing slash)\n",
                argv[0]);
        return 1;
    }

    ClayStr prompt;
    clay_str_init(&prompt);
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            if (i > 1) clay_str_push_char(&prompt, ' ');
            clay_str_push(&prompt, argv[i]);
        }
    } else {
        clay_str_push(&prompt, "What's the weather like in Buenos Aires right now? Use the get_weather tool.");
    }

    clay_http_init();

    ClayOpenAI *client = clay_openai_create(base_url, api_key, model);

    ClayJson *messages = clay_json_array();
    clay_json_array_push(messages, clay_openai_message("user", prompt.data));
    clay_str_free(&prompt);

    ClayJson *location_prop = clay_json_object();
    clay_json_object_set(location_prop, "type", clay_json_string("string"));
    clay_json_object_set(location_prop, "description", clay_json_string("City and country, e.g. 'Buenos Aires, Argentina'"));

    ClayJson *props = clay_json_object();
    clay_json_object_set(props, "location", location_prop);

    ClayJson *required = clay_json_array();
    clay_json_array_push(required, clay_json_string("location"));

    ClayJson *params = clay_json_object();
    clay_json_object_set(params, "type", clay_json_string("object"));
    clay_json_object_set(params, "properties", props);
    clay_json_object_set(params, "required", required);

    ClayTool tools[] = {
        {"get_weather", "Gets the current weather for a location.", params, tool_get_weather, NULL},
    };

    ClayOpenAICallbacks callbacks = {0};
    callbacks.on_token = on_token;
    callbacks.on_tool_call = on_tool_call;
    callbacks.on_tool_result = on_tool_result;
    callbacks.on_error = on_error;

    int rc = clay_openai_run(client, messages, tools, 1, 8, &callbacks);
    fputc('\n', stdout);
    if (rc != 0) fprintf(stderr, "clay_openai_run failed\n");

    clay_json_free(params);
    clay_json_free(messages);
    clay_openai_destroy(client);
    clay_http_cleanup();

    return rc == 0 ? 0 : 1;
}
