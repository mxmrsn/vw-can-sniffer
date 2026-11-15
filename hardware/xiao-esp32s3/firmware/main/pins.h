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

// LEDs. Only two fit on the 11 available pads, so three of platform.h's four
// indicators share them: see board_led() in main.c.
#define LED_CAN_PIN 1     // D0, green: CAN activity
#define LED_STATUS_PIN 2  // D1, blue: logging or a phone connected
#define LED_USER_PIN 21   // on the module itself, ACTIVE LOW: listen-only indicator

// Button to +3V3 with a 10k pull-down on the board, so PRESSED = HIGH, the same
// as the v0.2.0 board. GPIO3 is a strapping pin, and the pull-down also gives it
// a defined level at reset.
#define BTN_MODE_PIN 3    // D2

// microSD, SPI mode. No external pull-up on CS: drive it high before mounting.
// GPIO43/44 are UART0 during the ROM boot log, before app_main takes them over.
#define SD_SPI_HOST SPI2_HOST
#define SD_CS_PIN 43      // D6
#define SD_SCK_PIN 44     // D7
#define SD_MOSI_PIN 7     // D8
#define SD_MISO_PIN 8     // D9

// OBD supply sense: fused OBD +12V -> 100k / 22k -> GPIO9 (ADC1_CH8).
// Reads ~0 V when powered from USB only. Not read by the firmware yet.
#define OBD_SENSE_PIN 9   // D10
#define OBD_SENSE_ADC_CH ADC_CHANNEL_8
