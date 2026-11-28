#include "neutrino.h"
#include "socket_io.h"

#include <curl/curl.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { READY_LIMIT = 16, HEADER_CLIENTS = 4, BODY_CLIENTS = 4,
       CLIENTS = READY_LIMIT + HEADER_CLIENTS + BODY_CLIENTS,
       HEADER_LIMIT = 16384, BODY_LIMIT = 16 * 1024 * 1024,
       HEADER_TIMEOUT_MS = 5000, BODY_TIMEOUT_MS = 12000 };
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
    uint64_t touched, request_started;
    Text request, send, output, thinking, error;
    size_t sent, body_offset, body_length;
    Inference *job;
    Json *messages;
    Route route;
    bool closing, provider_request, model_catalog;
} Client;
typedef struct {
    const char *endpoint, *model, *key, *key_env;
    bool stream_usage;
} Provider;
struct Gateway {
    Client clients[CLIENTS];
    Socket listener;
    const char *host, *provider_admin, *token;
    Provider provider;
    char *provider_endpoint, *provider_model, *provider_key_env;
    Workspace profiles;
    bool profile_active;
    int workers;
    uint64_t last_provider_result;
    bool provider_ok;
};

static void configure_provider(Client *c, Gateway *gateway, const Json *settings);
static void finish_model_catalog(Client *c);

static bool route_for(const Provider *provider, const char *name, Route *route)
{
    const char *base = provider->endpoint;
    const char *key = provider->key;
    const bool slots = !strcmp(env("NEUTRINO_LLAMA_SLOTS", ""), "1");
    *route = (Route){.endpoint = base, .key = key, .model = provider->model,
        .options = {.slot = slots ? 0 : -1,
                    .include_usage = provider->stream_usage}, .lane = 0};
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
        if (!strncmp(binding, "default:", 8) && !strcmp(binding + 8, provider->model)) {
            route->endpoint = base;
            route->key = key;
            route->lane = slots ? 1 : 0;
            route->options.slot = slots ? 1 : -1;
            route->options.include_usage = provider->stream_usage;
            recall_model = provider->model;
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

static void respond_json(Client *c, Json *value)
{
    char *body = cJSON_PrintUnformatted(value);
    if (!body) abort();
    format(&c->send,"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
           "Cache-Control: no-store\r\nConnection: close\r\nContent-Length: %zu\r\n\r\n%s",strlen(body),body);
    c->closing = true;
    cJSON_free(body);
    cJSON_Delete(value);
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
    set(status, "model", gateway->provider.model);
    set(status, "provider", !gateway->last_provider_result ? "untested" : gateway->provider_ok ? "ok" : "failed");
    if (gateway->last_provider_result)
        put(status, "provider_age_s", cJSON_CreateNumber((double)((monotonic_ms() - gateway->last_provider_result) / 1000)));
    integer(status, "active", active);
    integer(status, "queued", queued);
    integer(status, "workers", pinned ? 1 : gateway->workers);
    put(status, "pinned_slots", cJSON_CreateBool(pinned));
    respond_json(c,status);
}

static bool receiving_request(const Client *c)
{
    return c->socket != BAD_SOCKET && !c->closing && !c->messages && !c->job;
}

static void parse_headers(Client *c, const char *token, const Gateway *gateway)
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
    const char provider_path[] = "POST /v1/provider HTTP/1.1";
    bool status_request = (size_t)(line_end - s) == sizeof status_path - 1 &&
                          !memcmp(s, status_path, sizeof status_path - 1);
    c->provider_request = (size_t)(line_end-s) == sizeof provider_path-1 &&
                          !memcmp(s,provider_path,sizeof provider_path-1);
    if (!status_request && !c->provider_request &&
        ((size_t)(line_end - s) != sizeof path - 1 || memcmp(s, path, sizeof path - 1))) {
        http_error(c, 404, "Not Found");
        return;
    }
    if (c->provider_request) {
        if (!*gateway->provider_admin) { http_error(c,404,"Not Found"); return; }
        token = gateway->provider_admin;
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
            if (has_host || (!local_host(value, end) &&
                (!*gateway->host || !field_is(value, (size_t)(end - value), gateway->host)))) {
                http_error(c, 400, "Invalid Host"); return;
            }
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
    if (c->provider_request && length > 4096) { http_error(c,413,"Body Too Large"); return; }
    if (c->request.len < offset + length) {
        int bodies = 0;
        for (int i = 0; i < CLIENTS; ++i)
            if (receiving_request(&gateway->clients[i]) && gateway->clients[i].body_offset) ++bodies;
        if (bodies >= BODY_CLIENTS) { http_error(c, 503, "Request Intake Busy"); return; }
    }
    c->body_offset = offset;
    c->body_length = length;
    c->request_started = monotonic_ms();
}

static void parse_request(Client *c, const char *token, Gateway *gateway)
{
    if (!c->body_offset) parse_headers(c, token, gateway);
    if (c->closing || !c->body_offset || c->request.len < c->body_offset + c->body_length) return;
    size_t offset = c->body_offset, length = c->body_length;
    const char *s = str(&c->request);
    if (c->request.len != offset + length || memchr(s + offset, 0, length)) {
        http_error(c, 400, "Bad Request"); return;
    }
    if (c->provider_request) {
        Json *settings = cJSON_ParseWithOpts(s+offset,nullptr,true);
        configure_provider(c,gateway,settings);
        cJSON_Delete(settings);
        return;
    }
    int ready = 0;
    for (int i = 0; i < CLIENTS; ++i)
        if (gateway->clients[i].messages || gateway->clients[i].job) ++ready;
    if (ready >= READY_LIMIT) { http_error(c, 503, "Gateway Busy"); return; }
    Json *body = cJSON_ParseWithOpts(s + offset, NULL, 1);
    Json *messages = at(body, "messages");
    if (!cJSON_IsArray(messages) || !messages->child ||
        cJSON_GetArraySize(messages) > NEUTRINO_GATEWAY_MESSAGE_LIMIT ||
        !route_for(&gateway->provider,js(body, "model"), &c->route)) {
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
    if (metrics.prompt_tokens > 0) integer(data, "prompt_tokens", metrics.prompt_tokens);
    if (metrics.clean_stop) put(data, "clean_stop", cJSON_CreateTrue());
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
        if (c->socket != BAD_SOCKET && c->model_catalog) {
            if (done) finish_model_catalog(c);
        } else if (c->socket != BAD_SOCKET) {
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
    if (receiving_request(c) &&
        monotonic_ms() - c->request_started >= (c->body_offset ? BODY_TIMEOUT_MS : HEADER_TIMEOUT_MS)) {
        discard_socket(c);
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

static void accept_client(Gateway *gateway, Socket incoming)
{
    if (!nonblocking(incoming)) { close_socket(incoming); return; }
    Client *free_slot = nullptr, *oldest_header = nullptr;
    int headers = 0;
    for (int i = 0; i < CLIENTS; ++i) {
        Client *client = &gateway->clients[i];
        if (client->socket == BAD_SOCKET && !client->job) free_slot = client;
        else if (receiving_request(client) && !client->body_offset) {
            ++headers;
            if (!oldest_header || client->request_started < oldest_header->request_started)
                oldest_header = client;
        }
    }
    if (oldest_header && (!free_slot || headers >= HEADER_CLIENTS)) free_slot = oldest_header;
    if (!free_slot) { close_socket(incoming); return; }
    reset_client(free_slot);
    free_slot->socket = incoming;
    free_slot->request_started = free_slot->touched = monotonic_ms();
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

static bool provider_url(const char *url)
{
    if ((strncmp(url,"http://",7) && strncmp(url,"https://",8)) || strpbrk(url," \t\r\n?#@")) return false;
    CURLU *parsed = curl_url();
    if (!parsed) return false;
    const bool valid = curl_url_set(parsed,CURLUPART_URL,url,CURLU_DISALLOW_USER) == CURLUE_OK;
    curl_url_cleanup(parsed);
    return valid;
}

static bool provider_key_variable(const char *key)
{
    const char prefix[] = "NEUTRINO_PROVIDER_KEY_";
    return !strncmp(key,prefix,sizeof prefix-1) && key[sizeof prefix-1];
}

static const char *provider_setup(Provider *provider, const ProviderSetup *setup, bool allow_empty_model)
{
    const char *model = setup->model ? setup->model : env("NEUTRINO_MODEL","");
    if ((!*model && !allow_empty_model) || (setup->model && strpbrk(model," \t\r\n")))
        return "Pass --model MODEL_ID or set NEUTRINO_MODEL before starting the gateway.";
    const char *endpoint = setup->endpoint ? setup->endpoint : env("NEUTRINO_ENDPOINT","http://127.0.0.1:11434/v1");
    const char *key_env = setup->key_env;
    if (setup->endpoint) {
        if (!strcmp(endpoint,"ollama")) endpoint = "http://127.0.0.1:11434/v1";
        else if (!strcmp(endpoint,"llamacpp")) endpoint = "http://127.0.0.1:8080/v1";
        else if (!strcmp(endpoint,"openai")) {
            endpoint = "https://api.openai.com/v1";
            if (!key_env) key_env = "OPENAI_API_KEY";
        }
        if (!provider_url(endpoint))
            return "Use a provider preset or an HTTP(S) API URL without credentials, query, or fragment.";
    }
    const char *key = setup->endpoint ? "" : env("NEUTRINO_API_KEY","");
    if (key_env) {
        const size_t letters = strspn(key_env,"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_");
        if (!letters || key_env[letters + strspn(key_env+letters,"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_0123456789")]) {
            return "Use the name of an API key environment variable.";
        }
        key = env(key_env,"");
        if (!*key || strpbrk(key,"\r\n")) return "The provider API key environment variable is empty or invalid.";
    }
    if (!key_env && !setup->endpoint && *key) key_env = "NEUTRINO_API_KEY";
    *provider = (Provider){endpoint,model,key,key_env,!strcmp(env("NEUTRINO_STREAM_USAGE",""),"1")};
    return nullptr;
}

enum { PROVIDER_PROFILE_LIMIT = 32 };

static bool profile_name(const char *name)
{
    size_t n = strlen(name);
    if (!n || n > 48 || *name == ' ' || name[n-1] == ' ') return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
        if (*p < 32 || *p == 127) return false;
    return true;
}

static bool profile_shape(const Json *entry)
{
    const Json *key = at(entry,"key_env");
    return cJSON_IsObject(entry) && cJSON_GetArraySize(entry) == (key ? 4 : 3) &&
        cJSON_IsString(at(entry,"endpoint")) && provider_url(js(entry,"endpoint")) &&
        cJSON_IsString(at(entry,"model")) && *js(entry,"model") && strlen(js(entry,"model")) <= 160 &&
        !strpbrk(js(entry,"model")," \t\r\n") && cJSON_IsBool(at(entry,"stream_usage")) &&
        (!key || (cJSON_IsString(key) && *js(entry,"key_env")));
}

static const char *load_profile(Provider *provider, const Json *entry)
{
    if (!profile_shape(entry)) return "Invalid saved provider profile.";
    const ProviderSetup setup = {js(entry,"endpoint"),js(entry,"model"),
                                at(entry,"key_env") ? js(entry,"key_env") : nullptr};
    const char *error = provider_setup(provider,&setup,false);
    if (!error) provider->stream_usage = cJSON_IsTrue(at(entry,"stream_usage"));
    return error;
}

static bool open_profiles(Gateway *gateway, const char *path)
{
    Workspace *store = &gateway->profiles;
    store->path = path;
    if (!workspace_lock(store)) return false;
    char *text = read_file(store->path);
    if (!text && errno != ENOENT) return false;
    store->root = cJSON_ParseWithOpts(text ? text : "{\"profiles\":{},\"active\":\"\"}",nullptr,true);
    free(text);
    const Json *list = at(store->root,"profiles");
    if (!cJSON_IsObject(list) || cJSON_GetArraySize(list) > PROVIDER_PROFILE_LIMIT ||
        !cJSON_IsString(at(store->root,"active"))) return false;
    const Json *entry;
    cJSON_ArrayForEach(entry,list)
        if (!profile_name(entry->string) || at(list,entry->string) != entry || !profile_shape(entry)) return false;
    const char *active = js(store->root,"active");
    if (*active) {
        const char *error = load_profile(&gateway->provider,at(list,active));
        if (error) { fprintf(stderr,"%s\n",error); return false; }
        gateway->profile_active = true;
    }
    return true;
}

static void replace_provider(Gateway *gateway, Provider provider)
{
    char *endpoint = copy(provider.endpoint), *model = copy(provider.model);
    char *key_env = provider.key_env ? copy(provider.key_env) : nullptr;
    free(gateway->provider_endpoint);
    free(gateway->provider_model);
    free(gateway->provider_key_env);
    provider.endpoint = gateway->provider_endpoint = endpoint;
    provider.model = gateway->provider_model = model;
    provider.key_env = gateway->provider_key_env = key_env;
    gateway->provider = provider;
    gateway->last_provider_result = 0;
}

static bool write_profiles(const Gateway *gateway, const Json *draft)
{
    char *data = cJSON_PrintUnformatted(draft);
    const bool saved = data && write_file(gateway->profiles.path,data,true);
    cJSON_free(data);
    return saved;
}

static const char *manage_profile(Gateway *gateway, const Json *settings)
{
    const char *edit = js(settings,"edit_profile"), *remove = js(settings,"delete_profile");
    const Json *profiles = at(gateway->profiles.root,"profiles");
    const char *name = *remove ? remove : edit;
    const Json *old = at(profiles,name);
    if (!old) return "Saved provider profile not found.";
    const bool selected = !strcmp(js(gateway->profiles.root,"active"),name);
    const char *renamed = js(settings,"save_as");
    if (!*renamed) renamed = name;
    Provider replacement = {0};
    Json *entry = nullptr;
    if (*remove) {
        if (cJSON_GetArraySize(settings) != 1) return "Delete one profile at a time.";
    } else {
        if (*js(settings,"use_profile") || !profile_name(renamed) || !cJSON_IsBool(at(settings,"stream_usage")))
            return "Enter valid profile settings.";
        if (strcmp(name,renamed) && at(profiles,renamed)) return "A profile already uses that name.";
        const char *endpoint = js(settings,"endpoint"), *model = js(settings,"model"), *key = js(settings,"key_env");
        const bool clear_key = cJSON_IsTrue(at(settings,"clear_key"));
        if (!provider_url(endpoint) || !*model || (clear_key && *key)) return "Enter an API URL and one model ID.";
        if (!*key && !clear_key && at(old,"key_env")) {
            if (strcmp(endpoint,js(old,"endpoint"))) return "Changing the URL requires a key variable or clearing the key.";
            key = js(old,"key_env");
        }
        entry = cJSON_CreateObject();
        set(entry,"endpoint",endpoint);
        set(entry,"model",model);
        if (*key) set(entry,"key_env",key);
        put(entry,"stream_usage",cJSON_CreateBool(cJSON_IsTrue(at(settings,"stream_usage"))));
        const char *error = !profile_shape(entry) ? "Invalid saved provider profile." :
            (selected && gateway->profile_active) || *js(settings,"key_env") ? load_profile(&replacement,entry) : nullptr;
        if (error) { cJSON_Delete(entry); return error; }
    }
    Json *draft = cJSON_Duplicate(gateway->profiles.root,true);
    if (!draft) abort();
    Json *list = at(draft,"profiles");
    if (*remove || strcmp(name,renamed)) cJSON_DeleteItemFromObjectCaseSensitive(list,name);
    if (entry) put(list,renamed,entry);
    if (selected) set(draft,"active",*remove ? "" : renamed);
    if (!write_profiles(gateway,draft)) { cJSON_Delete(draft); return "Could not write the gateway provider profile file."; }
    if (selected && gateway->profile_active) {
        if (*remove) gateway->profile_active = false;
        else replace_provider(gateway,replacement);
    }
    cJSON_Delete(gateway->profiles.root);
    gateway->profiles.root = draft;
    return nullptr;
}

static const char *update_provider(Gateway *gateway, const Json *settings)
{
    if (!cJSON_IsObject(settings)) return "Invalid provider settings.";
    if (!settings->child) return nullptr;
    const Json *field;
    cJSON_ArrayForEach(field,settings) {
        if (!field->string || at(settings,field->string) != field) return "Repeated provider setting.";
        if ((!strcmp(field->string,"stream_usage") || !strcmp(field->string,"clear_key")) && cJSON_IsBool(field)) continue;
        if ((!strcmp(field->string,"endpoint") || !strcmp(field->string,"model") ||
             !strcmp(field->string,"key_env") || !strcmp(field->string,"save_as") ||
             !strcmp(field->string,"use_profile") || !strcmp(field->string,"edit_profile") ||
             !strcmp(field->string,"delete_profile")) && cJSON_IsString(field) && *field->valuestring) continue;
        return "Invalid provider setting.";
    }
    const char *endpoint = js(settings,"endpoint"), *model = js(settings,"model"), *key = js(settings,"key_env");
    const char *save_as = js(settings,"save_as"), *use = js(settings,"use_profile");
    if ((*save_as && !profile_name(save_as)) || (*use && cJSON_GetArraySize(settings) != 1))
        return "Enter one valid profile name.";
    if ((*endpoint && !*model) || (*key && !*endpoint))
        return "Set the endpoint and model together.";
    if (strlen(model) > 160 || strpbrk(model," \t\r\n")) return "Enter one model ID.";
    if (*key && !provider_key_variable(key))
        return "Use a gateway key variable named NEUTRINO_PROVIDER_KEY_<NAME>.";
    for (int i = 0; i < CLIENTS; ++i)
        if (gateway->clients[i].job || gateway->clients[i].messages)
            return "The gateway has active requests. Try again after they finish.";
    if (*js(settings,"edit_profile") || *js(settings,"delete_profile")) return manage_profile(gateway,settings);
    if (at(settings,"clear_key")) return "Clear a key while editing a profile.";
    Provider replacement = gateway->provider;
    const Json *profiles = at(gateway->profiles.root,"profiles");
    if (*use) {
        const Json *entry = at(profiles,use);
        if (!entry) return "Saved provider profile not found.";
        const char *error = load_profile(&replacement,entry);
        if (error) return error;
    } else if (*endpoint) {
        const ProviderSetup setup = {endpoint,model,*key ? key : nullptr};
        const char *error = provider_setup(&replacement,&setup,false);
        if (error) return error;
        replacement.stream_usage = gateway->provider.stream_usage;
    }
    if (*model) replacement.model = model;
    if (at(settings,"stream_usage"))
        replacement.stream_usage = cJSON_IsTrue(at(settings,"stream_usage"));
    if (*save_as || *use) {
        if (*save_as && !at(profiles,save_as) && cJSON_GetArraySize(profiles) >= PROVIDER_PROFILE_LIMIT)
            return "The gateway already has 32 saved provider profiles.";
        if (!*replacement.model || !provider_url(replacement.endpoint)) return "Configure the API URL and model first.";
        Json *draft = cJSON_Duplicate(gateway->profiles.root,true);
        if (!draft) abort();
        if (*save_as) {
            Json *entry = cJSON_CreateObject();
            set(entry,"endpoint",replacement.endpoint);
            set(entry,"model",replacement.model);
            if (replacement.key_env) set(entry,"key_env",replacement.key_env);
            put(entry,"stream_usage",cJSON_CreateBool(replacement.stream_usage));
            put(at(draft,"profiles"),save_as,entry);
        }
        set(draft,"active",*save_as ? save_as : use);
        if (!write_profiles(gateway,draft)) { cJSON_Delete(draft); return "Could not write the gateway provider profile file."; }
        replace_provider(gateway,replacement);
        cJSON_Delete(gateway->profiles.root);
        gateway->profiles.root = draft;
        gateway->profile_active = true;
    } else {
        replace_provider(gateway,replacement);
        gateway->profile_active = false;
    }
    return nullptr;
}

static const char *start_model_catalog(Client *c, const Gateway *gateway, const Json *settings)
{
    const Json *field;
    cJSON_ArrayForEach(field,settings) {
        if (!field->string || at(settings,field->string) != field) return "Repeated provider setting.";
        if ((!strcmp(field->string,"discover") || !strcmp(field->string,"clear_key")) && cJSON_IsBool(field)) continue;
        if ((!strcmp(field->string,"endpoint") || !strcmp(field->string,"profile") ||
             !strcmp(field->string,"key_env")) && cJSON_IsString(field) && *field->valuestring) continue;
        return "Invalid model discovery setting.";
    }
    const char *endpoint = js(settings,"endpoint"), *name = js(settings,"profile"), *key = js(settings,"key_env");
    const bool clear_key = cJSON_IsTrue(at(settings,"clear_key"));
    if ((*key && (!*endpoint || clear_key)) || (clear_key && (!*name || !*endpoint)))
        return "Choose an API URL and one key setting.";
    if (*key && !provider_key_variable(key)) return "Use a gateway key variable named NEUTRINO_PROVIDER_KEY_<NAME>.";
    Provider provider = gateway->provider;
    if (*name) {
        const Json *entry = at(at(gateway->profiles.root,"profiles"),name);
        if (!entry) return "Saved provider profile not found.";
        if (!*endpoint) {
            const char *error = load_profile(&provider,entry);
            if (error) return error;
        } else if (!*key && !clear_key && at(entry,"key_env")) {
            if (strcmp(endpoint,js(entry,"endpoint"))) return "Changing the URL requires a key variable or clearing the key.";
            key = js(entry,"key_env");
        }
    }
    if (*endpoint) {
        const ProviderSetup setup = {endpoint,"models",*key ? key : nullptr};
        const char *error = provider_setup(&provider,&setup,false);
        if (error) return error;
    }
    if (!provider_url(provider.endpoint)) return "Enter a valid provider API URL.";
    int active = 0;
    for (int i = 0; i < CLIENTS; ++i)
        if (gateway->clients[i].job && gateway->clients[i].model_catalog) ++active;
    if (active >= 2) return "The gateway already has model lookups running.";
    c->job = inference_models(provider.endpoint,provider.key);
    if (!c->job) return "Could not start the provider connection test.";
    c->model_catalog = true;
    return nullptr;
}

static void finish_model_catalog(Client *c)
{
    Json *reply = cJSON_CreateObject();
    Json *catalog = !c->error.len ? cJSON_ParseWithOpts(str(&c->output),nullptr,true) : nullptr;
    const Json *data = at(catalog,"data"), *item;
    if (c->error.len) set(reply,"error",str(&c->error));
    else if (!cJSON_IsArray(data)) set(reply,"error","The provider did not return an OpenAI-compatible model list.");
    else {
        Json *models = cJSON_CreateArray();
        cJSON_ArrayForEach(item,data) {
            const char *id = js(item,"id");
            const unsigned char *p = (const unsigned char *)id;
            while (*p > 32 && *p != 127) ++p;
            if (!*id || *p || strlen(id)>160 ||
                inference_contains_key(c->job,id)) continue;
            const Json *prior;
            cJSON_ArrayForEach(prior,models)
                if (!strcmp(cJSON_GetStringValue(prior),id)) break;
            if (prior) continue;
            if (cJSON_GetArraySize(models) == 200) {
                put(reply,"truncated",cJSON_CreateTrue());
                break;
            }
            cJSON_AddItemToArray(models,cJSON_CreateString(id));
        }
        put(reply,"models",models);
    }
    cJSON_Delete(catalog);
    respond_json(c,reply);
}

static void configure_provider(Client *c, Gateway *gateway, const Json *settings)
{
    if (cJSON_IsTrue(at(settings,"discover"))) {
        const char *problem = start_model_catalog(c,gateway,settings);
        if (problem) {
            Json *reply = cJSON_CreateObject();
            set(reply,"error",problem);
            respond_json(c,reply);
        }
        return;
    }
    const char *error = update_provider(gateway,settings);
    Json *response = cJSON_CreateObject();
    if (error) set(response,"error",error);
    else {
        const Provider *provider = &gateway->provider;
        set(response,"model",provider->model);
        if (provider_url(provider->endpoint)) set(response,"endpoint",provider->endpoint);
        put(response,"authenticated",cJSON_CreateBool(*provider->key));
        put(response,"stream_usage",cJSON_CreateBool(provider->stream_usage));
        Json *names = cJSON_CreateArray();
        Json *details = cJSON_CreateObject();
        const Json *entry;
        cJSON_ArrayForEach(entry,at(gateway->profiles.root,"profiles")) {
            cJSON_AddItemToArray(names,cJSON_CreateString(entry->string));
            Json *info = cJSON_CreateObject();
            set(info,"endpoint",js(entry,"endpoint"));
            set(info,"model",js(entry,"model"));
            put(info,"stream_usage",cJSON_CreateBool(cJSON_IsTrue(at(entry,"stream_usage"))));
            put(info,"authenticated",cJSON_CreateBool(at(entry,"key_env") != nullptr));
            put(details,entry->string,info);
        }
        put(response,"profiles",names);
        put(response,"profile_details",details);
        if (gateway->profile_active) set(response,"active_profile",js(gateway->profiles.root,"active"));
    }
    respond_json(c,response);
}

static Gateway *gateway_open(const ProviderSetup *setup, const char *token, const char *admin,
                             const char *bind_to, const char *host, const char *profiles, int *port, int *code)
{
    Provider provider;
    *code = 2;
    if (*admin && (strlen(admin) < 16 || strpbrk(admin," \t\r\n") || !strcmp(admin,token))) {
        fputs("Use a separate provider admin password of at least 16 characters without whitespace.\n", stderr);
        return nullptr;
    }
    const char *error = provider_setup(&provider,setup,*admin && !setup->model);
    if (error) { fprintf(stderr,"%s\n",error); return nullptr; }
    const char *configured_workers = env("NEUTRINO_WORKERS", "2");
    if (strlen(configured_workers) != 1 || configured_workers[0] < '1' || configured_workers[0] > '4') {
        fputs("NEUTRINO_WORKERS must be a number from 1 to 4.\n", stderr);
        return nullptr;
    }
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons((unsigned short)*port)};
#ifdef _WIN32
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data)) { *code = 1; return nullptr; }
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    Socket listener = BAD_SOCKET;
    if (inet_pton(AF_INET, bind_to, &address.sin_addr) != 1 ||
        ((*host || (ntohl(address.sin_addr.s_addr) >> 24) != 127) && (!*host || strlen(token) < 16))) {
        fputs("Configure an IPv4 gateway bind. Remote access requires NEUTRINO_SERVER_HOST and a token of at least 16 characters.\n", stderr);
    } else {
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#ifndef _WIN32
        int reuse = 1;
        if (listener != BAD_SOCKET) setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
#endif
#ifdef _WIN32
        int address_length = sizeof address;
#else
        socklen_t address_length = sizeof address;
#endif
        if (listener != BAD_SOCKET && nonblocking(listener) &&
            !bind(listener, (struct sockaddr *)&address, sizeof address) && !listen(listener, CLIENTS) &&
            !getsockname(listener, (struct sockaddr *)&address, &address_length)) {
            Gateway *gateway = calloc(1,sizeof *gateway);
            if (!gateway) abort();
            gateway->listener = listener;
            gateway->host = host;
            gateway->provider_admin = copy(admin);
            gateway->token = copy(token);
            gateway->provider = provider;
            gateway->workers = configured_workers[0] - '0';
            for (int i = 0; i < CLIENTS; ++i) gateway->clients[i].socket = BAD_SOCKET;
            if (*admin && !open_profiles(gateway,profiles)) {
                fputs("Could not open the gateway provider profile file. Check its path, format, and active profile key.\n",stderr);
                gateway_close(gateway);
                return nullptr;
            }
            if (gateway->profile_active) replace_provider(gateway,gateway->provider);
            *port = ntohs(address.sin_port);
            *code = 0;
            return gateway;
        }
        fprintf(stderr, "Could not listen on %s:%d.\n", bind_to, *port);
        *code = 1;
    }
    if (listener != BAD_SOCKET) close_socket(listener);
#ifdef _WIN32
    WSACleanup();
#endif
    return nullptr;
}

Gateway *local_gateway_open(const char *password, const char *admin, const char *profiles, int *port)
{
    int code;
    const ProviderSetup setup = {0};
    return gateway_open(&setup,password,admin,"127.0.0.1","",profiles,port,&code);
}

void gateway_poll(Gateway *gateway)
{
    Client *clients = gateway->clients;
    for (int i = 0; i < CLIENTS; ++i) pump_client(&clients[i], gateway->token, gateway);
    Socket incoming = accept(gateway->listener, NULL, NULL);
    if (incoming != BAD_SOCKET) accept_client(gateway, incoming);
    dispatch(clients, gateway->workers);
}

void gateway_close(Gateway *gateway)
{
    if (!gateway) return;
    for (int i = 0; i < CLIENTS; ++i) discard_socket(&gateway->clients[i]);
    for (int i = 0; i < CLIENTS; ++i) reset_client(&gateway->clients[i]);
    free(gateway->provider_endpoint);
    free(gateway->provider_model);
    free(gateway->provider_key_env);
    workspace_close(&gateway->profiles);
    free((char *)gateway->provider_admin);
    free((char *)gateway->token);
    close_socket(gateway->listener);
    free(gateway);
#ifdef _WIN32
    WSACleanup();
#endif
}

int inference_server(int port, const ProviderSetup *setup, const volatile sig_atomic_t *stop)
{
    const char *bind_to = env("NEUTRINO_SERVER_BIND", "127.0.0.1");
    int code;
    Gateway *gateway = gateway_open(setup,env("NEUTRINO_SERVER_TOKEN",""),env("NEUTRINO_PROVIDER_ADMIN_TOKEN",""),
        bind_to,env("NEUTRINO_SERVER_HOST",""),env("NEUTRINO_PROVIDERS_FILE",".neutrino-providers.json"),&port,&code);
    if (!gateway) return code;
    printf("Neutrino server listening on http://%s:%d/v1\n", bind_to, port);
    fflush(stdout);
    while (!*stop) {
        gateway_poll(gateway);
        pause_ms(12);
    }
    gateway_close(gateway);
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
