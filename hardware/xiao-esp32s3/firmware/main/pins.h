#pragma once

// Pin map for the Seeed XIAO ESP32-S3 carrier, PCB v0.3.0
// (hardware/xiao-esp32s3/pcb). GPIO numbers, not pad names; the pad each one
// sits on is in the comment. Authoritative source: ../../docs/design_notes.md.
//
// The assignment follows the board layout: signals that leave the module
// towards the transceiver, the card socket and the divider sit on the pads
// nearest them, and the two indicators and the button sit on the far side.

// CAN (TWAI) -> TJA1051T/3
#define CAN_TX_PIN 5      // D4, CTX
#define CAN_RX_PIN 6      // D5, CRX
#define CAN_SILENT_PIN 4  // D3, S: high = listen-only. R2 holds it high until this pin drives it.

// Indicators: four addressable RGB LEDs in one chain, through a buffer that
// lifts the 3.3 V data line to the LEDs' supply level. See leds.c.
#define LED_DATA_PIN 1    // D0

// Spare pad on header J3, for a GPS pulse-per-second input or a camera sync line.
#define SPARE_IO_PIN 2    // D1

// Button to +3V3 with a 10k pull-down on the board, so PRESSED = HIGH, the same
// as the v0.2.0 board. GPIO3 is a strapping pin, and the pull-down also gives it
// a defined level at reset.
#define BTN_MODE_PIN 3    // D2
#define BTN_MODE_PRESSED 1              // pressed reads high
#define BTN_MODE_PULL GPIO_FLOATING     // the board's 10k pull-down does it

// microSD, SPI mode. No external pull-up on CS: drive it high before mounting.
// GPIO43/44 are UART0 during the ROM boot log, before app_main takes them over.
#define SD_SPI_HOST SPI2_HOST
#define SD_CS_PIN 43      // D6
#define SD_SCK_PIN 44     // D7
#define SD_MOSI_PIN 7     // D8
#define SD_MISO_PIN 8     // D9

// OBD supply sense: fused OBD +12V -> 100k / 22k -> GPIO9 (ADC1_CH8).
// Reads ~0 V when powered from USB only. Read by the self-test (main.c).
#define OBD_SENSE_PIN 9   // D10
#define OBD_SENSE_ADC_CH ADC_CHANNEL_8
