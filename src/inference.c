#include "neutrino.h"

#include <stdio.h>
#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <process.h>
#else
#include <curl/curl.h>
#include <pthread.h>
#endif

uint64_t monotonic_ms(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
#endif
}
void pause_ms(unsigned ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec t = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
#endif
}

struct Inference {
    char *url, *key, *body;
    int timeout_ms;
    Text out, reasoning, error;
    InferenceMetrics metrics;
    atomic_bool cancel;
    bool done, started, probe, from_gateway;
#ifdef _WIN32
    SRWLOCK lock;
    HANDLE thread;
#else
    pthread_mutex_t lock;
    pthread_t thread;
#endif
};

typedef struct {
    Inference *job;
    Text pending, batch;
    uint64_t start, first, last;
    bool sse, final, end_marker, malformed, saw_output;
} Response;

static void lock(Inference *j)
{
#ifdef _WIN32
    AcquireSRWLockExclusive(&j->lock);
#else
    pthread_mutex_lock(&j->lock);
#endif
}
static void unlock(Inference *j)
{
#ifdef _WIN32
    ReleaseSRWLockExclusive(&j->lock);
#else
    pthread_mutex_unlock(&j->lock);
#endif
}
static bool cancelled(Inference *j)
{
    return atomic_load_explicit(&j->cancel, memory_order_relaxed);
}

static double positive_number(const Json *obj, const char *key)
{
    const Json *value = at(obj, key);
    return cJSON_IsNumber(value) && isfinite(value->valuedouble) && value->valuedouble > 0
        ? value->valuedouble : 0;
}

static bool read_choice(Response *r, const char *packet)
{
    Json *json = cJSON_ParseWithOpts(packet, NULL, 1);
    if (!json) return false;
    Json *choice = cJSON_GetArrayItem(at(json, "choices"), 0);
    Json *message = at(choice, "delta");
    if (!message) message = at(choice, "message");
    const char *content = js(message, "content"), *thinking = js(message, "reasoning_content");
    if (!*thinking) thinking = js(message, "reasoning");
    Inference *j = r->job;
    if (*content || *thinking) {
        r->last = monotonic_ms();
        if (!r->saw_output) {
            r->first = r->last;
            r->saw_output = true;
        }
    }
    lock(j);
    const Json *metrics = at(json, "neutrino_metrics");
    if (j->from_gateway && cJSON_IsObject(metrics)) {
        double rate = positive_number(metrics, "tokens_per_second");
        double tokens = positive_number(metrics, "completion_tokens");
        const Json *ttft = at(metrics, "ttft_ms");
        j->metrics.tokens_per_second = rate;
        if (tokens <= INT_MAX) j->metrics.completion_tokens = (int)tokens;
        if (cJSON_IsNumber(ttft) && ttft->valuedouble >= 0 && ttft->valuedouble <= 3600000) {
            j->metrics.ttft_ms = (uint64_t)ttft->valuedouble;
            j->metrics.has_ttft = true;
        }
    } else if (!j->from_gateway) {
        const Json *timings = at(json, "timings");
        double tokens = positive_number(at(json, "usage"), "completion_tokens");
        if (!tokens) tokens = positive_number(timings, "predicted_n");
        if (tokens > 0 && tokens <= INT_MAX) j->metrics.completion_tokens = (int)tokens;
        double rate = positive_number(timings, "predicted_per_second");
        if (rate) j->metrics.tokens_per_second = rate;
    }
    if (at(json, "error")) append(&j->error, "Provider returned an inference error.");
    if (j->out.len + strlen(content) <= EDIT_LIMIT && j->reasoning.len + strlen(thinking) <= EDIT_LIMIT) {
        append(&j->out, content);
        append(&j->reasoning, thinking);
    } else if (!j->error.len) append(&j->error, "Response exceeded the preview limit.");
    unlock(j);
    if (*js(choice, "finish_reason")) r->final = true;
    cJSON_Delete(json);
    return true;
}

static bool receive(Response *r, const char *data, size_t n)
{
    if (cancelled(r->job) || r->pending.len + n > FILE_LIMIT) return false;
    if (r->job->probe) {
        if (r->pending.len + n > 16384 || memchr(data, 0, n)) return false;
        append_n(&r->pending, data, n);
        return true;
    }
    append_n(&r->pending, data, n);
    size_t offset = 0;
    while (offset < r->pending.len) {
        char *line = r->pending.data + offset;
        char *end = memchr(line, '\n', r->pending.len - offset);
        if (!end) break;
        offset = (size_t)(end - r->pending.data) + 1;
        size_t len = (size_t)(end - line);
        if (len && line[len - 1] == '\r') len--;
        if (len >= 5 && !memcmp(line, "data:", 5)) {
            r->sse = true;
            clear(&r->batch);
            const char *body = line + 5;
            len -= 5;
            while (len && *body == ' ') { body++; len--; }
            if (len == 6 && !memcmp(body, "[DONE]", 6)) r->final = r->end_marker = true;
            else if (len) {
                append_n(&r->batch, body, len);
                if (!read_choice(r, str(&r->batch))) r->malformed = true;
                clear(&r->batch);
            }
        } else if (!r->sse) {
            size_t line_size = (size_t)(end - line) + 1;
            if (r->batch.len + line_size > FILE_LIMIT) return false;
            append_n(&r->batch, line, line_size);
        }
    }
    if (offset) {
        r->pending.len -= offset;
        memmove(r->pending.data, r->pending.data + offset, r->pending.len);
        r->pending.data[r->pending.len] = 0;
    }
    return true;
}

#ifdef _WIN32
static wchar_t *wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    if (!n) return NULL;
    wchar_t *w = malloc((size_t)n * sizeof(*w));
    if (!w) abort();
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n);
    return w;
}
static wchar_t *wide_slice(const wchar_t *s, DWORD n)
{
    wchar_t *w = calloc((size_t)n + 1, sizeof(*w));
    if (!w) abort();
    if (n) memcpy(w, s, (size_t)n * sizeof(*w));
    return w;
}
static bool transfer(Inference *j, Response *r, long *status)
{
    wchar_t *url = wide(j->url);
    URL_COMPONENTS parts = {0};
    parts.dwStructSize = sizeof parts;
    parts.dwSchemeLength = parts.dwHostNameLength = parts.dwUrlPathLength =
        parts.dwUserNameLength = parts.dwPasswordLength = parts.dwExtraInfoLength = (DWORD)-1;
    if (!url || !WinHttpCrackUrl(url, 0, 0, &parts) || parts.dwUserNameLength ||
        parts.dwPasswordLength || parts.dwExtraInfoLength ||
        (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS)) {
        free(url);
        return false;
    }
    wchar_t *host = wide_slice(parts.lpszHostName, parts.dwHostNameLength);
    wchar_t *path = wide_slice(parts.lpszUrlPath, parts.dwUrlPathLength);
    HINTERNET session = WinHttpOpen(L"Neutrino/" NEUTRINO_VERSION, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session) {
        int setup = j->timeout_ms > 0 && j->timeout_ms < 10000 ? j->timeout_ms : 10000;
        int read = j->timeout_ms > 0 ? j->timeout_ms : 30000;
        WinHttpSetTimeouts(session, setup, setup, setup, read);
    }
    HINTERNET conn = session ? WinHttpConnect(session, host, parts.nPort, 0) : NULL;
    HINTERNET req = conn ? WinHttpOpenRequest(conn, j->probe ? L"GET" : L"POST", path, NULL, WINHTTP_NO_REFERER,
                        WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS
                        ? WINHTTP_FLAG_SECURE : 0) : NULL;
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    Text headers = {0};
    append(&headers, "Content-Type: application/json\r\nAccept: text/event-stream, application/json\r\n");
    if (*j->key) format(&headers, "Authorization: Bearer %s\r\n", j->key);
    wchar_t *fields = wide(str(&headers));
    DWORD length = j->body ? (DWORD)strlen(j->body) : 0, code = 0, size = sizeof code;
    bool ok = req && fields && !cancelled(j) &&
        WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof redirect) &&
        WinHttpSendRequest(req, fields, (DWORD)-1, j->body, length, length, 0) &&
        WinHttpReceiveResponse(req, NULL) &&
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
    *status = (long)code;
    if (ok && code >= 200 && code < 300) {
        char buffer[8192];
        DWORD n = 0;
        do {
            ok = !cancelled(j) && WinHttpReadData(req, buffer, sizeof buffer, &n);
            if (ok && n) ok = receive(r, buffer, n);
        } while (ok && n && !r->end_marker);
    }
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    if (session) WinHttpCloseHandle(session);
    release(&headers);
    free(fields);
    free(host);
    free(path);
    free(url);
    return ok;
}
#else
static size_t curl_receive(char *buf, size_t size, size_t n, void *user)
{
    Response *r = user;
    return receive(r, buf, size * n) ? size * n : 0;
}
static int curl_progress(void *user, curl_off_t dlt, curl_off_t dl, curl_off_t ult, curl_off_t ul)
{
    (void)dlt; (void)dl; (void)ult; (void)ul;
    return cancelled(user) ? 1 : 0;
}
static bool transfer(Inference *j, Response *r, long *status)
{
    CURL *curl = curl_easy_init();
    if (!curl) return false;
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: text/event-stream, application/json");
    headers = curl_slist_append(headers, "Expect:");
    Text auth = {0};
    if (*j->key) {
        format(&auth, "Authorization: Bearer %s", j->key);
        headers = curl_slist_append(headers, str(&auth));
    }
    curl_easy_setopt(curl, CURLOPT_URL, j->url);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
    curl_easy_setopt(curl, CURLOPT_DISALLOW_USERNAME_IN_URL, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_NONE);
    curl_easy_setopt(curl, CURLOPT_PROXYAUTH, (long)CURLAUTH_NONE);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, j->probe ? 2500L : 10000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, j->timeout_ms > 0 ? (long)j->timeout_ms : 300000L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    if (!j->probe) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, j->body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_receive);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, r);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, j);
    CURLcode code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    release(&auth);
    return code == CURLE_OK;
}
#endif

#ifdef _WIN32
static unsigned __stdcall worker(void *arg)
#else
static void *worker(void *arg)
#endif
{
    Inference *j = arg;
    Response r = {.job = j, .start = monotonic_ms()};
    long status = 0;
    bool ok = transfer(j, &r, &status);
    if (!j->probe && ok && status >= 200 && status < 300 && !r.sse) {
        if (!strncmp(str(&r.pending), "data:", 5)) receive(&r, "\n", 1);
        else {
            append(&r.batch, str(&r.pending));
            r.final = read_choice(&r, str(&r.batch));
        }
    } else if (ok && r.sse && r.pending.len) receive(&r, "\n", 1);
    lock(j);
    if (cancelled(j)) {
        clear(&j->error);
        append(&j->error, "Cancelled. Any partial output is available for review.");
    } else if (!j->error.len) {
        if (status && (status < 200 || status >= 300))
            format(&j->error, "Provider returned HTTP %ld.", status);
        else if (!ok) append(&j->error, "Connection failed or timed out. Check the endpoint.");
        else if (j->probe) append_n(&j->out, str(&r.pending), r.pending.len);
        else if (!r.final || r.malformed) append(&j->error, "The provider returned an incomplete or invalid stream.");
        else if (!j->out.len) append(&j->error, "The model returned no prose.");
    }
    if (!j->probe && !j->from_gateway && r.saw_output && !j->error.len) {
        j->metrics.has_ttft = true;
        j->metrics.ttft_ms = r.first - r.start;
        if (!j->metrics.tokens_per_second && j->metrics.completion_tokens > 1 && r.last > r.first)
            j->metrics.tokens_per_second = (j->metrics.completion_tokens - 1) * 1000.0 / (double)(r.last - r.first);
    }
    j->done = true;
    unlock(j);
    release(&r.pending);
    release(&r.batch);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static char *request_body(const char *model, const Json *messages, RequestOptions options)
{
    Json *body = cJSON_CreateObject();
    set(body, "model", model);
    put(body, "messages", cJSON_Duplicate(messages, true));
    put(body, "stream", cJSON_CreateTrue());
    if (options.include_usage) {
        Json *stream_options = cJSON_CreateObject();
        put(stream_options, "include_usage", cJSON_CreateTrue());
        put(body, "stream_options", stream_options);
    }
    if (options.slot >= 0) {
        integer(body, "id_slot", options.slot);
        put(body, "cache_prompt", cJSON_CreateTrue());
    }
    if (options.max_tokens > 0) integer(body, "max_tokens", options.max_tokens);
    if (options.json_schema) {
        Json *format = cJSON_CreateObject();
        Json *wrapper = cJSON_CreateObject();
        set(format, "type", "json_schema");
        set(wrapper, "name", "compendium_filing");
        put(wrapper, "strict", cJSON_CreateTrue());
        put(wrapper, "schema", cJSON_ParseWithOpts(options.json_schema, NULL, 1));
        put(format, "json_schema", wrapper);
        put(body, "response_format", format);
    }
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json) abort();
    return json;
}

static Inference *start_request(const char *endpoint, const char *key, const char *model,
                                const Json *messages, RequestOptions options, bool probe, bool from_gateway)
{
    if ((strncmp(endpoint, "http://", 7) && strncmp(endpoint, "https://", 8)) ||
        strpbrk(endpoint, "\r\n?#") || strpbrk(key, "\r\n") || (!probe && !*model)) return NULL;
#ifndef _WIN32
    static bool curl_ready;
    if (!curl_ready) curl_ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    if (!curl_ready) return NULL;
#endif
    Inference *j = calloc(1, sizeof(*j));
    if (!j) abort();
    atomic_init(&j->cancel, false);
    j->probe = probe;
    j->from_gateway = from_gateway;
    Text url = {0};
    size_t len = strlen(endpoint);
    while (len && endpoint[len - 1] == '/') len--;
    const char suffix[] = "/chat/completions";
    const size_t suffix_len = sizeof suffix - 1;
    const bool has_completion_path = len >= suffix_len && !memcmp(endpoint + len - suffix_len, suffix, suffix_len);
    if (probe && has_completion_path) len -= suffix_len;
    append_n(&url, endpoint, len);
    if (probe) append(&url, "/status");
    else if (!has_completion_path) append(&url, suffix);
    j->url = url.data;
    j->key = copy(key);
    j->timeout_ms = options.timeout_ms;
    if (!probe) j->body = request_body(model, messages, options);
#ifdef _WIN32
    InitializeSRWLock(&j->lock);
    j->thread = (HANDLE)_beginthreadex(NULL, 0, worker, j, 0, NULL);
    j->started = j->thread != NULL;
#else
    if (pthread_mutex_init(&j->lock, NULL)) abort();
    j->started = pthread_create(&j->thread, NULL, worker, j) == 0;
#endif
    if (j->started) return j;
    inference_free(j);
    return NULL;
}
Inference *inference_request(const char *endpoint, const char *key, const char *model,
                             const Json *messages, RequestOptions options)
{
    return start_request(endpoint, key, model, messages, options, false, false);
}
Inference *inference_start(const char *endpoint, const char *key, const char *model, const Json *messages)
{
    return start_request(endpoint, key, model, messages, (RequestOptions){.slot = -1}, false, true);
}
Inference *inference_probe(const char *endpoint, const char *key)
{
    return start_request(endpoint, key, "", nullptr, (RequestOptions){.timeout_ms = 2500}, true, false);
}
InferenceMetrics inference_metrics(Inference *j)
{
    lock(j);
    InferenceMetrics metrics = j->metrics;
    unlock(j);
    return metrics;
}
static void mirror(Text *dst, const Text *src)
{
    if (dst->len == src->len) return;
    clear(dst);
    append_n(dst, str(src), src->len);
}
bool inference_read(Inference *j, Text *out, Text *thinking, Text *error)
{
    lock(j);
    mirror(out, &j->out);
    mirror(thinking, &j->reasoning);
    mirror(error, &j->error);
    bool done = j->done;
    unlock(j);
    return done;
}
void inference_cancel(Inference *j)
{
    atomic_store_explicit(&j->cancel, true, memory_order_relaxed);
}
void inference_free(Inference *j)
{
#ifdef _WIN32
    if (j->started) {
        WaitForSingleObject(j->thread, INFINITE);
        CloseHandle(j->thread);
    }
#else
    if (j->started) pthread_join(j->thread, NULL);
    pthread_mutex_destroy(&j->lock);
#endif
    free(j->url);
    free(j->key);
    cJSON_free(j->body);
    release(&j->out);
    release(&j->reasoning);
    release(&j->error);
    free(j);
}
