#include "neutrino.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET Socket;
typedef int Count;
#define BAD_SOCKET INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int Socket;
typedef ssize_t Count;
#define BAD_SOCKET (-1)
#endif

enum { CLIENTS = 16, HEADER_LIMIT = 16384, BODY_LIMIT = 16 * 1024 * 1024 };
static const char filing_schema[] =
    "{\"type\":\"array\",\"maxItems\":6,\"items\":{\"type\":\"object\","
    "\"properties\":{\"type\":{\"enum\":[\"character\",\"place\",\"item\",\"event\",\"note\",\"faction\","
    "\"rule\",\"relationship\",\"plot\",\"quest\",\"creature\"]},"
    "\"name\":{\"type\":\"string\"},"
    "\"description\":{\"type\":\"string\"}},"
    "\"required\":[\"type\",\"name\",\"description\"],\"additionalProperties\":false}}";
static const char acoustic_selector_schema[] =
    "{\"type\":\"object\",\"properties\":{\"neutrino_tool\":{\"enum\":[\"acoustic\"]}},"
    "\"required\":[\"neutrino_tool\"],\"additionalProperties\":false}";
static const char research_plan_schema[] =
    "{\"type\":\"object\",\"properties\":{\"kind\":{\"enum\":[\"none\",\"news\",\"reference\",\"dictionary\"]},"
    "\"query\":{\"type\":\"string\"}},\"required\":[\"kind\",\"query\"],\"additionalProperties\":false}";
typedef struct {
    const char *endpoint, *key, *model, *web_mode;
    RequestOptions options;
    int lane;
} Route;
typedef struct {
    Socket socket;
    uint64_t touched;
    Text request, send, output, thinking, error;
    size_t sent;
    Inference *job;
    Json *messages;
    Route route;
    bool closing;
} Client;
typedef struct {
    Client clients[CLIENTS];
    int workers;
    uint64_t last_provider_result;
    bool provider_ok;
} Gateway;

static void close_socket(Socket s)
{
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}
static bool nonblocking(Socket s)
{
#ifdef _WIN32
    u_long value = 1;
    return ioctlsocket(s, FIONBIO, &value) == 0;
#else
    return fcntl(s, F_SETFL, O_NONBLOCK) == 0;
#endif
}
static bool would_block(void)
{
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}
static const char *server_model(void) { return env("NEUTRINO_MODEL", ""); }

static bool route_for(const char *name, Route *route)
{
    const char *base = env("NEUTRINO_ENDPOINT", "http://127.0.0.1:11434/v1");
    const char *key = env("NEUTRINO_API_KEY", "");
    const bool slots = !strcmp(env("NEUTRINO_LLAMA_SLOTS", ""), "1");
    *route = (Route){.endpoint = base, .key = key, .model = server_model(),
        .options = {.slot = slots ? 0 : -1,
                    .include_usage = !strcmp(env("NEUTRINO_STREAM_USAGE", ""), "1")}, .lane = 0};
    if (!strcmp(name, "web-search") || !strcmp(name, "web-read")) {
        route->lane = 3;
        route->web_mode = !strcmp(name, "web-search") ? "web-search" : "web-read";
        return true;
    }
    if (!strcmp(name, "narrator") ||
        (!strncmp(name, "narrator/", 9) && !strcmp(name + 9, route->model)))
        return *route->model != 0;
    if (!strcmp(name, "narrator-acoustic") ||
        (!strncmp(name, "narrator-acoustic/", 18) && !strcmp(name + 18, route->model))) {
        if (slots) {
            route->options.json_schema = acoustic_selector_schema;
            route->options.max_tokens = 48;
        }
        return *route->model != 0;
    }
    if (!strcmp(name, "bench-narrator")) {
        route->options.max_tokens = 1;
        return *route->model != 0;
    }
    route->lane = slots ? 1 : 0;
    route->options.slot = slots ? 1 : -1;
    route->model = env("NEUTRINO_AGENT_MODEL", route->model);
    if (!strcmp(name, "research-plan")) {
        if (slots) route->options.json_schema = research_plan_schema;
        route->options.max_tokens = 72;
        route->options.timeout_ms = 18000;
        return *route->model != 0;
    }
    if (!strcmp(name, "file") || !strcmp(name, "history") || !strcmp(name, "bench-agent")) {
        route->options.max_tokens = !strcmp(name, "bench-agent") ? 1 : 1200;
        if (slots && !strcmp(name, "file")) route->options.json_schema = filing_schema;
        return *route->model != 0;
    }
    if (strcmp(name, "recall") && strncmp(name, "recall/", 7)) return false;
    const char *recall_endpoint = env("NEUTRINO_RECALL_ENDPOINT", "");
    if (*recall_endpoint && strcmp(recall_endpoint, base)) {
        route->endpoint = recall_endpoint;
        route->key = env("NEUTRINO_RECALL_API_KEY", "");
        route->options.slot = -1;
        route->options.include_usage = false;
        route->lane = 2;
    }
    const char *recall_model = env("NEUTRINO_RECALL_MODEL", route->model);
    if (name[6] == '/') {
        const char *binding = name + 7;
        if (!strncmp(binding, "default:", 8) && !strcmp(binding + 8, server_model())) {
            route->endpoint = base;
            route->key = key;
            route->lane = slots ? 1 : 0;
            route->options.slot = slots ? 1 : -1;
            route->options.include_usage = !strcmp(env("NEUTRINO_STREAM_USAGE", ""), "1");
            recall_model = server_model();
        } else if (strncmp(binding, "recall:", 7) || strcmp(binding + 7, recall_model)) {
            return false;
        }
    }
    route->model = recall_model;
    route->options.max_tokens = 180;
    route->options.timeout_ms = 8000;
    return *route->model != 0;
}

static void discard_socket(Client *c)
{
    if (c->socket != BAD_SOCKET) close_socket(c->socket);
    c->socket = BAD_SOCKET;
    if (c->job) inference_cancel(c->job);
    cJSON_Delete(c->messages);
    c->messages = NULL;
    release(&c->request);
    release(&c->send);
}

static void reset_client(Client *c)
{
    discard_socket(c);
    if (c->job) inference_free(c->job);
    release(&c->output);
    release(&c->thinking);
    release(&c->error);
    *c = (Client){.socket = BAD_SOCKET};
}

static void http_error(Client *c, int status, const char *reason)
{
    format(&c->send, "HTTP/1.1 %d %s\r\nConnection: close\r\nContent-Length: 0\r\n\r\n", status, reason);
    c->closing = true;
}

static bool field_is(const char *name, size_t length, const char *expected)
{
    if (length != strlen(expected)) return false;
    for (size_t i = 0; i < length; i++)
        if (tolower((unsigned char)name[i]) != tolower((unsigned char)expected[i])) return false;
    return true;
}

static bool bearer_matches(const char *header, size_t n, const char *token)
{
    size_t size = strlen(token);
    if (n != size + 7 || memcmp(header, "Bearer ", 7)) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < size; i++) diff |= (unsigned char)(header[i + 7] ^ token[i]);
    return diff == 0;
}

static bool local_host(const char *host, const char *end)
{
    const char *colon = memchr(host, ':', (size_t)(end - host));
    const char *base_end = colon ? colon : end;
    if (!field_is(host, (size_t)(base_end - host), "127.0.0.1") &&
        !field_is(host, (size_t)(base_end - host), "localhost")) return false;
    if (colon && colon + 1 == end) return false;
    if (colon) for (const char *p = colon + 1; p < end; p++)
        if (!isdigit((unsigned char)*p)) return false;
    return true;
}

static void respond_status(Client *c, const Gateway *gateway)
{
    int active = 0, queued = 0;
    for (int i = 0; i < CLIENTS; i++) {
        if (gateway->clients[i].job) active++;
        if (gateway->clients[i].messages) queued++;
    }
    bool pinned = !strcmp(env("NEUTRINO_LLAMA_SLOTS", ""), "1");
    Json *status = cJSON_CreateObject();
    set(status, "service", "neutrino");
    set(status, "version", NEUTRINO_VERSION);
    set(status, "model", server_model());
    set(status, "provider", !gateway->last_provider_result ? "untested" : gateway->provider_ok ? "ok" : "failed");
    if (gateway->last_provider_result)
        put(status, "provider_age_s", cJSON_CreateNumber((double)((monotonic_ms() - gateway->last_provider_result) / 1000)));
    integer(status, "active", active);
    integer(status, "queued", queued);
    integer(status, "workers", pinned ? 1 : gateway->workers);
    put(status, "pinned_slots", cJSON_CreateBool(pinned));
    char *body = cJSON_PrintUnformatted(status);
    if (!body) abort();
    format(&c->send, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
           "Cache-Control: no-store\r\nConnection: close\r\nContent-Length: %zu\r\n\r\n%s", strlen(body), body);
    c->closing = true;
    cJSON_free(body);
    cJSON_Delete(status);
}

static void parse_request(Client *c, const char *token, const Gateway *gateway)
{
    const char *s = str(&c->request), *headers_end = strstr(s, "\r\n\r\n");
    if (!headers_end) {
        if (c->request.len > HEADER_LIMIT) http_error(c, 431, "Headers Too Large");
        return;
    }
    size_t offset = (size_t)(headers_end - s) + 4;
    if (offset > HEADER_LIMIT) { http_error(c, 431, "Headers Too Large"); return; }
    const char *line_end = strstr(s, "\r\n");
    const char path[] = "POST /v1/chat/completions HTTP/1.1";
    const char status_path[] = "GET /v1/status HTTP/1.1";
    bool status_request = (size_t)(line_end - s) == sizeof status_path - 1 &&
                          !memcmp(s, status_path, sizeof status_path - 1);
    if (!status_request && ((size_t)(line_end - s) != sizeof path - 1 || memcmp(s, path, sizeof path - 1))) {
        http_error(c, 404, "Not Found");
        return;
    }
    bool has_length = false, has_host = false, authorized = !*token, auth_seen = false;
    size_t length = 0;
    for (const char *line = line_end + 2; line < headers_end;) {
        const char *end = strstr(line, "\r\n"), *sep = memchr(line, ':', (size_t)(end - line));
        if (!sep) { http_error(c, 400, "Bad Request"); return; }
        const char *value = sep + 1;
        while (value < end && (*value == ' ' || *value == '\t')) value++;
        size_t name_len = (size_t)(sep - line);
        if (field_is(line, name_len, "Origin") || field_is(line, name_len, "Transfer-Encoding") ||
            field_is(line, name_len, "Expect")) {
            http_error(c, 400, "Bad Request"); return;
        }
        if (field_is(line, name_len, "Authorization")) {
            if (auth_seen) { http_error(c, 400, "Bad Request"); return; }
            auth_seen = true;
            authorized = !*token || bearer_matches(value, (size_t)(end - value), token);
        } else if (field_is(line, name_len, "Host")) {
            if (has_host || !local_host(value, end)) { http_error(c, 400, "Invalid Host"); return; }
            has_host = true;
        } else if (field_is(line, name_len, "Content-Length")) {
            if (has_length || value == end) { http_error(c, 400, "Bad Request"); return; }
            has_length = true;
            for (const char *digit = value; digit < end; digit++) {
                if (*digit < '0' || *digit > '9') { http_error(c, 400, "Bad Request"); return; }
                length = length * 10 + (size_t)(*digit - '0');
                if (length > BODY_LIMIT) { http_error(c, 413, "Body Too Large"); return; }
            }
        }
        line = end + 2;
    }
    if (!has_host) { http_error(c, 400, "Host Required"); return; }
    if (!authorized) { http_error(c, 401, "Unauthorized"); return; }
    if (status_request) {
        if (length || c->request.len != offset) http_error(c, 400, "Bad Request");
        else respond_status(c, gateway);
        return;
    }
    if (!has_length) { http_error(c, 411, "Length Required"); return; }
    if (c->request.len < offset + length) return;
    if (c->request.len != offset + length || memchr(s + offset, 0, length)) {
        http_error(c, 400, "Bad Request"); return;
    }
    Json *body = cJSON_ParseWithOpts(s + offset, NULL, 1);
    Json *messages = at(body, "messages");
    if (!cJSON_IsArray(messages) || !messages->child || cJSON_GetArraySize(messages) > 512 ||
        !route_for(js(body, "model"), &c->route)) {
        http_error(c, 400, "Invalid Messages Or Unconfigured Model");
    } else {
        Json *message;
        bool valid = true;
        cJSON_ArrayForEach(message, messages) {
            const char *role = js(message, "role");
            if ((!strcmp(role, "system") || !strcmp(role, "user") || !strcmp(role, "assistant")) &&
                cJSON_IsString(at(message, "content"))) continue;
            valid = false;
        }
        if (valid) c->messages = cJSON_DetachItemViaPointer(body, messages);
        else http_error(c, 400, "Invalid Message");
    }
    cJSON_Delete(body);
    release(&c->request);
}

static void event(Text *out, const char *field, const char *value)
{
    Json *body = cJSON_CreateObject();
    if (!strcmp(field, "error")) {
        put(body, "error", cJSON_CreateObject());
    } else {
        Json *choices = cJSON_CreateArray(), *choice = cJSON_CreateObject(), *delta = cJSON_CreateObject();
        set(delta, field, value);
        put(choice, "delta", delta);
        cJSON_AddItemToArray(choices, choice);
        put(body, "choices", choices);
    }
    char *json = cJSON_PrintUnformatted(body);
    if (!json) abort();
    format(out, "data: %s\n\n", json);
    cJSON_free(json);
    cJSON_Delete(body);
}

static void event_metrics(Text *out, InferenceMetrics metrics)
{
    Json *body = cJSON_CreateObject(), *data = cJSON_CreateObject();
    if (metrics.tokens_per_second > 0)
        put(data, "tokens_per_second", cJSON_CreateNumber(metrics.tokens_per_second));
    if (metrics.completion_tokens > 0) integer(data, "completion_tokens", metrics.completion_tokens);
    if (metrics.has_ttft) put(data, "ttft_ms", cJSON_CreateNumber((double)metrics.ttft_ms));
    put(body, "neutrino_metrics", data);
    char *json = cJSON_PrintUnformatted(body);
    if (!json) abort();
    format(out, "data: %s\n\n", json);
    cJSON_free(json);
    cJSON_Delete(body);
}

static void pump_client(Client *c, const char *token, Gateway *gateway)
{
    if (c->job) {
        size_t out = c->output.len, thought = c->thinking.len;
        bool done = inference_read(c->job, &c->output, &c->thinking, &c->error);
        if (c->socket != BAD_SOCKET) {
            if (out < c->output.len) event(&c->send, "content", c->output.data + out);
            if (thought < c->thinking.len) event(&c->send, "reasoning_content", c->thinking.data + thought);
            if (done) {
                if (c->route.lane == 0) {
                    gateway->last_provider_result = monotonic_ms();
                    gateway->provider_ok = !c->error.len;
                }
                if (c->error.len) event(&c->send, "error", "");
                else if (!c->route.web_mode) event_metrics(&c->send, inference_metrics(c->job));
                append(&c->send, "data: [DONE]\n\n");
                c->closing = true;
            }
        }
        if (done) {
            inference_free(c->job);
            c->job = NULL;
        }
    }
    if (c->socket == BAD_SOCKET) {
        if (!c->job) reset_client(c);
        return;
    }
    if (!c->closing) {
        char buffer[8192];
        Count n = recv(c->socket, buffer, sizeof buffer, 0);
        if (n > 0 && !c->job && !c->messages) {
            c->touched = monotonic_ms();
            if (c->request.len + (size_t)n > BODY_LIMIT + HEADER_LIMIT ||
                memchr(buffer, 0, (size_t)n)) http_error(c, 400, "Bad Request");
            else {
                append_n(&c->request, buffer, (size_t)n);
                parse_request(c, token, gateway);
            }
        } else if (n == 0 || n > 0 || (n < 0 && !would_block())) {
            discard_socket(c);
            return;
        }
    }
    if (c->send.len > c->sent) {
        size_t left = c->send.len - c->sent;
        Count n = send(c->socket, c->send.data + c->sent, left > 16384 ? 16384 : (int)left, 0);
        if (n > 0) {
            c->sent += (size_t)n;
            c->touched = monotonic_ms();
        } else if (n < 0 && !would_block()) { discard_socket(c); return; }
    }
    if (c->sent == c->send.len) {
        if (c->closing) { reset_client(c); return; }
        clear(&c->send);
        c->sent = 0;
    }
    if ((!c->job || c->sent < c->send.len) &&
        monotonic_ms() - c->touched > (c->messages ? 300000u : 30000u)) discard_socket(c);
}

static void dispatch(Client clients[CLIENTS], int workers)
{
    int active[4] = {0};
    for (int i = 0; i < CLIENTS; i++)
        if (clients[i].job) active[clients[i].route.lane]++;
    /* Players go ahead of agents waiting for a shared provider slot. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < CLIENTS; i++) {
            Client *c = &clients[i];
            Route *r = &c->route;
            int limit = r->options.slot >= 0 || r->lane != 0 ? 1 : workers;
            if (!c->messages || active[r->lane] >= limit ||
                ((pass == 0) != (c->route.options.max_tokens == 0 || c->route.options.max_tokens == 1))) continue;
            c->job = r->web_mode ? inference_web(r->web_mode, c->messages) :
                inference_request(r->endpoint, r->key, r->model, c->messages, r->options);
            cJSON_Delete(c->messages);
            c->messages = NULL;
            c->touched = monotonic_ms();
            if (c->job) {
                active[r->lane]++;
                append(&c->send, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                                  "Cache-Control: no-store\r\nConnection: close\r\n\r\n");
            } else http_error(c, 502, "Upstream Unavailable");
        }
    }
}

int inference_server(int port, const volatile sig_atomic_t *stop)
{
    if (!*server_model()) {
        fputs("Set NEUTRINO_MODEL in the server environment before starting Neutrino.\n", stderr);
        return 2;
    }
    const char *configured_workers = env("NEUTRINO_WORKERS", "2");
    if (strlen(configured_workers) != 1 || configured_workers[0] < '1' || configured_workers[0] > '4') {
        fputs("NEUTRINO_WORKERS must be a number from 1 to 4.\n", stderr);
        return 2;
    }
    const int workers = configured_workers[0] - '0';
#ifdef _WIN32
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data)) return 1;
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    const char *token = env("NEUTRINO_SERVER_TOKEN", "");
    Socket listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((unsigned short)port);
#ifndef _WIN32
    int reuse = 1;
    if (listener != BAD_SOCKET) setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
#endif
    int code = 0;
    if (listener == BAD_SOCKET || !nonblocking(listener) ||
        bind(listener, (struct sockaddr *)&address, sizeof address) || listen(listener, CLIENTS)) {
        fprintf(stderr, "Could not listen on 127.0.0.1:%d.\n", port);
        code = 1;
    } else {
        printf("Neutrino server listening on http://127.0.0.1:%d/v1\n", port);
        fflush(stdout);
        Gateway gateway = {.workers = workers};
        Client *clients = gateway.clients;
        for (int i = 0; i < CLIENTS; i++) clients[i].socket = BAD_SOCKET;
        while (!*stop) {
            Socket incoming = accept(listener, NULL, NULL);
            if (incoming != BAD_SOCKET) {
                int slot = 0;
                while (slot < CLIENTS && (clients[slot].socket != BAD_SOCKET || clients[slot].job)) slot++;
                if (slot == CLIENTS || !nonblocking(incoming)) close_socket(incoming);
                else {
                    clients[slot].socket = incoming;
                    clients[slot].touched = monotonic_ms();
                }
            }
            for (int i = 0; i < CLIENTS; i++) pump_client(&clients[i], token, &gateway);
            dispatch(clients, workers);
            pause_ms(12);
        }
        for (int i = 0; i < CLIENTS; i++) discard_socket(&clients[i]);
        for (int i = 0; i < CLIENTS; i++) reset_client(&clients[i]);
    }
    if (listener != BAD_SOCKET) close_socket(listener);
#ifdef _WIN32
    WSACleanup();
#endif
    return code;
}

static Json *benchmark_prompt(unsigned long long key, bool agent)
{
    Json *messages = cJSON_CreateArray(), *m = cJSON_CreateObject();
    Text content = {0};
    format(&content, "Private synthetic benchmark %llu. ", key);
    for (int i = 0; i < (agent ? 125 : 375); i++)
        format(&content, "Record %d: %s.\n", i, agent ? "Slate meets silver on a steep hill"
                                                       : "A lantern marks the entrance to an empty station");
    append(&content, "Reply with the word ready.");
    set(m, "role", "user");
    set(m, "content", str(&content));
    cJSON_AddItemToArray(messages, m);
    release(&content);
    return messages;
}

static int sample(const char *server, const char *token, const char *role, const Json *messages,
                  const volatile sig_atomic_t *stop)
{
    uint64_t start = monotonic_ms();
    Inference *job = inference_start(server, token, role, messages);
    if (!job) return -1;
    Text out = {0}, thought = {0}, error = {0};
    int first = -1;
    while (true) {
        bool done = inference_read(job, &out, &thought, &error);
        if (first < 0 && (out.len || thought.len)) first = (int)(monotonic_ms() - start);
        if (done) break;
        if (*stop) inference_cancel(job);
        pause_ms(2);
    }
    inference_free(job);
    if (error.len) fprintf(stderr, "%s\n", str(&error));
    int result = error.len ? -1 : first;
    release(&out);
    release(&thought);
    release(&error);
    return result;
}

int bench_ttft(const char *server, const char *token, int trials, const volatile sig_atomic_t *stop)
{
    int cold_sum = 0, warm_sum = 0, after_sum = 0;
    for (int i = 0; i < trials && !*stop; i++) {
        unsigned long long salt = (unsigned long long)monotonic_ms();
        Json *narrator = benchmark_prompt(salt, false), *agent = benchmark_prompt(salt, true);
        int cold = sample(server, token, "bench-narrator", narrator, stop);
        int warm = cold >= 0 ? sample(server, token, "bench-narrator", narrator, stop) : -1;
        int other = warm >= 0 ? sample(server, token, "bench-agent", agent, stop) : -1;
        int after = other >= 0 ? sample(server, token, "bench-narrator", narrator, stop) : -1;
        cJSON_Delete(agent);
        cJSON_Delete(narrator);
        if (after < 0) return 1;
        printf("Round %d  cold %d ms  warm %d ms  agent %d ms  after agent %d ms\n",
               i + 1, cold, warm, other, after);
        cold_sum += cold;
        warm_sum += warm;
        after_sum += after;
    }
    if (*stop) return 1;
    double ratio = (double)after_sum / (warm_sum ? warm_sum : 1);
    printf("After-agent/warm TTFT ratio %.2fx. ", ratio);
    if (cold_sum <= warm_sum * 13 / 10) puts("Inconclusive: cold and warm are too close.");
    else if (after_sum <= warm_sum + (cold_sum - warm_sum) / 3) puts("Timing is consistent with cache retention.");
    else if (after_sum >= cold_sum - (cold_sum - warm_sum) / 3) puts("Timing is consistent with cache eviction.");
    else puts("Inconclusive: after-agent timing falls between warm and cold.");
    return 0;
}
