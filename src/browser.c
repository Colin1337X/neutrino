#include "app.h"
#include "socket_io.h"
#include "browser_assets.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { WEB_CLIENTS = 8, WEB_HEADERS = 8192 };
typedef struct {
    Socket socket;
    uint64_t deadline;
    Text request, response;
    size_t offset, length, sent;
    BrowserRoute route;
    bool ready;
} BrowserClient;

struct Browser {
    Socket listener;
    int port;
    const char *token, *host;
    BrowserClient clients[WEB_CLIENTS];
};

static void close_client(BrowserClient *client)
{
    if (client->socket != BAD_SOCKET) close_socket(client->socket);
    release(&client->request);
    release(&client->response);
    *client = (BrowserClient){.socket = BAD_SOCKET};
}

static void reply(BrowserClient *client, int status, const char *type, const char *name,
                  const char *body, size_t length)
{
    const char *reason = status == 200 ? "OK" : status == 401 ? "Unauthorized" :
        status == 403 ? "Forbidden" : status == 404 ? "Not Found" :
        status == 409 ? "Conflict" : status == 413 ? "Content Too Large" : "Bad Request";
    format(&client->response, "HTTP/1.1 %d %s\r\nConnection: close\r\nContent-Type: %s\r\n"
           "Content-Length: %zu\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
           "Cross-Origin-Resource-Policy: same-origin\r\nReferrer-Policy: no-referrer\r\n"
           "Content-Security-Policy: default-src 'none'; script-src 'self'; style-src 'self'; "
           "connect-src 'self'; img-src 'self'; base-uri 'none'; frame-ancestors 'none'; form-action 'none'\r\n",
           status, reason, type, length);
    if (name) format(&client->response, "Content-Disposition: attachment; filename=\"%s\"\r\n", name);
    append(&client->response, "\r\n");
    append_n(&client->response, body, length);
    release(&client->request);
    client->ready = true;
    client->deadline = monotonic_ms() + 10000;
}

static void web_error(BrowserClient *client, int status, const char *message)
{
    reply(client, status, "text/plain; charset=utf-8", NULL, message, strlen(message));
}

static bool equal_field(const char *left, const char *right)
{
    while (*left && *right)
        if (tolower((unsigned char)*left++) != tolower((unsigned char)*right++)) return false;
    return *left == *right;
}

static bool host_allowed(const Browser *web, const char *host)
{
    if (*web->host && equal_field(web->host, host)) return true;
    char address[32], localhost[32];
    snprintf(address, sizeof address, "127.0.0.1:%d", web->port);
    snprintf(localhost, sizeof localhost, "localhost:%d", web->port);
    return equal_field(host, "localhost") || !strcmp(host, "127.0.0.1") ||
        !strcmp(host, address) || equal_field(host, localhost);
}

static bool authorized(const Browser *web, const char *value)
{
    size_t length = strlen(web->token);
    if (!length) return true;
    if (!value || strlen(value) != length + 7 || strncmp(value, "Bearer ", 7)) return false;
    unsigned int diff = 0;
    for (size_t i = 0; i < length; ++i) diff |= (unsigned char)(value[i + 7] ^ web->token[i]);
    return diff == 0;
}

static void parse_headers(const Browser *web, BrowserClient *client)
{
    char *headers = client->request.data, *end = strstr(headers, "\r\n\r\n");
    if (!end) {
        if (client->request.len >= WEB_HEADERS || memchr(headers, 0, client->request.len))
            web_error(client, 400, "Invalid request headers.");
        return;
    }
    size_t offset = (size_t)(end - headers) + 4;
    if (offset > WEB_HEADERS) { web_error(client, 400, "Request headers are too large."); return; }
    char *line = strstr(headers, "\r\n");
    *line = 0;
    bool post = !strncmp(headers, "POST ", 5);
    char *path = post ? headers + 5 : !strncmp(headers, "GET ", 4) ? headers + 4 : NULL;
    char *version = path ? strchr(path, ' ') : NULL;
    if (!version || strcmp(version, " HTTP/1.1")) {
        web_error(client, 400, "Use HTTP/1.1 with GET or POST.");
        return;
    }
    *version = 0;
    const char *host = NULL, *auth = NULL, *marker = NULL, *length = NULL;
    for (line += 2; line <= end && line[0] != '\r';) {
        char *next = strstr(line, "\r\n"), *colon = strchr(line, ':');
        if (!next || !colon || colon >= next) { web_error(client, 400, "Invalid header field."); return; }
        *next = *colon = 0;
        char *value = colon + 1;
        while (*value == ' ' || *value == '\t') ++value;
        const char **field = equal_field(line, "Host") ? &host :
            equal_field(line, "Authorization") ? &auth :
            equal_field(line, "X-Neutrino-Web") ? &marker :
            equal_field(line, "Content-Length") ? &length : NULL;
        if ((field && *field) || equal_field(line, "Transfer-Encoding") || equal_field(line, "Expect")) {
            web_error(client, 400, "Unsupported or repeated request header.");
            return;
        }
        if (field) *field = value;
        line = next + 2;
    }
    if (!host || !host_allowed(web, host)) { web_error(client, 403, "Unconfigured browser host."); return; }
    size_t size = 0;
    if (length) {
        if (!*length) { web_error(client, 400, "Content length is empty."); return; }
        for (const char *digit = length; *digit; ++digit) {
            if (*digit < '0' || *digit > '9') { web_error(client, 400, "Invalid content length."); return; }
            size = size * 10 + (size_t)(*digit - '0');
            if (size > FILE_LIMIT) { web_error(client, 413, "The upload exceeds 32 MiB."); return; }
        }
    }
    if ((!post && size) || (post && !length)) { web_error(client, 400, "Invalid request body."); return; }
    static const struct { const char *path, *type; const unsigned char *data; size_t size; } assets[] = {
        {"/", "text/html; charset=utf-8", index_html, sizeof index_html - 1},
        {"/app.css", "text/css; charset=utf-8", app_css, sizeof app_css - 1},
        {"/app.js", "text/javascript; charset=utf-8", app_js, sizeof app_js - 1}
    };
    for (size_t i = 0; !post && i < sizeof assets / sizeof *assets; ++i) {
        if (strcmp(path, assets[i].path)) continue;
        reply(client, 200, assets[i].type, NULL, (const char *)assets[i].data, assets[i].size);
        return;
    }
    static const struct { const char *path; bool post; BrowserRoute route; } routes[] = {
        {"/api/state", false, BROWSER_STATE}, {"/api/history", false, BROWSER_HISTORY},
        {"/api/action", true, BROWSER_ACTION}, {"/api/import", true, BROWSER_IMPORT},
        {"/api/lore", true, BROWSER_LORE}, {"/api/preset", true, BROWSER_PRESET},
        {"/api/export", false, BROWSER_EXPORT}, {"/api/trace", false, BROWSER_TRACE},
        {"/api/candidate", false, BROWSER_CANDIDATE}
    };
    for (size_t i = 0; i < sizeof routes / sizeof *routes; ++i) {
        if (routes[i].post != post || strcmp(path, routes[i].path)) continue;
        if (!marker || strcmp(marker, "1")) { web_error(client, 403, "Use the Neutrino browser client."); return; }
        if (!authorized(web, auth)) { web_error(client, 401, "Enter the browser access token."); return; }
        client->route = routes[i].route;
        client->offset = offset;
        client->length = size;
        client->deadline = monotonic_ms() + 30000;
        return;
    }
    web_error(client, 404, "Page not found.");
}

static void handle_request(BrowserClient *client, App *app)
{
    if (client->request.len != client->offset + client->length) {
        web_error(client, 400, "Request length does not match its header.");
        return;
    }
    Text input = {.data = client->request.data + client->offset, .len = client->length};
    Text output = {0};
    int status = app_browser_response(app, client->route, &input, &output);
    const char *type = "application/json; charset=utf-8", *name = NULL;
    if (status != 200) type = "text/plain; charset=utf-8";
    else if (client->route == BROWSER_EXPORT) name = "neutrino-workspace.json";
    else if (client->route == BROWSER_TRACE || client->route == BROWSER_CANDIDATE) {
        type = "text/plain; charset=utf-8";
        name = client->route == BROWSER_TRACE ? "neutrino-trace.txt" : "neutrino-candidate.txt";
    }
    reply(client, status, type, name, str(&output), output.len);
    release(&output);
}

static void pump(const Browser *web, BrowserClient *client, App *app)
{
    if (monotonic_ms() >= client->deadline) { close_client(client); return; }
    char buffer[16384];
    for (int i = 0; i < 16 && !client->ready; ++i) {
        Count bytes = recv(client->socket, buffer, sizeof buffer, 0);
        if (bytes <= 0) {
            if (!bytes || !would_block()) close_client(client);
            return;
        }
        append_n(&client->request, buffer, (size_t)bytes);
        if (!client->offset) parse_headers(web, client);
        if (!client->ready && client->offset && client->request.len >= client->offset + client->length)
            handle_request(client, app);
    }
    if (client->ready) {
        size_t left = client->response.len - client->sent;
        Count n = send(client->socket, client->response.data + client->sent, left > 131072 ? 131072 : (int)left, 0);
        if (n > 0) client->sent += (size_t)n;
        if ((n < 0 && !would_block()) || client->sent == client->response.len) close_client(client);
    }
}

Browser *browser_open(int port)
{
    const char *bind_to = env("NEUTRINO_WEB_BIND", "127.0.0.1");
    const char *host = env("NEUTRINO_WEB_HOST", ""), *token = env("NEUTRINO_WEB_TOKEN", "");
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons((unsigned short)port)};
#ifdef _WIN32
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data)) return NULL;
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    Socket listener = BAD_SOCKET;
    if (inet_pton(AF_INET, bind_to, &address.sin_addr) != 1 ||
        ((*host || (ntohl(address.sin_addr.s_addr) >> 24) != 127) && (!*host || strlen(token) < 16))) {
        fputs("Configure an IPv4 browser bind. Remote access requires NEUTRINO_WEB_HOST and a token of at least 16 characters.\n", stderr);
    } else {
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#ifndef _WIN32
        int reuse = 1;
        if (listener != BAD_SOCKET) setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
#endif
        if (listener != BAD_SOCKET && nonblocking(listener) &&
            !bind(listener, (struct sockaddr *)&address, sizeof address) && !listen(listener, WEB_CLIENTS)) {
            Browser *web = malloc(sizeof *web);
            if (!web) abort();
            *web = (Browser){.listener = listener, .port = port, .token = token, .host = host};
            for (int i = 0; i < WEB_CLIENTS; ++i) web->clients[i].socket = BAD_SOCKET;
            printf("Neutrino browser listening on http://%s:%d\n", bind_to, port);
            fflush(stdout);
            return web;
        }
        fprintf(stderr, "Could not open browser listener on %s:%d.\n", bind_to, port);
    }
    if (listener != BAD_SOCKET) close_socket(listener);
#ifdef _WIN32
    WSACleanup();
#endif
    return NULL;
}

void browser_poll(Browser *web, App *app)
{
    for (int i = 0; i < WEB_CLIENTS; ++i)
        if (web->clients[i].socket != BAD_SOCKET) pump(web, &web->clients[i], app);
    for (int i = 0; i < WEB_CLIENTS; ++i) {
        BrowserClient *client = &web->clients[i];
        if (client->socket != BAD_SOCKET) continue;
        Socket incoming = accept(web->listener, NULL, NULL);
        if (incoming == BAD_SOCKET) break;
        if (!nonblocking(incoming)) { close_socket(incoming); continue; }
        client->socket = incoming;
        client->deadline = monotonic_ms() + 5000;
    }
}

void browser_close(Browser *web)
{
    for (int i = 0; i < WEB_CLIENTS; ++i) close_client(&web->clients[i]);
    close_socket(web->listener);
    free(web);
#ifdef _WIN32
    WSACleanup();
#endif
}
