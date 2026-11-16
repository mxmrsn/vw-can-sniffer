// The board's indicators: one chain of four addressable RGB LEDs on one pin.
#pragma once

#include <stdbool.h>

enum board_led {
    BOARD_LED_SILENT, // D1, the status LED: amber while listen-only, green once it may transmit
    BOARD_LED_CAN,    // D2: CAN activity
    BOARD_LED_BLE,    // D3: a phone is connected
    BOARD_LED_LOG,    // D4: logging to the card
};

void leds_init(void);
void leds_set(enum board_led which, bool on); // safe from any task
