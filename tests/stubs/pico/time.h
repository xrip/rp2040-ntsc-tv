#pragma once

#include <stdint.h>
#include <stdbool.h>

// Virtual time lets the host check the demo cycle without waiting 84 seconds.
typedef uint64_t absolute_time_t;
static absolute_time_t test_time_us;
static inline absolute_time_t get_absolute_time(void) { return test_time_us; }
static inline absolute_time_t delayed_by_ms(absolute_time_t time, unsigned ms) {
    return time + (uint64_t)ms * 1000u;
}
static inline absolute_time_t make_timeout_time_ms(unsigned ms) {
    return delayed_by_ms(test_time_us, ms);
}
static inline int64_t absolute_time_diff_us(absolute_time_t from, absolute_time_t to) {
    return (int64_t)to - (int64_t)from;
}
static inline bool time_reached(absolute_time_t time) { return test_time_us >= time; }
static inline void sleep_until(absolute_time_t time) {
    if (time > test_time_us) test_time_us = time;
}
static inline void sleep_ms(unsigned ms) { test_time_us = delayed_by_ms(test_time_us, ms); }
