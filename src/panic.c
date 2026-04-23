#include "panic.h"
#include "kprintf.h"

void panic(const char *msg) {
    kprintf("\n[panic] %s\n[panic] halting CPU\n", msg);
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}
