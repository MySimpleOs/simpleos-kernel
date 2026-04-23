#pragma once

void idt_init(void);
void idt_load(void);              /* re-lidt on the current CPU */
