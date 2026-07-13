#ifndef CPUID_H
#define CPUID_H

#include <stdint.h>

#define CPUID_FEATURE_APIC      (1 << 9)
#define CPUID_FEATURE_MSR       (1 << 5)
#define CPUID_FEATURE_XAPIC     (1 << 21)
#define CPUID_FEATURE_X2APIC    (1 << 21)

static inline void cpuid(uint32_t eax, uint32_t *eax_out, uint32_t *ebx_out, uint32_t *ecx_out, uint32_t *edx_out) {
    uint32_t a, b, c, d;
    __asm__ __volatile__("cpuid"
        : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
        : "a"(eax));
    if (eax_out) *eax_out = a;
    if (ebx_out) *ebx_out = b;
    if (ecx_out) *ecx_out = c;
    if (edx_out) *edx_out = d;
}

static inline int cpuid_has_apic(void) {
    uint32_t edx;
    cpuid(1, 0, 0, 0, &edx);
    return (edx & CPUID_FEATURE_APIC) != 0;
}

#endif
