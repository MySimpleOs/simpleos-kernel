#include "hypervisor.h"

static void cpuid_leaf(uint32_t leaf, uint32_t sub,
                        uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    uint32_t a, b, c, d;
    __asm__ volatile ("cpuid"
                      : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                      : "a"(leaf), "c"(sub));
    if (eax) *eax = a;
    if (ebx) *ebx = b;
    if (ecx) *ecx = c;
    if (edx) *edx = d;
}

static void u32_to_sig4(uint32_t u, char *out) {
    out[0] = (char) (u & 0xffu);
    out[1] = (char) ((u >> 8) & 0xffu);
    out[2] = (char) ((u >> 16) & 0xffu);
    out[3] = (char) ((u >> 24) & 0xffu);
}

int hypervisor_is_virtualbox(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid_leaf(1, 0, &eax, &ebx, &ecx, &edx);
    if ((ecx & (1u << 31)) == 0)
        return 0;

    cpuid_leaf(0x40000000u, 0, &eax, &ebx, &ecx, &edx);
    (void) eax;
    char sig[13];
    u32_to_sig4(ebx, sig);
    u32_to_sig4(ecx, sig + 4);
    u32_to_sig4(edx, sig + 8);
    sig[12] = '\0';

    /* VirtualBox exposes this 12-byte ID (see VirtualBox CPUID spec). */
    static const char vb[] = "VBoxVBoxVBox";
    for (int i = 0; i < 12; i++) {
        if (sig[i] != vb[i]) return 0;
    }
    return 1;
}
