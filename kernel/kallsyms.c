#include "kallsyms.h"
#include <stddef.h>

extern const ksym_t kallsyms_table[];
extern const int kallsyms_count;

const char *kallsyms_lookup(uint32_t addr) {
    if (kallsyms_count <= 0)
        return NULL;

    int lo = 0, hi = kallsyms_count - 1;
    int best = -1;

    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (kallsyms_table[mid].addr <= addr) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }

    if (best < 0)
        return NULL;

    return kallsyms_table[best].name;
}
