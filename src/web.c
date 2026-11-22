#include "web.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#endif
#include <curl/curl.h>

enum { WEB_BODY_LIMIT = 2 * 1024 * 1024, WEB_MAX_RESULTS = 12, WEB_EXCERPT = 1800, WEB_QUERY_MAX = 400 };

typedef struct {
    Text body;
    const atomic_bool *cancel;
} Download;

static bool cancelled(const atomic_bool *cancel)
{
    return atomic_load_explicit(cancel, memory_order_relaxed);
}

static curl_socket_t public_socket(void *unused, curlsocktype purpose, struct curl_sockaddr *address)
{
    (void)unused;
    if (purpose != CURLSOCKTYPE_IPCXN) return CURL_SOCKET_BAD;
    if (address->family == AF_INET) {
        const struct sockaddr_in *ip = (const void *)&address->addr;
        unsigned long a = ntohl(ip->sin_addr.s_addr);
        unsigned long first = a >> 24, second = (a >> 16) & 255, third = (a >> 8) & 255;
        if (!first || first == 10 || first == 127 || first >= 224 ||
            (first == 100 && second >= 64 && second <= 127) ||
            (first == 169 && second == 254) || (first == 172 && second >= 16 && second <= 31) ||
            (first == 192 && (second == 168 || (second == 0 && (third == 0 || third == 2)))) ||
            (first == 198 && (second == 18 || second == 19 || (second == 51 && third == 100))) ||
            (first == 203 && second == 0 && third == 113)) return CURL_SOCKET_BAD;
    } else if (address->family == AF_INET6) {
        const struct sockaddr_in6 *ip = (const void *)&address->addr;
        const unsigned char *a = ip->sin6_addr.s6_addr;
        if ((a[0] & 0xe0) != 0x20 || (a[0] == 0x20 && a[1] == 0x02) ||
            (a[0] == 0x20 && a[1] == 1 && (a[2] < 2 || (a[2] == 0x0d && a[3] == 0xb8))))
            return CURL_SOCKET_BAD;
    } else return CURL_SOCKET_BAD;
    return socket(address->family, address->socktype, address->protocol);
}

static bool url_allowed(const char *url, bool trusted)
{
    if (!*url || strlen(url) > 4096) return false;
    for (const unsigned char *p = (const unsigned char *)url; *p; ++p)
        if (*p <= 32 || *p == 127) return false;
    CURLU *parsed = curl_url();
    if (!parsed) abort();
    char *scheme = nullptr, *port = nullptr;
    bool ok = !curl_url_set(parsed, CURLUPART_URL, url, CURLU_DISALLOW_USER) &&
        !curl_url_get(parsed, CURLUPART_SCHEME, &scheme, 0) &&
        (!strcmp(scheme, "https") || (trusted && !strcmp(scheme, "http"))) &&
        (trusted || curl_url_get(parsed, CURLUPART_PORT, &port, 0) == CURLUE_NO_PORT ||
                    (port && !strcmp(port, "443")));
    curl_free(scheme);
    curl_free(port);
    curl_url_cleanup(parsed);
    return ok;
}

static size_t receive(void *data, size_t size, size_t count, void *context)
{
    Download *download = context;
    if (!size || count > (WEB_BODY_LIMIT - download->body.len) / size || cancelled(download->cancel)) return 0;
    size_t length = size * count;
    if (memchr(data, 0, length)) return 0;
    append_n(&download->body, data, length);
    return length;
}

static int progress(void *context, curl_off_t total, curl_off_t received, curl_off_t up, curl_off_t sent)
{
    (void)total; (void)received; (void)up; (void)sent;
    return cancelled(context);
}

typedef struct { Text body, final_url; long status; bool html, plain; } Document;

static bool download_page(const char *url, bool trusted, const atomic_bool *cancel,
                          Document *document, Text *redirect)
{
    if (cancelled(cancel) || !url_allowed(url, trusted)) return false;
    CURL *curl = curl_easy_init();
    if (!curl) return false;
    Download transfer = {.cancel = cancel};
    curl_easy_setopt(curl, CURLOPT_URL, url);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, trusted ? "http,https" : "https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, trusted ? CURLPROTO_HTTP | CURLPROTO_HTTPS : CURLPROTO_HTTPS);
#endif
    // The next hop passes through the same URL and destination checks as the first.
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_DISALLOW_USERNAME_IN_URL, 1L);
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_NETRC, (long)CURL_NETRC_IGNORED);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 2500L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 4500L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Neutrino/" NEUTRINO_VERSION " (headless research reader)");
#ifdef CURLSSLOPT_NATIVE_CA
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
#endif
    const char *ca_bundle = env("NEUTRINO_CA_BUNDLE", "");
    if (*ca_bundle) curl_easy_setopt(curl, CURLOPT_CAINFO, ca_bundle);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancel);
    if (!trusted) curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, public_socket);
    CURLcode code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &document->status);
    char *type = nullptr;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &type);
    document->html = type && (!strncmp(type, "text/html", 9) || !strncmp(type, "application/xhtml+xml", 21));
    document->plain = type && !strncmp(type, "text/plain", 10);
    if (code == CURLE_OK && !trusted && document->status >= 300 && document->status < 400) {
        char *next = nullptr;
        curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &next);
        if (next && url_allowed(next, false)) append(redirect, next);
    }
    curl_easy_cleanup(curl);
    if (code != CURLE_OK || document->status < 200 || document->status >= 300 || !transfer.body.len) {
        release(&transfer.body);
        return false;
    }
    document->body = transfer.body;
    append(&document->final_url, url);
    return true;
}

static bool download(const char *url, bool trusted, const atomic_bool *cancel, Document *document)
{
    Text hop = {0};
    append(&hop, url);
    bool ok = false;
    for (int i = 0; i < 3; ++i) {
        Text next = {0};
        ok = download_page(str(&hop), trusted, cancel, document, &next);
        release(&hop);
        hop = next;
        if (!hop.len) break;
    }
    release(&hop);
    return ok;
}

static size_t utf8_width(const unsigned char *s)
{
    unsigned c = *s;
    size_t n = c < 0x80 ? 1 : c >= 0xc2 && c <= 0xdf ? 2 :
               c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
    for (size_t i = 1; i < n; ++i) if ((s[i] & 0xc0) != 0x80) return 0;
    if ((c == 0xe0 && s[1] < 0xa0) || (c == 0xed && s[1] >= 0xa0) ||
        (c == 0xf0 && s[1] < 0x90) || (c == 0xf4 && s[1] >= 0x90)) return 0;
    return n;
}

static void codepoint(Text *out, unsigned c)
{
    char utf[4];
    size_t n = 0;
    if (c < 32 || (c >= 127 && c <= 159) || c == 0xa0) c = ' ';
    if (c < 0x80) utf[n++] = (char)c;
    else if (c < 0x800) {
        utf[n++] = (char)(0xc0 | (c >> 6)); utf[n++] = (char)(0x80 | (c & 63));
    } else if (c < 0x10000) {
        if (c >= 0xd800 && c <= 0xdfff) return;
        utf[n++] = (char)(0xe0 | (c >> 12)); utf[n++] = (char)(0x80 | ((c >> 6) & 63));
        utf[n++] = (char)(0x80 | (c & 63));
    } else if (c <= 0x10ffff) {
        utf[n++] = (char)(0xf0 | (c >> 18)); utf[n++] = (char)(0x80 | ((c >> 12) & 63));
        utf[n++] = (char)(0x80 | ((c >> 6) & 63)); utf[n++] = (char)(0x80 | (c & 63));
    }
    append_n(out, utf, n);
}

static const char *entity(Text *out, const char *s)
{
    static const struct { const char *name; unsigned value; } names[] = {
        {"amp;", '&'}, {"lt;", '<'}, {"gt;", '>'}, {"quot;", '"'}, {"apos;", '\''},
        {"nbsp;", ' '}, {"ndash;", 0x2013}, {"mdash;", 0x2014}, {"rsquo;", 0x2019},
        {"lsquo;", 0x2018}, {"ldquo;", 0x201c}, {"rdquo;", 0x201d}, {"hellip;", 0x2026}
    };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        size_t n = strlen(names[i].name);
        if (!strncmp(s + 1, names[i].name, n)) { codepoint(out, names[i].value); return s + n + 1; }
    }
    if (s[1] == '#') {
        const char *start = s + 2;
        int base = 10;
        if (*start == 'x' || *start == 'X') { base = 16; start++; }
        if (isxdigit((unsigned char)*start)) {
            char *end;
            unsigned long c = strtoul(start, &end, base);
            if (*end == ';' && end != start && c <= 0x10ffffUL) { codepoint(out, (unsigned)c); return end + 1; }
        }
    }
    append_n(out, s, 1);
    return s + 1;
}

static void visible_space(Text *out, bool paragraph)
{
    if (!out->len || out->data[out->len - 1] == '\n') return;
    if (paragraph) {
        while (out->len && out->data[out->len - 1] == ' ') out->data[--out->len] = 0;
        append(out, "\n");
    } else if (out->data[out->len - 1] != ' ') append(out, " ");
}

static const char *tag_end(const char *s)
{
    char quote = 0;
    for (; *s; ++s) {
        if (quote) {
            if (*s == quote) quote = 0;
        } else if (*s == '\'' || *s == '"') quote = *s;
        else if (*s == '>') return s + 1;
    }
    return s;
}

static bool in_list(const char *tag, const char *list)
{
    char needle[36];
    snprintf(needle, sizeof needle, "|%s|", tag);
    return strstr(list, needle) != nullptr;
}

void web_readable_html(const char *html, Text *out, size_t limit)
{
    Text article = {0}, fallback = {0};
    struct { char name[24]; bool skip, main; } stack[80];
    size_t depth = 0;
    bool skip = false, main = false;
    size_t ignored_depth = 0;
    for (const char *p = html; *p && (article.len < limit || fallback.len < limit);) {
        if (*p == '<' && (!strncmp(p, "<!--", 4) || p[1] == '!' || p[1] == '?' ||
                         p[1] == '/' || isalpha((unsigned char)p[1]))) {
            if (!strncmp(p, "<!--", 4)) {
                const char *end = strstr(p + 4, "-->");
                if (!end) break;
                p = end + 3;
                continue;
            }
            bool closing = p[1] == '/';
            const char *start = p + (closing ? 2 : 1);
            char tag[24] = {0};
            size_t n = 0;
            while (isalnum((unsigned char)*start) || *start == '-' || *start == ':') {
                if (n < sizeof tag - 1) tag[n++] = (char)tolower((unsigned char)*start);
                start++;
            }
            p = tag_end(start);
            if (!tag[0]) continue;
            bool paragraph = in_list(tag, "|article|main|section|div|p|li|h1|h2|h3|h4|h5|h6|blockquote|pre|br|tr|");
            if (paragraph && !skip && !ignored_depth) {
                visible_space(&fallback, true);
                if (main) visible_space(&article, true);
            }
            if (!closing && in_list(tag, "|script|style|textarea|")) {
                char end_tag[32];
                snprintf(end_tag, sizeof end_tag, "</%s", tag);
                size_t close_len = strlen(end_tag);
                for (; *p; ++p) {
                    if (*p != '<') continue;
                    size_t i = 0;
                    while (i < close_len && tolower((unsigned char)p[i]) == end_tag[i]) ++i;
                    if (i == close_len && (p[i] == '>' || isspace((unsigned char)p[i]))) break;
                }
                p = tag_end(p);
            } else if (closing) {
                if (ignored_depth) { ignored_depth--; continue; }
                for (size_t k = depth; k; k--) {
                    if (strcmp(stack[k - 1].name, tag)) continue;
                    depth = k - 1;
                    skip = stack[depth].skip;
                    main = stack[depth].main;
                    break;
                }
            } else if (!in_list(tag, "|area|base|br|col|embed|hr|img|input|link|meta|param|source|track|wbr|")) {
                if (depth == sizeof stack / sizeof *stack || ignored_depth) { ignored_depth++; continue; }
                memcpy(stack[depth].name, tag, sizeof tag);
                stack[depth].skip = skip;
                stack[depth++].main = main;
                skip |= in_list(tag, "|head|nav|header|footer|aside|form|noscript|svg|canvas|template|table|figure|");
                main |= in_list(tag, "|article|main|");
            }
            continue;
        }
        if (skip || ignored_depth) { ++p; continue; }
        Text *targets[] = {&fallback, &article};
        const char *next = p + 1;
        for (int i = 0; i <= (int)main; i++) {
            Text *target = targets[i];
            if (target->len + 4 > limit) continue;
            unsigned c = (unsigned char)*p;
            if (isspace((int)c)) visible_space(target, false);
            else if (c == '&') {
                next = entity(target, p);
            } else if (c > 31 && c != 127) {
                size_t width = utf8_width((const unsigned char *)p);
                if (width) { append_n(target, p, width); next = p + width; }
            }
        }
        p = next;
    }
    const Text *selected = article.len >= 90 ? &article : &fallback;
    size_t length = selected->len;
    while (length && isspace((unsigned char)selected->data[length - 1])) length--;
    if (length) append_n(out, selected->data, length);
    release(&article);
    release(&fallback);
}

static void encode(Text *out, const char *input)
{
    for (const unsigned char *p = (const unsigned char *)input; *p; ++p)
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || strchr("-._~", *p)) append_n(out, (const char *)p, 1);
        else format(out, "%%%02X", (unsigned)*p);
}

static void add_result(Json *results, const char *engine, const char *url,
                       const char *title, const char *snippet, const char *date)
{
    if (strlen(url) > 1800 || !url_allowed(url, false) || cJSON_GetArraySize(results) >= WEB_MAX_RESULTS) return;
    const Json *item;
    cJSON_ArrayForEach(item, results) if (!strcmp(url, js(item, "url"))) return;
    Json *result = cJSON_CreateObject();
    set(result, "url", url);
    set(result, "engine", engine);
    Text clean = {0};
    web_readable_html(title, &clean, 250);
    set(result, "title", clean.len ? str(&clean) : url);
    clear(&clean);
    web_readable_html(snippet, &clean, 550);
    if (clean.len) set(result, "snippet", str(&clean));
    clear(&clean);
    web_readable_html(date, &clean, 96);
    if (clean.len) set(result, "source_date", str(&clean));
    release(&clean);
    cJSON_AddItemToArray(results, result);
}

static void xml_field(const char *s, const char *name, Text *out)
{
    char start[32], end[32];
    snprintf(start, sizeof start, "<%s>", name);
    snprintf(end, sizeof end, "</%s>", name);
    const char *field = strstr(s, start);
    if (!field) return;
    field += strlen(start);
    const char *close = strstr(field, end);
    if (!close || close - field > 8192) return;
    if (close - field >= 12 && !strncmp(field, "<![CDATA[", 9) && !memcmp(close - 3, "]]>", 3)) {
        field += 9;
        close -= 3;
    }
    Text bounded = {0};
    append_n(&bounded, field, (size_t)(close - field));
    for (const char *p = str(&bounded); *p;) {
        if (*p == '&') p = entity(out, p);
        else {
            size_t n = utf8_width((const unsigned char *)p);
            if (n && (unsigned char)*p >= 32) append_n(out, p, n);
            p += n ? n : 1;
        }
    }
    release(&bounded);
}

static void news_results(const char *rss, Json *results)
{
    const char *p = rss;
    for (int i = 0; i < 4 && (p = strstr(p, "<item>")); ++i) {
        const char *end = strstr(p, "</item>");
        if (!end) break;
        Text item = {0}, title = {0}, url = {0}, date = {0};
        append_n(&item, p, (size_t)(end - p));
        xml_field(str(&item), "title", &title);
        xml_field(str(&item), "link", &url);
        xml_field(str(&item), "pubDate", &date);
        add_result(results, "Google News RSS", str(&url), str(&title), "", str(&date));
        release(&item); release(&title); release(&url); release(&date);
        p = end + 7;
    }
}

static const char *array_string(const Json *array, int index)
{
    const char *value = cJSON_GetStringValue(cJSON_GetArrayItem(array, index));
    return value ? value : "";
}

static void json_results(const char *body, const char *engine, Json *results)
{
    Json *root = cJSON_ParseWithOpts(body, nullptr, true);
    if (!strcmp(engine, "Wikipedia") || !strcmp(engine, "Wiktionary")) {
        const Json *titles = cJSON_GetArrayItem(root, 1), *urls = cJSON_GetArrayItem(root, 3);
        int first = 0, limit = cJSON_GetArraySize(urls);
        if (limit > 3) limit = 3;
        if (!strcmp(engine, "Wiktionary"))
            for (int i = 0; i < limit; i++) {
                if (strcmp(array_string(titles, i), array_string(root, 0))) continue;
                first = i;
                limit = i + 1;
                break;
            }
        for (int i = first; i < limit; i++)
            add_result(results, engine, array_string(urls, i), array_string(titles, i), "", "");
    } else {
        bool searx = !strcmp(engine, "SearXNG");
        const Json *item, *items = at(root, searx ? "results" : "hits");
        int count = 0;
        cJSON_ArrayForEach(item, items) {
            if (count++ >= 4) break;
            add_result(results, engine, js(item, "url"), js(item, "title"),
                       js(item, searx ? "content" : "story_text"), js(item, searx ? "publishedDate" : "created_at"));
        }
    }
    cJSON_Delete(root);
}

static bool word_byte(unsigned char c) { return c >= 128 || isalnum(c); }

static bool term_in_line(const char *line, const char *end, const char *term, size_t length)
{
    for (const char *p = line; end - p >= (ptrdiff_t)length; ++p) {
        if (p != line && word_byte((unsigned char)p[-1])) continue;
        size_t i = 0;
        while (i < length && tolower((unsigned char)p[i]) == tolower((unsigned char)term[i])) ++i;
        if (i == length && (p + i == end || !word_byte((unsigned char)p[i]))) return true;
    }
    return false;
}

static void relevant_excerpt(Text *out, const Text *page, const char *query)
{
    const char *terms[12];
    size_t lengths[12], count = 0;
    for (const char *p = query; *p && count < 12;) {
        while (*p && !word_byte((unsigned char)*p)) ++p;
        const char *term = p;
        while (word_byte((unsigned char)*p)) ++p;
        if (p - term >= 3) {
            terms[count] = term;
            lengths[count++] = (size_t)(p - term);
        }
    }
    const char *best = str(page);
    int best_score = 0;
    for (const char *line = best; count && *line;) {
        const char *end = strchr(line, '\n');
        if (!end) end = line + strlen(line);
        if (end - line >= 85) {
            int score = 0;
            for (size_t i = 0; i < count; ++i) score += term_in_line(line, end, terms[i], lengths[i]);
            if (score > best_score) {
                best = line;
                best_score = score;
            }
        }
        line = *end ? end + 1 : end;
    }
    size_t length = strlen(best);
    if (length > WEB_EXCERPT) {
        length = WEB_EXCERPT;
        while (((unsigned char)best[length] & 0xc0) == 0x80) --length;
        const char *break_at = best + length;
        while (break_at > best + length / 2 && !isspace((unsigned char)break_at[-1])) --break_at;
        if (break_at > best + length / 2) length = (size_t)(break_at - best);
    }
    while (length && isspace((unsigned char)best[length - 1])) --length;
    if (length) append_n(out, best, length);
}

static bool crawl(const char *url, const char *query, const atomic_bool *cancel, Json *result)
{
    Document doc = {0};
    bool ok = download(url, false, cancel, &doc);
    if (ok && (doc.html || doc.plain)) {
        Text page = {0}, excerpt = {0};
        web_readable_html(str(&doc.body), &page, 60000);
        relevant_excerpt(&excerpt, &page, query);
        ok = excerpt.len > 0;
        if (ok) set(result, "excerpt", str(&excerpt));
        release(&excerpt);
        release(&page);
    } else ok = false;
    if (!ok) {
        if (doc.status >= 300 && doc.status < 400) set(result, "crawl", "Redirect chain did not resolve to a public HTTPS page.");
        else set(result, "crawl", "Readable source text unavailable.");
    }
    if (ok && strcmp(url, str(&doc.final_url))) set(result, "resolved_url", str(&doc.final_url));
    release(&doc.body);
    release(&doc.final_url);
    return ok;
}

Json *web_wiktionary_english_senses(const char *json)
{
    Json *root = cJSON_ParseWithOpts(json, nullptr, true);
    Json *senses = cJSON_CreateArray();
    const Json *english = at(root, "en"), *group;
    if (!cJSON_IsArray(english)) english = nullptr;
    cJSON_ArrayForEach(group, english) {
        const Json *definitions = at(group, "definitions"), *item;
        if (!cJSON_IsArray(definitions)) continue;
        cJSON_ArrayForEach(item, definitions) {
            Text definition = {0}, part = {0};
            web_readable_html(js(item, "definition"), &definition, 512);
            if (definition.len && definition.len < 500) {
                web_readable_html(js(group, "partOfSpeech"), &part, 48);
                Json *sense = cJSON_CreateObject();
                set(sense, "definition", str(&definition));
                if (part.len) set(sense, "part_of_speech", str(&part));
                cJSON_AddItemToArray(senses, sense);
            }
            release(&definition);
            release(&part);
            if (cJSON_GetArraySize(senses) >= 6) break;
        }
        if (cJSON_GetArraySize(senses) >= 6) break;
    }
    cJSON_Delete(root);
    return senses;
}

static void crawl_dictionary(Json *result, const atomic_bool *cancel)
{
    const char *title = js(result, "title");
    Text url = {0};
    append(&url, "https://en.wiktionary.org/api/rest_v1/page/definition/");
    encode(&url, title);
    Document doc = {0};
    if (download(str(&url), false, cancel, &doc)) {
        Json *senses = web_wiktionary_english_senses(str(&doc.body));
        if (senses->child) {
            put(result, "english_senses", senses);
            Text excerpt = {0};
            const Json *sense;
            int index = 0;
            cJSON_ArrayForEach(sense, senses) {
                format(&excerpt, "%d. %s\n", ++index, js(sense, "definition"));
            }
            set(result, "excerpt", str(&excerpt));
            set(result, "definition_source", "Wiktionary structured definitions API");
            release(&excerpt);
        } else cJSON_Delete(senses);
    }
    if (!at(result, "english_senses")) set(result, "crawl", "Verified English definitions unavailable.");
    release(&doc.body);
    release(&doc.final_url);
    release(&url);
}

static int days(const char *range)
{
    if (!strcmp(range, "day")) return 1;
    if (!strcmp(range, "week")) return 7;
    if (!strcmp(range, "month")) return 30;
    if (!strcmp(range, "year")) return 365;
    return 0;
}

static bool source_timestamp(const char *date, char iso[21])
{
    int year, month, day, hour, minute, second;
    char suffix[6] = {0};
    int n = 0;
    if (sscanf(date, "%4d-%2d-%2dT%2d:%2d:%2d%n",
               &year, &month, &day, &hour, &minute, &second, &n) == 6) {
        const char *zone = date + n;
        if (*zone == '.') while (isdigit((unsigned char)*++zone)) {}
        if (strcmp(zone, "Z") && strcmp(zone, "+00:00")) return false;
    } else {
        static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                             "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        const char *comma = strchr(date, ',');
        char word[4];
        if (!comma || sscanf(comma + 1, "%2d %3s %4d %2d:%2d:%2d %5s",
                            &day, word, &year, &hour, &minute, &second, suffix) != 7 ||
            (strcmp(suffix, "GMT") && strcmp(suffix, "+0000"))) return false;
        month = 1;
        while (month <= 12 && strcmp(word, months[month - 1])) ++month;
    }
    if (year < 1970 || year > 9999 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60) return false;
    return snprintf(iso, 21, "%04d-%02d-%02dT%02d:%02d:%02dZ",
                    year, month, day, hour, minute, second) == 20;
}

static bool format_utc(time_t stamp, char iso[21])
{
    struct tm utc;
#ifdef _WIN32
    if (gmtime_s(&utc, &stamp)) return false;
#else
    if (!gmtime_r(&stamp, &utc)) return false;
#endif
    return strftime(iso, 21, "%Y-%m-%dT%H:%M:%SZ", &utc) == 20;
}

static void search(const char *query, const char *range, const char *only,
                   const atomic_bool *cancel, Json *results, Json *errors)
{
    Text encoded = {0}, url = {0};
    encode(&encoded, query);
    const char *configured = env("NEUTRINO_SEARXNG_URL", "");
    const char *const engines[] = {"SearXNG", "Google News RSS", "Wikipedia", "Wiktionary", "Hacker News"};
    bool hourly = !strcmp(range, "hour");
    int age = hourly ? 1 : days(range);
    time_t now = time(nullptr);
    for (int i = 0; i < 5 && !cancelled(cancel); i++) {
        if ((*only && strcmp(only, engines[i])) || (!i && !*configured) || ((i == 2 || i == 3) && age)) continue;
        clear(&url);
        if (i == 0) {
            format(&url, "%s%sformat=json&q=%s", configured, strchr(configured, '?') ? "&" : "?", str(&encoded));
            if (age) format(&url, "&time_range=%s", hourly ? "day" : range);
        } else if (i == 1) {
            format(&url, "https://news.google.com/rss/search?hl=en-US&gl=US&ceid=US%%3Aen&q=%s", str(&encoded));
            if (hourly) append(&url, "%20when%3A1h");
            else if (age) format(&url, "%%20when%%3A%dd", age);
        } else if (i == 2 || i == 3) {
            format(&url, "https://en.%s.org/w/api.php?action=opensearch&namespace=0&limit=%d&format=json&search=%s",
                   i == 2 ? "wikipedia" : "wiktionary", i == 2 || *only ? 3 : 1, str(&encoded));
        } else {
            format(&url, "https://hn.algolia.com/api/v1/%s?tags=story&hitsPerPage=4&query=%s",
                   age ? "search_by_date" : "search", str(&encoded));
            if (age) format(&url, "&numericFilters=created_at_i%%3E%lld",
                            (long long)now - (hourly ? 3600LL : age * 86400LL));
        }
        Document doc = {0};
        if (download(str(&url), !i, cancel, &doc)) {
            if (i == 1) news_results(str(&doc.body), results);
            else json_results(str(&doc.body), engines[i], results);
        } else cJSON_AddItemToArray(errors, cJSON_CreateString(engines[i]));
        release(&doc.body);
        release(&doc.final_url);
    }
    release(&encoded); release(&url);
    if (hourly) {
        char cutoff[21], upper[21], dated[21];
        bool have_clock = format_utc(now - 3600, cutoff) && format_utc(now, upper);
        Json *next;
        for (Json *item = results->child; item; item = next) {
            next = item->next;
            if (!have_clock || !source_timestamp(js(item, "source_date"), dated) ||
                strcmp(dated, cutoff) < 0 || strcmp(dated, upper) > 0)
                cJSON_Delete(cJSON_DetachItemViaPointer(results, item));
        }
    }
    // Use direct publisher links first. RSS news links commonly require a script or redirect.
    int fetched = 0;
    Json *result;
    cJSON_ArrayForEach(result, results) {
        if (fetched >= 3 || cancelled(cancel)) break;
        if (!strcmp(js(result, "engine"), "Google News RSS")) {
            set(result, "crawl", "Feed metadata only. Use the publisher's direct URL for page text.");
            continue;
        }
        if (!strcmp(js(result, "engine"), "Wiktionary")) crawl_dictionary(result, cancel);
        else crawl(js(result, "url"), query, cancel, result);
        fetched++;
    }
}

void web_research(const char *mode, const Json *request, const atomic_bool *cancel, Text *out)
{
    Json *report = cJSON_CreateObject(), *results = cJSON_CreateArray(), *errors = cJSON_CreateArray();
    put(report, "sources", results);
    put(report, "unavailable_engines", errors);
    const char *value = js(request, "value"), *range = js(request, "range");
    const char *engine = js(request, "engine");
    const char *only = !strcmp(engine, "wiktionary") ? "Wiktionary" :
                       !strcmp(engine, "wikipedia") ? "Wikipedia" : "";
    char fetched[21];
    if (format_utc(time(nullptr), fetched)) set(report, "fetched_at", fetched);
    if (!strcmp(mode, "web-read") && strlen(value) <= 1800 && url_allowed(value, false)) {
        add_result(results, "Direct", value, value, "", "");
        const char *slug = strrchr(value, '/');
        if (!crawl(value, slug ? slug + 1 : "", cancel, results->child)) set(report, "error", "Page text unavailable.");
    } else if (!strcmp(mode, "web-search") && *value && strlen(value) <= WEB_QUERY_MAX &&
               (!*range || days(range) || !strcmp(range, "hour"))) {
        set(report, "query", value);
        if (*range) set(report, "range", range);
        if (*only) set(report, "engine", only);
        search(value, range, only, cancel, results, errors);
        if (!results->child && !strcmp(only, "Wikipedia") && !cancelled(cancel)) {
            set(report, "engine", "General search");
            search(value, range, "", cancel, results, errors);
        }
        if (!results->child) set(report, "error", "No sources found for this query.");
    } else set(report, "error", "Use a search query up to 400 bytes, or an HTTPS source URL on port 443.");
    Json *source;
    int id = 0;
    cJSON_ArrayForEach(source, results) integer(source, "id", ++id);
    char *json = cJSON_PrintUnformatted(report);
    if (!json) abort();
    append(out, json);
    cJSON_free(json);
    cJSON_Delete(report);
}

static bool citation_url(const char *url)
{
    return !strncmp(url, "https://", 8) && url[8] && !strpbrk(url, " \t\r\n<>");
}

static bool reply_contains_url(const char *reply, const char *url)
{
    size_t n = strlen(url);
    for (const char *p = reply; (p = strstr(p, url)); p += n) {
        unsigned char next = (unsigned char)p[n];
        if (next == '.' || next == ',') next = (unsigned char)p[n + 1];
        if (!next || isspace(next) || strchr(">)\"]'`", next)) return true;
    }
    return false;
}

void web_cite_reply(const char *report, Text *reply, Text *trace)
{
    if (!*report) return;
    Json *data = cJSON_ParseWithOpts(report, nullptr, true);
    const Json *sources = at(data, "sources"), *source;
    bool heading = false;
    cJSON_ArrayForEach(source, sources) {
        const char *url = js(source, "url"), *resolved = js(source, "resolved_url");
        const char *used = citation_url(url) && reply_contains_url(str(reply), url) ? url :
                           citation_url(resolved) && reply_contains_url(str(reply), resolved) ? resolved : nullptr;
        if (!used) continue;
        if (!heading) append(trace, "\nWeb citations in the narrator's reply\n");
        heading = true;
        format(trace, "%s\n", used);
    }
    if (!heading) {
        int count = 0;
        bool dated = *js(data, "range");
        for (int pass = 0; pass < 2 && count < 3; pass++) {
            cJSON_ArrayForEach(source, sources) {
                const char *url = js(source, "resolved_url");
                if (!citation_url(url)) url = js(source, "url");
                bool preferred = dated ? *js(source, "source_date") : *js(source, "excerpt");
                if (preferred != (pass == 0) || !citation_url(url) || reply_contains_url(str(reply), url)) continue;
                if (!count++) {
                    append(reply, "\n\nSources supplied for this reply\n");
                    append(trace, "\nWeb citations appended because the narrator omitted source URLs\n");
                }
                format(reply, "- %s\n", url);
                format(trace, "%s\n", url);
                if (count == 3) break;
            }
        }
    }
    cJSON_Delete(data);
}
Json *web_research_request(const char *kind, const char *query)
{
    if (!*query || strlen(query) > 160 || strpbrk(query, "\r\n\t?") || strstr(query, "://") ||
        (strcmp(kind, "news") && strcmp(kind, "reference") && strcmp(kind, "dictionary"))) return nullptr;
    Json *request = cJSON_CreateObject();
    set(request, "value", query);
    if (!strcmp(kind, "news")) set(request, "range", "day");
    else set(request, "engine", !strcmp(kind, "dictionary") ? "wiktionary" : "wikipedia");
    return request;
}

static bool cue_has(const char *cue, const char *word)
{
    return term_in_line(cue, cue + strlen(cue), word, strlen(word));
}

static Json *request_span(const char *kind, const char *start, size_t length)
{
    while (length && isspace((unsigned char)*start)) { ++start; --length; }
    while (length && (isspace((unsigned char)start[length - 1]) ||
                      strchr(".?!,;:", start[length - 1]))) --length;
    char query[161];
    if (!length || length >= sizeof query) return nullptr;
    memcpy(query, start, length);
    query[length] = 0;
    return web_research_request(kind, query);
}

Json *web_auto_request(const char *cue, bool *consider)
{
    *consider = false;
    size_t n = strlen(cue);
    if (!n || n > 600 || strpbrk(cue, "\r\n") || strstr(cue, "neutrino_tool")) return nullptr;
    char lower[601];
    for (size_t i = 0; i <= n; ++i) lower[i] = (char)tolower((unsigned char)cue[i]);
    bool lookup = cue_has(lower, "lookup") || cue_has(lower, "look up") || cue_has(lower, "research");
    bool question = strchr(cue, '?') && (cue_has(lower, "what") || cue_has(lower, "who") ||
        cue_has(lower, "when") || cue_has(lower, "where") || cue_has(lower, "which") ||
        cue_has(lower, "why") || cue_has(lower, "how"));
    bool dictionary = cue_has(lower, "define") || cue_has(lower, "wiktionary") || cue_has(lower, "dictionary") ||
        (cue_has(lower, "word") && (cue_has(lower, "meaning") || cue_has(lower, "mean")));
    bool news = cue_has(lower, "headlines") || cue_has(lower, "news") || cue_has(lower, "latest") ||
        cue_has(lower, "recently") || cue_has(lower, "today");
    bool reference = cue_has(lower, "meme") || cue_has(lower, "episode") || cue_has(lower, "film") ||
        cue_has(lower, "movie") || cue_has(lower, "anime") || cue_has(lower, "song") ||
        cue_has(lower, "game") || cue_has(lower, "reference") || cue_has(lower, "pop culture");
    *consider = question || lookup || dictionary;
    if (!*consider) return nullptr;
    const char *kind = dictionary ? "dictionary" : news ? "news" : "reference";
    if (dictionary || news || reference) {
        const char *quoted = strchr(cue, '"');
        char delimiter = '"';
        if (!quoted) { quoted = strstr(cue, " '"); if (quoted) ++quoted; delimiter = '\''; }
        const char *end = quoted ? strchr(quoted + 1, delimiter) : nullptr;
        if (end) return request_span(kind, quoted + 1, (size_t)(end - quoted - 1));
    }
    if (dictionary) {
        const char *term = strstr(lower, "word ");
        if (term) term += 5;
        else if ((term = strstr(lower, "define "))) term += 7;
        if (term) {
            const char *end = term;
            while (word_byte((unsigned char)*end) || *end == '-') ++end;
            if (end != term) return request_span(kind, cue + (term - lower), (size_t)(end - term));
        }
    }
    if (news) {
        const char *subject = strstr(lower, " about ");
        if (subject) subject += 7;
        else if ((subject = strstr(lower, " on "))) subject += 4;
        if (subject) {
            const char *end = strpbrk(subject, "?.!;");
            return request_span(kind, cue + (subject - lower), end ? (size_t)(end - subject) : strlen(subject));
        }
    }
    if (question && !news && !dictionary) {
        const char *const creator_verbs[] = {" created ", " wrote ", " directed ", " developed ",
                                             " published ", " recorded ", " voiced "};
        for (size_t i = 0; i < sizeof creator_verbs / sizeof *creator_verbs; ++i) {
            const char *subject = strstr(lower, creator_verbs[i]);
            if (!subject) continue;
            subject += strlen(creator_verbs[i]);
            const char *end = strchr(subject, '?');
            const char *named = cue + (subject - lower);
            if (end && (reference || isupper((unsigned char)*named)))
                return request_span("reference", named, (size_t)(end - subject));
        }
    }
    return nullptr;
}

static const char *source_url(const Json *source)
{
    const char *url = js(source, "resolved_url");
    if (!citation_url(url)) url = js(source, "url");
    return citation_url(url) ? url : "";
}

static const Json *best_source(const Json *sources, const char *reply)
{
    const Json *best = nullptr, *source;
    int best_score = -1;
    const char *end = reply + strlen(reply);
    cJSON_ArrayForEach(source, sources) {
        if (!*source_url(source)) continue;
        int score = *js(source, "excerpt") ? 2 : 0;
        for (const char *p = js(source, "title"); *p;) {
            while (*p && !word_byte((unsigned char)*p)) ++p;
            const char *start = p;
            while (word_byte((unsigned char)*p)) ++p;
            if (p - start >= 5 && term_in_line(reply, end, start, (size_t)(p - start))) score += 3;
        }
        if (score > best_score) {
            best = source;
            best_score = score;
        }
    }
    return best;
}

static bool already_inline(const char *text, const char *url)
{
    if (!reply_contains_url(text, url)) return false;
    size_t n = strlen(url);
    for (const char *match = text; (match = strstr(match, url)); match += n) {
        const char *line = match;
        while (line > text && line[-1] != '\n') --line;
        while (line < match && strchr(" \t-*+", *line)) ++line;
        if (*line == '[' || !strncmp(line, "Source:", 7) || !strncmp(line, "Reference:", 10)) continue;
        for (const char *p = line; p < match; ++p)
            if (isalnum((unsigned char)*p)) return true;
    }
    return false;
}

void web_cite_inline(const char *report, Text *reply, Text *trace)
{
    Json *data = cJSON_ParseWithOpts(report, nullptr, true);
    const Json *sources = at(data, "sources"), *source;
    Text revised = {0};
    int linked = 0;
    const char *start = str(reply), *scan = start, *marker;
    while ((marker = strstr(scan, "[["))) {
        scan = marker + 2;
        char *end = nullptr;
        long id = strtol(scan, &end, 10);
        if (!isdigit((unsigned char)*scan) || strncmp(end, "]]", 2)) continue;
        const Json *item = id > 0 && id <= cJSON_GetArraySize(sources) ?
                           cJSON_GetArrayItem(sources, (int)id - 1) : nullptr;
        const char *url = source_url(item);
        append_n(&revised, start, (size_t)(marker - start));
        if (*url) {
            const char *line = marker;
            while (line > str(reply) && (line[-1] == ' ' || line[-1] == '\t')) --line;
            if (line > str(reply) && line[-1] == '\n') {
                while (revised.len && isspace((unsigned char)revised.data[revised.len - 1]))
                    revised.data[--revised.len] = 0;
                if (revised.len) append(&revised, " ");
            }
            format(&revised, "([%ld](<%s>))", id, url);
            format(trace, "\nInline web citation %ld resolved to %s\n", id, url);
            linked++;
        }
        start = scan = end + 2;
    }
    append(&revised, start);
    if (strcmp(str(&revised), str(reply))) {
        release(reply);
        *reply = revised;
    } else release(&revised);
    if (linked) { cJSON_Delete(data); return; }
    cJSON_ArrayForEach(source, sources) {
        const char *url = source_url(source);
        const char *original = js(source, "url");
        if ((*url && already_inline(str(reply), url)) ||
            (citation_url(original) && already_inline(str(reply), original))) {
            format(trace, "\nInline web citation written by narrator: %s\n",
                   *url && already_inline(str(reply), url) ? url : original);
            cJSON_Delete(data);
            return;
        }
    }
    const char *url = source_url(best_source(sources, str(reply)));
    if (*url) {
        Text cited = {0};
        const char *body = str(reply), *first = body;
        while (isspace((unsigned char)*first)) ++first;
        const char *paragraph = strstr(first, "\n\n");
        const char *end = paragraph ? paragraph : body + reply->len;
        while (end > body && isspace((unsigned char)end[-1])) --end;
        append_n(&cited, body, (size_t)(end - body));
        format(&cited, " ([Reference](<%s>))", url);
        append(&cited, end);
        release(reply);
        *reply = cited;
        format(trace, "\nInline web citation supplied by Neutrino: %s\n", url);
    }
    cJSON_Delete(data);
}

bool web_ground_definition(const char *report, Text *reply, Text *trace)
{
    Json *data = cJSON_ParseWithOpts(report, nullptr, true);
    if (strcmp(js(data, "engine"), "Wiktionary")) {
        cJSON_Delete(data);
        return false;
    }
    const Json *sources = at(data, "sources"), *source;
    int index = 0;
    cJSON_ArrayForEach(source, sources) {
        ++index;
        if (!strcmp(js(source, "title"), js(data, "query")) && *source_url(source)) break;
    }
    const Json *senses = at(source, "english_senses");
    int count = cJSON_GetArraySize(senses);
    if (!count) {
        clear(reply);
        append(reply, "I couldn’t verify an English definition for that exact term in Wiktionary.");
        append(trace, "\nWiktionary returned no verified English senses for the exact headword.\n");
        cJSON_Delete(data);
        return true;
    }
    const char *choice = str(reply);
    char *end;
    long selected = strtol(choice, &end, 10);
    bool valid = end != choice && selected >= 1 && selected <= count;
    while (isspace((unsigned char)*end)) ++end;
    valid = valid && !*end;
    if (!valid) selected = 1;
    const Json *sense = cJSON_GetArrayItem(senses, (int)selected - 1);
    const char *part = js(sense, "part_of_speech");
    clear(reply);
    format(reply, "**%s**", js(source, "title"));
    if (*part) format(reply, " (%s)", part);
    format(reply, " — %s [[%d]]", js(sense, "definition"), index);
    format(trace, "\nPublished Wiktionary English sense %ld %s\n%s\n",
           selected, valid ? "selected by the narrator" : "used after an invalid narrator selection",
           js(sense, "definition"));
    cJSON_Delete(data);
    web_cite_inline(report, reply, trace);
    return true;
}
