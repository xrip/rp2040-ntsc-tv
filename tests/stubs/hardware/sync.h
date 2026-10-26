#pragma once

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

// These single-threaded tests verify descriptor/renderer semantics, not the
// RP2040 inter-core memory model. Detect accidental recursive lock acquisition.
typedef struct {
    bool held;
} spin_lock_t;

static inline unsigned spin_lock_claim_unused(bool required) {
    (void)required;
    return 0;
}

static inline spin_lock_t *spin_lock_init(unsigned lock_number) {
    (void)lock_number;
    static spin_lock_t lock;
    return &lock;
}

static inline uint32_t spin_lock_blocking(spin_lock_t *lock) {
    assert(!lock->held);
    lock->held = true;
    return 0;
}

static inline void spin_unlock(spin_lock_t *lock, uint32_t saved_state) {
    (void)saved_state;
    assert(lock->held);
    lock->held = false;
}
