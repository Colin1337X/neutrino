#include "app.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

void forget_secret(Text *secret)
{
    volatile char *data = secret->data;
    for (size_t i = 0; i < secret->cap; ++i) data[i] = 0;
    release(secret);
}

bool session_secret(const char *value, bool optional)
{
    if (!*value) return optional;
    if (strlen(value) > 256) return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
        if (*p < 33 || *p > 126) return false;
    return true;
}

bool session_secret_matches(const char *actual, const char *expected)
{
    size_t length = strlen(expected);
    if (!length || strlen(actual) != length) return false;
    unsigned diff = 0;
    for (size_t i = 0; i < length; ++i) diff |= (unsigned char)(actual[i] ^ expected[i]);
    return !diff;
}

bool session_host(const char *url)
{
    if (strlen(url) > 1024 || strpbrk(url," \\?#@\t\r\n")) return false;
    CURLU *parsed = curl_url();
    if (!parsed) return false;
    char *scheme = nullptr, *host = nullptr, *path = nullptr;
    bool valid = !curl_url_set(parsed,CURLUPART_URL,url,0) &&
        !curl_url_get(parsed,CURLUPART_SCHEME,&scheme,0) &&
        !curl_url_get(parsed,CURLUPART_HOST,&host,0) &&
        !curl_url_get(parsed,CURLUPART_PATH,&path,0) &&
        (!strcmp(scheme,"https") || (!strcmp(scheme,"http") &&
            (!strcmp(host,"127.0.0.1") || !strcmp(host,"localhost") || !strcmp(host,"[::1]")))) &&
        (!strcmp(path,"/") || !strcmp(path,"/v1") || !strcmp(path,"/v1/"));
    curl_free(scheme);
    curl_free(host);
    curl_free(path);
    curl_url_cleanup(parsed);
    return valid;
}

bool session_frame_safe(const char *frame)
{
    if (strncmp(frame,"\x1b[?25l",6)) return false;
    const unsigned char *p = (const unsigned char *)frame;
    while (*p) {
        if (*p == 27) {
            if (*++p != '[') return false;
            const unsigned char *start = ++p;
            while ((*p >= '0' && *p <= '9') || *p == ';' || *p == '?') {
                if (++p - start > 32) return false;
            }
            size_t length = (size_t)(p-start);
            if (*p == 'h' || *p == 'l') {
                if (length != 3 || memcmp(start,"?25",3)) return false;
            } else if (*p == 'J') {
                if (length != 1 || *start != '2') return false;
            } else if ((*p != 'm' && *p != 'H') || memchr(start,'?',length)) return false;
        } else if (*p < 32 || *p == 127) return false;
        else if (*p >= 128) {
            unsigned remaining = *p >= 0xf0 && *p <= 0xf4 ? 3 : *p >= 0xe0 && *p < 0xf0 ? 2 :
                                 *p >= 0xc2 && *p < 0xe0 ? 1 : 0;
            if (!remaining || (*p == 0xc2 && p[1] < 0xa0) || (*p == 0xe0 && p[1] < 0xa0) ||
                (*p == 0xed && p[1] >= 0xa0) || (*p == 0xf0 && p[1] < 0x90) ||
                (*p == 0xf4 && p[1] >= 0x90)) return false;
            while (remaining--) if ((*++p & 0xc0) != 0x80) return false;
        }
        ++p;
    }
    return true;
}

static size_t receive_session(char *data, size_t size, size_t count, void *context)
{
    Text *output = context;
    size_t length = size * count;
    if (length > FILE_LIMIT - output->len || memchr(data,0,length)) return 0;
    append_n(output,data,length);
    return length;
}

int session_http(const char *url, const char *path, const char *password, const char *admin,
                 const Text *input, Text *output)
{
    CURL *curl = curl_easy_init();
    if (!curl) return 0;
    Text target = {0}, authorization = {0}, elevation = {0};
    size_t length = strlen(url);
    while (length && url[length-1] == '/') --length;
    append_n(&target,url,length);
    append(&target,path);
    struct curl_slist *headers = nullptr;
    if (*password) format(&authorization,"Authorization: Bearer %s",password);
    if (*admin) format(&elevation,"X-Neutrino-Admin: %s",admin);
    const char *fields[] = {"X-Neutrino-Web: 1", "Content-Type: application/json", "Expect:",
                            str(&authorization), str(&elevation)};
    for (size_t i = 0; i < sizeof fields / sizeof *fields; ++i) {
        if (!*fields[i]) continue;
        struct curl_slist *next = curl_slist_append(headers,fields[i]);
        if (!next) abort();
        headers = next;
    }
    curl_easy_setopt(curl,CURLOPT_URL,str(&target));
    curl_easy_setopt(curl,CURLOPT_HTTP_VERSION,CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
    curl_easy_setopt(curl,CURLOPT_DISALLOW_USERNAME_IN_URL,1L);
    curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,0L);
    curl_easy_setopt(curl,CURLOPT_HTTPAUTH,(long)CURLAUTH_NONE);
    curl_easy_setopt(curl,CURLOPT_PROXYAUTH,(long)CURLAUTH_NONE);
    curl_easy_setopt(curl,CURLOPT_NOPROXY,"127.0.0.1,localhost,::1");
    curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT_MS,2500L);
    curl_easy_setopt(curl,CURLOPT_TIMEOUT_MS,8000L);
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,receive_session);
    curl_easy_setopt(curl,CURLOPT_WRITEDATA,output);
    if (input) {
        curl_easy_setopt(curl,CURLOPT_POSTFIELDS,str(input));
        curl_easy_setopt(curl,CURLOPT_POSTFIELDSIZE_LARGE,(curl_off_t)input->len);
    }
    long status = 0;
    CURLcode result = curl_easy_perform(curl);
    if (result == CURLE_OK) curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status);
    curl_easy_cleanup(curl);
    for (struct curl_slist *header = headers; header; header = header->next) {
        volatile char *data = header->data;
        for (size_t i = 0, n = strlen(header->data); i < n; ++i) data[i] = 0;
    }
    curl_slist_free_all(headers);
    release(&target);
    forget_secret(&authorization);
    forget_secret(&elevation);
    return (int)status;
}
