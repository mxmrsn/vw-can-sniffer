// The TJA1043T's operating modes, and its ERR_N flag pin, for the CAN port and power control.
#pragma once

#include <stdbool.h>

enum tja1043_mode {
    TJA1043_NORMAL,        // STB_N high, EN high: can transmit
    TJA1043_LISTEN_ONLY,   // STB_N high, EN low: receives, cannot drive the bus
    TJA1043_STANDBY,       // STB_N low, EN low: low power, wake detection; INH stays on
    TJA1043_GO_TO_SLEEP,   // STB_N low, EN high: on to Sleep, where INH floats and the board's power goes off
};

void tja1043_pins_init(void);
void tja1043_set_mode(enum tja1043_mode m);
bool tja1043_flag(void);             // ERR_N low: the flag this mode shows is set
void tja1043_stop_controller(void);  // TWAI off, before sleeping
