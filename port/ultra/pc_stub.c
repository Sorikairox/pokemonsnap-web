/* PC layer: bookkeeping for the generated logged stubs (port/build/gen/stubs.c, port/tools/gen_stubs.py). Every stub
 * calls port_stub_hit(name): the first call of each stub is logged, all calls are counted; port_stub_report() lists
 * the stubs hit so far (exported, for the test runner). Session S2. */
#include "pc_mem.h"
#include <string.h>

#define STUB_SLOTS 4096
static struct { const char *name; unsigned hits; } slots[STUB_SLOTS];
static unsigned used;

void port_stub_hit(const char *name) {
    unsigned h = 2166136261u;
    for (const char *p = name; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
    for (unsigned i = 0; i < STUB_SLOTS; i++) {
        unsigned k = (h + i) & (STUB_SLOTS - 1);
        if (!slots[k].name) {
            slots[k].name = name;
            slots[k].hits = 1;
            used++;
            port_log("[stub] %s (first call)\n", name);
            return;
        }
        if (slots[k].name == name || !strcmp(slots[k].name, name)) { slots[k].hits++; return; }
    }
}

int port_stub_report(void) {
    port_log("[stub] %u different stubs called:\n", used);
    for (unsigned i = 0; i < STUB_SLOTS; i++)
        if (slots[i].name) port_log("[stub]   %-40s %u\n", slots[i].name, slots[i].hits);
    return (int)used;
}
