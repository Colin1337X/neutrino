#include "web.h"

#include <stdio.h>
#include <string.h>

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
    return readable && fallback && bounded ? 0 : 1;
}
