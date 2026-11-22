#include "web.h"

#include <stdio.h>
#include <string.h>

static bool citation_check(void)
{
    const char *report = "{\"range\":\"day\",\"sources\":["
                         "{\"url\":\"https://example.org/reference\",\"excerpt\":\"An older reference.\"},"
                         "{\"url\":\"https://example.org/news\",\"source_date\":\"2026-11-22\"},"
                         "{\"url\":\"https://example.org/news\"},"
                         "{\"url\":\"file:///local\"}]}";
    Text reply = {0}, trace = {0};
    append(&reply, "The article reports a change. See https://example.org/newsroom.");
    web_cite_reply(report, &reply, &trace);
    const char *news = strstr(str(&reply), "- https://example.org/news\n");
    const char *reference = strstr(str(&reply), "- https://example.org/reference\n");
    bool added = news && reference && news < reference && !strstr(news + 2, "- https://example.org/news\n") &&
        !strstr(str(&reply), "file:///") && strstr(str(&trace), "Web citations appended");
    clear(&reply);
    clear(&trace);
    append(&reply, "Read [the report](https://example.org/news).");
    web_cite_reply(report, &reply, &trace);
    bool unchanged = !strcmp(str(&reply), "Read [the report](https://example.org/news).") &&
        strstr(str(&trace), "Web citations in the narrator's reply");
    clear(&reply);
    clear(&trace);
    append(&reply, "News at `https://example.org/news`.");
    web_cite_reply(report, &reply, &trace);
    bool backtick = !strcmp(str(&reply), "News at `https://example.org/news`.") &&
        strstr(str(&trace), "Web citations in the narrator's reply");
    clear(&reply);
    clear(&trace);
    web_cite_reply("", &reply, &trace);
    bool empty = !reply.len && !trace.len;
    release(&reply);
    release(&trace);
    return added && unchanged && backtick && empty;
}

int main(void)
{
    Text out = {0};
    web_readable_html(
        "<header><nav>Navigation that must disappear</nav></header>"
        "<main><h1>Fresh news</h1><script>privatePayload()</script>"
        "<p>Today &amp; tomorrow &#x1F30F; we have an example article with enough text to"
        " distinguish it from surrounding page boilerplate.</p>"
        "<!-- hidden comment --><p title='A > B'>Article ends here.</p>"
        "<aside>Sidebar that must disappear</aside></main>"
        "<div>Unrelated footer-like tail.</div>", &out, 1000);
    const bool readable = strstr(str(&out), "Fresh news\nToday & tomorrow 🌏") &&
        strstr(str(&out), "Article ends here.") && !strstr(str(&out), "privatePayload") &&
        !strstr(str(&out), "Navigation") && !strstr(str(&out), "hidden comment") &&
        !strstr(str(&out), "Sidebar") && !strstr(str(&out), "Unrelated");
    clear(&out);
    web_readable_html("<p>Fallback &quot;plain&quot; caf&#233;.</p>", &out, 128);
    const bool fallback = !strcmp(str(&out), "Fallback \"plain\" café.");
    clear(&out);
    web_readable_html("<p>Déjà vu and more text than fits here.</p>", &out, 12);
    const bool bounded = out.len <= 12 && strstr(str(&out), "Déjà");
    release(&out);
    if (!readable || !fallback || !bounded) fputs("Headless reader extraction failed.\n", stderr);
    const bool citations = citation_check();
    if (!citations) fputs("Saved replies must preserve or supply exact research source URLs.\n", stderr);
    return readable && fallback && bounded && citations ? 0 : 1;
}
