#pragma once

// Host preview only: the firmware demo's LED has no simulated hardware.
#define GPIO_OUT 1
#define PICO_DEFAULT_LED_PIN 25
static inline void gpio_init(unsigned pin) { (void)pin; }
static inline void gpio_set_dir(unsigned pin, int direction) {
    (void)pin; (void)direction;
}
static inline void gpio_put(unsigned pin, int value) {
    (void)pin; (void)value;
}
