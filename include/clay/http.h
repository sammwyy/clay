#ifndef CLAY_HTTP_H
#define CLAY_HTTP_H

#include <stddef.h>

typedef struct {
    const char *name;
    const char *value;
} ClayHttpHeader;

/* Called per chunk of the response body, not aligned to any line/message
   boundary - callers needing whole lines buffer it themselves (see
   providers/openai.c). Return non-zero to abort the transfer. */
typedef int (*ClayHttpChunkFn)(const char *data, size_t len, void *userdata);
typedef int (*ClayHttpAbortFn)(void *userdata);

typedef struct {
    const char *method; /* "GET", "POST", ... */
    const char *url;
    const ClayHttpHeader *headers;
    size_t header_count;
    const char *body; /* request body, NULL for none */
    size_t body_len;
    ClayHttpChunkFn on_chunk; /* NULL to buffer the whole response into ClayHttpResponse instead */
    void *userdata;
    ClayHttpAbortFn should_abort;
    void *abort_userdata;
    int timeout_seconds; /* 0 = no timeout; streaming responses can run long */
    size_t max_response_bytes; /* 0 = unlimited */
} ClayHttpRequest;

typedef struct {
    long status;
    char *body;      /* malloc'd, NUL-terminated; only populated when on_chunk was NULL. Caller frees. */
    size_t body_len;
} ClayHttpResponse;

/* Call once at process startup/shutdown; not safe to call concurrently
   with a request. */
int clay_http_init(void);
void clay_http_cleanup(void);

/* Synchronous. Returns 0 once the exchange completed - check
   response->status for the HTTP outcome, a 4xx/5xx included - nonzero
   only on a transport failure. */
int clay_http_request(const ClayHttpRequest *req, ClayHttpResponse *response);

void clay_http_response_free(ClayHttpResponse *response);

#endif /* CLAY_HTTP_H */
