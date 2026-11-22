#ifndef NEUTRINO_WEB_H
#define NEUTRINO_WEB_H

#include "neutrino.h"
#include <stdatomic.h>

void web_research(const char *mode, const Json *request, const atomic_bool *cancel, Text *out);
void web_readable_html(const char *html, Text *out, size_t limit);
void web_cite_reply(const char *report, Text *reply, Text *trace);
void web_cite_inline(const char *report, Text *reply, Text *trace);
Json *web_research_request(const char *kind, const char *query);
Json *web_auto_request(const char *cue, bool *consider);

#endif
