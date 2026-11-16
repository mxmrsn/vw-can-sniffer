#pragma once

// Pin map for the OBD dongle, top board v0.4.0 (hardware/esp32s3-dongle/pcb/top): ESP32-S3-MINI-1
// GPIO numbers. Authoritative source: ../../docs/architecture.md and the schematic generator
// pcb/scripts/top.py. Signals to the bottom board go through the 2 x 7 stacking connector.

// CAN: TJA1043T on the bottom board
#define CAN_TX_PIN 4       // CAN_TXD
#define CAN_RX_PIN 5       // CAN_RXD
#define CAN_STB_N_PIN 6    // CAN_STB_N: with EN, the transceiver's mode (internal pull-down)
#define CAN_EN_PIN 7       // CAN_EN (internal pull-down)
#define CAN_ERR_N_PIN 15   // CAN_ERR_N: error, power-on and wake-source flags, active low

// Battery at the OBD port, through 1 M / 220 k on the bottom board: ADC1 channel 0.
#define OBD_SENSE_PIN 1
#define OBD_SENSE_ADC_CH ADC_CHANNEL_0
#define OBD_SENSE_RATIO ((1000.0 + 220.0) / 220.0)

// MODE button: pulls WAKE_BTN low (waking a sleeping dongle); read here through a diode, with the
// internal pull-up, so pressed reads low. The engine-running detector pulls the same node low,
// so while the engine runs the button cannot be told apart: press it with the engine off.
#define BTN_MODE_PIN 2     // MODE_IN
#define BTN_MODE_PRESSED 0
#define BTN_MODE_PULL GPIO_PULLUP_ONLY

// Four addressable LEDs in one chain, through a buffer (as on the XIAO board).
#define LED_DATA_PIN 48

// microSD, SPI mode, 47 k pull-ups on CS, MOSI and MISO.
#define SD_SPI_HOST SPI2_HOST
#define SD_CS_PIN 10
#define SD_MOSI_PIN 11
#define SD_SCK_PIN 12
#define SD_MISO_PIN 13

// Not used by the firmware yet: IMU (I2C, optional part), GPS / sync connector, expansion.
#define IMU_SDA_PIN 8
#define IMU_SCL_PIN 9
#define IMU_INT_PIN 16
#define GPS_RXD_PIN 17
#define GPS_TXD_PIN 18
#define GPS_PPS_PIN 34
#define SYNC_OUT_PIN 35
#define EXP_CS1_PIN 14
#define EXP_CS2_PIN 21
#define EXP_INT1_PIN 47
#define EXP_INT2_PIN 33
