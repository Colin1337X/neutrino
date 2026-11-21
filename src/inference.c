#include "neutrino.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <process.h>
#else
#include <curl/curl.h>
#include <pthread.h>
#endif

struct Inference {
    char *url, *key, *body;
    Text out, reasoning, error;
    bool cancel, done, started;
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
    bool sse, final, malformed;
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
    lock(j);
    bool result = j->cancel;
    unlock(j);
    return result;
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
    lock(j);
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
            if (len == 6 && !memcmp(body, "[DONE]", 6)) r->final = true;
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
    HINTERNET session = WinHttpOpen(L"Neutrino/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session) WinHttpSetTimeouts(session, 10000, 10000, 10000, 30000);
    HINTERNET conn = session ? WinHttpConnect(session, host, parts.nPort, 0) : NULL;
    HINTERNET req = conn ? WinHttpOpenRequest(conn, L"POST", path, NULL, WINHTTP_NO_REFERER,
                        WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS
                        ? WINHTTP_FLAG_SECURE : 0) : NULL;
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    Text headers = {0};
    append(&headers, "Content-Type: application/json\r\nAccept: text/event-stream, application/json\r\n");
    if (*j->key) format(&headers, "Authorization: Bearer %s\r\n", j->key);
    wchar_t *fields = wide(str(&headers));
    DWORD length = (DWORD)strlen(j->body), code = 0, size = sizeof code;
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
        } while (ok && n && !r->final);
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
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 300000L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, j->body);
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
    Response r = {.job = j};
    long status = 0;
    bool ok = transfer(j, &r, &status);
    if (ok && status >= 200 && status < 300 && !r.sse) {
        if (!strncmp(str(&r.pending), "data:", 5)) receive(&r, "\n", 1);
        else {
            append(&r.batch, str(&r.pending));
            r.final = read_choice(&r, str(&r.batch));
        }
    } else if (ok && r.sse && r.pending.len) receive(&r, "\n", 1);
    lock(j);
    if (j->cancel) {
        clear(&j->error);
        append(&j->error, "Cancelled. Any partial output is available for review.");
    } else if (!j->error.len) {
        if (status && (status < 200 || status >= 300))
            format(&j->error, "Provider returned HTTP %ld.", status);
        else if (!ok) append(&j->error, "Connection failed or timed out. Check the endpoint.");
        else if (!r.final || r.malformed) append(&j->error, "The provider returned an incomplete or invalid stream.");
        else if (!j->out.len) append(&j->error, "The model returned no prose.");
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

Inference *inference_start(const char *endpoint, const char *key, const char *model, const Json *messages)
{
    if ((strncmp(endpoint, "http://", 7) && strncmp(endpoint, "https://", 8)) ||
        strpbrk(endpoint, "\r\n?#") || strpbrk(key, "\r\n") || !*model) return NULL;
#ifndef _WIN32
    static bool curl_ready;
    if (!curl_ready) curl_ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    if (!curl_ready) return NULL;
#endif
    Inference *j = calloc(1, sizeof(*j));
    if (!j) abort();
    Text url = {0};
    size_t len = strlen(endpoint);
    while (len && endpoint[len - 1] == '/') len--;
    append_n(&url, endpoint, len);
    const char suffix[] = "/chat/completions";
    if (len < sizeof suffix - 1 || strcmp(str(&url) + len - (sizeof suffix - 1), suffix))
        append(&url, suffix);
    j->url = url.data;
    j->key = copy(key);
    Json *body = cJSON_CreateObject();
    set(body, "model", model);
    put(body, "messages", cJSON_Duplicate(messages, true));
    put(body, "stream", cJSON_CreateTrue());
    j->body = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!j->body) abort();
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
    lock(j);
    j->cancel = true;
    unlock(j);
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
