#pragma once

#define KEYBOARD_VECTOR 0x21
#define KEYBOARD_GSI    1

void keyboard_init(void);
void keyboard_handle_irq(void);
