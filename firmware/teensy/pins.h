#pragma once

// Output pins
#define LED_CAN_PIN 14     // CAN activity
#define LED_WIFI_PIN 15    // UART/Wi-Fi active
#define LED_USB_PIN 16     // USB activity
#define LED_SILENT_PIN 21  // CAN silent indicator

// Buttons (active-low with pullups)
#define BTN_MODE_PIN 8
#define BTN_SELECT_PIN 9

// CAN transceiver silent mode (Teensy pin 20 -> TJA1051 S)
#define CAN_SILENT_PIN 20
