#ifndef NEUTRINO_WEB_H
#define NEUTRINO_WEB_H

#include "neutrino.h"
#include <stdatomic.h>

void web_research(const char *mode, const Json *request, const atomic_bool *cancel, Text *out);
void web_readable_html(const char *html, Text *out, size_t limit);

#endif
