#pragma once

// The host renderer calls demo_draw(), not the firmware's infinite main loop.
static inline void sleep_ms(unsigned milliseconds) { (void)milliseconds; }
