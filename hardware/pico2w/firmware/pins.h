#pragma once

// Pin map for the Pico 2 W carrier, PCB v0.2.0
// (hardware/pico2w/pcb). GPIO numbers, not package pins.

// CAN (PIO / can2040) -> TJA1051T/3
#define CAN_TX_PIN 17      // TXD
#define CAN_RX_PIN 18      // RXD
#define CAN_SILENT_PIN 16  // S: high = listen-only

// LEDs (active-high, 330R to GND)
#define LED_CAN_PIN 7      // CAN activity
#define LED_WIFI_PIN 13    // Wi-Fi active
#define LED_USB_PIN 14     // USB activity
#define LED_SILENT_PIN 15  // CAN silent indicator

// Buttons: switch to 3V3 with external 10k pull-down -> ACTIVE-HIGH, no internal pull needed
#define BTN_MODE_PIN 3
#define BTN_SELECT_PIN 6

// microSD, SPI mode on SPI1. No external pull-up on CS: drive it high first.
#define SD_SPI_PORT spi1
#define SD_MISO_PIN 8
#define SD_SCK_PIN 10
#define SD_MOSI_PIN 11
#define SD_CS_PIN 12

// OBD supply sense: fused OBD +12V -> 100k / 22k -> GP28 (ADC2). V_obd = V_adc * 5.545
// Reads ~0 V when powered from USB only.
#define OBD_SENSE_PIN 28
#define OBD_SENSE_ADC_CH 2
