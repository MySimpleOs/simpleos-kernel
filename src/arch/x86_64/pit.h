#pragma once

#include <stdint.h>

#define PIT_FREQUENCY 1193182u

/* Arm PIT channel 2 for a one-shot of `ticks` counts at the 1.193 MHz base.
 * Channel 2 is traditionally the speaker; driving it with the speaker
 * disabled lets us time a known-length interval without touching the
 * system timer (channel 0) the bootloader might still be using. */
void pit_prepare_oneshot(uint16_t ticks);

/* Non-zero once the channel 2 output pin has latched high — the oneshot
 * finished. */
int  pit_oneshot_done(void);
