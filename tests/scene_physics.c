#include "app.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
    const struct {
        const char *name, *json, *expected;
    } cases[] = {
        {"ray", "{\"origin\":[0,0,0],\"direction\":[3,0,0],\"min\":[2,-1,-1],\"max\":[3,1,1]}", "hits the box at 2 m"},
        {"ray", "{\"origin\":[0,2,0],\"direction\":[1,0,0],\"min\":[2,-1,-1],\"max\":[3,1,1]}", "misses the box"},
        {"ray", "{\"origin\":[0,0,0],\"direction\":[0,0,0],\"min\":[2,-1,-1],\"max\":[3,1,1]}", nullptr},
        {"fluid", "{\"height\":1,\"tank_area\":1,\"outlet_area\":0,\"seconds\":10}", "drained 0 L"},
        {"fluid", "{\"height\":1,\"tank_area\":1,\"outlet_area\":0.01,\"seconds\":1000}", "drained 1000 L"},
        {"gas", "{\"moles\":10,\"volume\":1,\"kelvin\":300,\"leak\":0.2,\"seconds\":10}", "8 mol remaining"},
        {"gas", "{\"moles\":10,\"volume\":0,\"kelvin\":300,\"leak\":0,\"seconds\":0}", nullptr},
    };
    int failures = 0;
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        Json *args = cJSON_Parse(cases[i].json);
        Text result = {0};
        bool ok = args && physics_run(cases[i].name, args, &result);
        if (ok != (cases[i].expected != nullptr) ||
            (ok && !strstr(str(&result), cases[i].expected))) {
            fprintf(stderr, "%s test %zu gave: %s\n", cases[i].name, i + 1, str(&result));
            failures++;
        }
        cJSON_Delete(args);
        release(&result);
    }
    return failures != 0;
}
