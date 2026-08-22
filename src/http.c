#include "clay/http.h"

#include "clay/str.h"

#include <curl/curl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    ClayHttpResponse *response;
    ClayHttpChunkFn on_chunk;
    void *userdata;
    size_t received;
    size_t max_response_bytes;
} ClayHttpWriteCtx;

static int progress_cb(void *userdata, curl_off_t total_download, curl_off_t downloaded,
                       curl_off_t total_upload, curl_off_t uploaded) {
    (void)total_download;
    (void)downloaded;
    (void)total_upload;
    (void)uploaded;
    const ClayHttpRequest *req = userdata;
    return req->should_abort && req->should_abort(req->abort_userdata);
}

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    ClayHttpWriteCtx *ctx = userdata;
    size_t len = size * nmemb;

    if (size != 0 && len / size != nmemb) return 0;
    if (ctx->max_response_bytes > 0 &&
        (ctx->received > ctx->max_response_bytes || len > ctx->max_response_bytes - ctx->received)) {
        return 0;
    }
    ctx->received += len;

    if (ctx->on_chunk) {
        return ctx->on_chunk(ptr, len, ctx->userdata) ? 0 : len;
    }

    ClayHttpResponse *r = ctx->response;
    if (len > SIZE_MAX - r->body_len - 1) return 0;
    char *grown = realloc(r->body, r->body_len + len + 1);
    if (!grown) return 0;
    r->body = grown;
    memcpy(r->body + r->body_len, ptr, len);
    r->body_len += len;
    r->body[r->body_len] = '\0';
    return len;
}

int clay_http_init(void) {
    return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK ? 0 : -1;
}

void clay_http_cleanup(void) {
    curl_global_cleanup();
}

int clay_http_request(const ClayHttpRequest *req, ClayHttpResponse *response) {
    memset(response, 0, sizeof(*response));

    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    struct curl_slist *headers = NULL;
    for (size_t i = 0; i < req->header_count; i++) {
        ClayStr line;
        clay_str_init(&line);
        clay_str_printf(&line, "%s: %s", req->headers[i].name, req->headers[i].value);
        headers = curl_slist_append(headers, line.data);
        clay_str_free(&line);
    }

    ClayHttpWriteCtx ctx = {response, req->on_chunk, req->userdata, 0, req->max_response_bytes};

    curl_easy_setopt(curl, CURLOPT_URL, req->url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (req->should_abort) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, req);
    }
    if (req->timeout_seconds > 0) {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)req->timeout_seconds);
    }

    if (req->method && strcmp(req->method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req->body ? req->body : "");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)req->body_len);
    } else if (req->method && strcmp(req->method, "GET") != 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, req->method);
        if (req->body) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req->body);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)req->body_len);
        }
    }

    CURLcode rc = curl_easy_perform(curl);

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    response->status = status;

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return rc == CURLE_OK ? 0 : -1;
}

void clay_http_response_free(ClayHttpResponse *response) {
    free(response->body);
    response->body = NULL;
    response->body_len = 0;
}
