/*
  Teensy 4.0 CAN sniffer -> UART/USB streamer
  Protocol documented in docs/uart_protocol.md

  Dependencies:
  - FlexCAN_T4 (Teensyduino Library Manager)
*/

#include <Arduino.h>
#include <FlexCAN_T4.h>
#include "pins.h"

// ---------- Config ----------
static const uint32_t UART_BAUD = 921600;
static const uint32_t CAN_BAUD  = 500000; // typical VW CAN speed; adjust if needed

// Set to 1 to generate synthetic frames on a timer (bench testing without CAN)
#define TEST_MODE 0
#define TEST_RATE_HZ 50

// UART device (to ESP32)
#define UART_PORT Serial1
// USB serial device (to PC)
#define USB_PORT Serial

// Output mode selection
enum OutputMode : uint8_t { OUTPUT_UART = 0, OUTPUT_USB = 1 };
static OutputMode output_mode = OUTPUT_UART;

// External LEDs
static const uint32_t LED_PULSE_MS = 40;
static const uint32_t LED_WIFI_PERIOD_MS = 500;
static uint32_t led_can_last_ms = 0;
static uint32_t led_usb_last_ms = 0;
static uint32_t led_wifi_last_ms = 0;

// Buttons (active-low with pullups)
static uint32_t last_btn_ms = 0;
static bool last_mode_state = true;
static bool last_select_state = true;

// CAN device (Teensy 4.0 has CAN1/CAN2/CAN3)
FlexCAN_T4<CAN1, RX_SIZE_256, TX_SIZE_16> Can0;

// ---------- Protocol ----------
static const uint8_t SOF  = 0xA5;
static const uint8_t TYPE_CAN = 0x01;
static const uint8_t TYPE_STAT = 0x02;
static const uint8_t TYPE_STATE = 0x03;
static const uint8_t TYPE_CMD = 0x10;

struct CanFrameLite {
  uint32_t ts_us;
  uint32_t id;
  uint8_t  dlc;
  uint8_t  data[8];
};

// ---------- Ring Buffer ----------
static const uint16_t RB_SIZE = 256;
static CanFrameLite rb[RB_SIZE];
static volatile uint16_t rb_head = 0;
static volatile uint16_t rb_tail = 0;

// ---------- Stats ----------
static volatile uint32_t stat_rx_ok = 0;
static volatile uint32_t stat_rx_drop = 0;
static uint32_t stat_last_ms = 0;
static uint32_t state_last_ms = 0;

// ---------- Command Parser ----------
struct CmdParser {
  uint8_t state = 0;
  uint8_t len = 0;
  uint8_t type = 0;
  uint8_t idx = 0;
  uint8_t crc = 0;
  uint8_t payload[32];
} cmd_parser;

static inline bool rb_is_full() {
  return ((rb_head + 1) % RB_SIZE) == rb_tail;
}

static inline bool rb_is_empty() {
  return rb_head == rb_tail;
}

static inline void rb_push(const CanFrameLite &f) {
  if (rb_is_full()) {
    // Drop newest if full (simple policy)
    stat_rx_drop++;
    return;
  }
  rb[rb_head] = f;
  rb_head = (rb_head + 1) % RB_SIZE;
}

static inline bool rb_pop(CanFrameLite &out) {
  if (rb_is_empty()) return false;
  out = rb[rb_tail];
  rb_tail = (rb_tail + 1) % RB_SIZE;
  return true;
}

// ---------- Helpers ----------
static inline uint8_t xor_crc(const uint8_t *buf, size_t len) {
  uint8_t c = 0;
  for (size_t i = 0; i < len; i++) c ^= buf[i];
  return c;
}

static inline uint32_t encode_can_id(const CAN_message_t &msg) {
  if (msg.flags.extended) {
    return (0x80000000u | (msg.id & 0x1FFFFFFFu));
  }
  return (msg.id & 0x7FFu);
}

static void set_output_mode(OutputMode mode) {
  output_mode = mode;
  USB_PORT.printf("Output mode: %s\n", output_mode == OUTPUT_UART ? "UART" : "USB");
}

static void set_silent_mode(bool silent) {
  digitalWrite(CAN_SILENT_PIN, silent ? HIGH : LOW);
}

static void send_frame(uint8_t type, const uint8_t *payload, uint8_t len) {
  uint8_t header[3] = { SOF, len, type };
  uint8_t crc = len ^ type ^ xor_crc(payload, len);

  if (output_mode == OUTPUT_UART) {
    UART_PORT.write(header, sizeof(header));
    UART_PORT.write(payload, len);
    UART_PORT.write(&crc, 1);
  } else {
    USB_PORT.write(header, sizeof(header));
    USB_PORT.write(payload, len);
    USB_PORT.write(&crc, 1);
    digitalWrite(LED_USB_PIN, HIGH);
    led_usb_last_ms = millis();
  }
}

static void handle_cmd(const uint8_t *payload, uint8_t len) {
  if (len < 2) return;
  uint8_t cmd = payload[0];
  uint8_t val = payload[1];

  switch (cmd) {
    case 0x01: // set output mode
      if (val <= 1) {
        set_output_mode(val == 0 ? OUTPUT_UART : OUTPUT_USB);
      }
      break;
    case 0x02: // set silent
      set_silent_mode(val != 0);
      break;
    default:
      break;
  }
}

static void cmd_parser_feed(uint8_t b) {
  switch (cmd_parser.state) {
    case 0:
      if (b == SOF) cmd_parser.state = 1;
      break;
    case 1:
      cmd_parser.len = b;
      cmd_parser.crc = b;
      cmd_parser.state = 2;
      break;
    case 2:
      cmd_parser.type = b;
      cmd_parser.crc ^= b;
      cmd_parser.idx = 0;
      if (cmd_parser.len > sizeof(cmd_parser.payload)) {
        cmd_parser.state = 0;
      } else if (cmd_parser.len == 0) {
        cmd_parser.state = 4;
      } else {
        cmd_parser.state = 3;
      }
      break;
    case 3:
      cmd_parser.payload[cmd_parser.idx++] = b;
      cmd_parser.crc ^= b;
      if (cmd_parser.idx >= cmd_parser.len) cmd_parser.state = 4;
      break;
    case 4:
      if (cmd_parser.crc == b) {
        if (cmd_parser.type == TYPE_CMD) {
          handle_cmd(cmd_parser.payload, cmd_parser.len);
        }
      }
      cmd_parser.state = 0;
      break;
    default:
      cmd_parser.state = 0;
      break;
  }
}

void setup() {
  UART_PORT.begin(UART_BAUD);
  USB_PORT.begin(115200);
  while (!UART_PORT && millis() < 2000) {}

  pinMode(CAN_SILENT_PIN, OUTPUT);
  digitalWrite(CAN_SILENT_PIN, LOW); // normal mode (LOW = normal, HIGH = silent)

  pinMode(LED_CAN_PIN, OUTPUT);
  pinMode(LED_WIFI_PIN, OUTPUT);
  pinMode(LED_USB_PIN, OUTPUT);
  pinMode(LED_SILENT_PIN, OUTPUT);
  digitalWrite(LED_CAN_PIN, LOW);
  digitalWrite(LED_WIFI_PIN, HIGH);
  digitalWrite(LED_USB_PIN, LOW);
  digitalWrite(LED_SILENT_PIN, LOW);

  pinMode(BTN_MODE_PIN, INPUT_PULLUP);
  pinMode(BTN_SELECT_PIN, INPUT_PULLUP);

  Can0.begin();
  Can0.setBaudRate(CAN_BAUD);
  Can0.setMaxMB(16);
  Can0.enableFIFO();
  Can0.enableFIFOInterrupt();
  Can0.onReceive([](const CAN_message_t &msg){
    CanFrameLite f;
    f.ts_us = micros();
    f.id = encode_can_id(msg);
    f.dlc = msg.len;
    for (uint8_t i = 0; i < msg.len; i++) f.data[i] = msg.buf[i];
    rb_push(f);
    stat_rx_ok++;

    // CAN activity pulse
    digitalWrite(LED_CAN_PIN, HIGH);
    led_can_last_ms = millis();
  });
}

void loop() {
  uint32_t now_ms = millis();

  // Mode button (toggle output)
  bool mode_state = digitalRead(BTN_MODE_PIN);
  bool select_state = digitalRead(BTN_SELECT_PIN);
  if (now_ms - last_btn_ms > 50) {
    if (last_mode_state && !mode_state) {
      set_output_mode(output_mode == OUTPUT_UART ? OUTPUT_USB : OUTPUT_UART);
      last_btn_ms = now_ms;
    }
    if (last_select_state && !select_state) {
      set_silent_mode(digitalRead(CAN_SILENT_PIN) == LOW);
      last_btn_ms = now_ms;
    }
  }
  last_mode_state = mode_state;
  last_select_state = select_state;

  // LEDs
  if (led_can_last_ms && now_ms - led_can_last_ms >= LED_PULSE_MS) {
    digitalWrite(LED_CAN_PIN, LOW);
    led_can_last_ms = 0;
  }
  if (led_usb_last_ms && now_ms - led_usb_last_ms >= LED_PULSE_MS) {
    digitalWrite(LED_USB_PIN, LOW);
    led_usb_last_ms = 0;
  }
  if (output_mode == OUTPUT_UART) {
    if (now_ms - led_wifi_last_ms >= LED_WIFI_PERIOD_MS) {
      led_wifi_last_ms = now_ms;
      digitalWrite(LED_WIFI_PIN, !digitalRead(LED_WIFI_PIN));
    }
  } else {
    digitalWrite(LED_WIFI_PIN, LOW);
  }
  digitalWrite(LED_SILENT_PIN, digitalRead(CAN_SILENT_PIN) ? HIGH : LOW);

  // Periodic status frame (TEST_MODE only)
#if TEST_MODE
  if (now_ms - stat_last_ms >= 1000) {
    stat_last_ms = now_ms;
    uint8_t payload[8];
    memcpy(&payload[0], &stat_rx_ok, 4);
    memcpy(&payload[4], &stat_rx_drop, 4);
    send_frame(TYPE_STAT, payload, 8);
  }
#endif

  // Periodic state frame (always)
  if (now_ms - state_last_ms >= 1000) {
    state_last_ms = now_ms;
    uint8_t payload[2];
    payload[0] = (uint8_t)output_mode;
    payload[1] = digitalRead(CAN_SILENT_PIN) ? 1 : 0;
    send_frame(TYPE_STATE, payload, 2);
  }

  // Handle incoming commands from ESP32 over UART
  while (UART_PORT.available()) {
    cmd_parser_feed((uint8_t)UART_PORT.read());
  }

  // Synthetic frame generator for bench testing
#if TEST_MODE
  static uint32_t last_us = 0;
  uint32_t now = micros();
  if ((uint32_t)(now - last_us) >= (1000000u / TEST_RATE_HZ)) {
    last_us = now;
    CanFrameLite f;
    f.ts_us = now;
    f.id = 0x123;
    f.dlc = 8;
    for (uint8_t i = 0; i < f.dlc; i++) f.data[i] = (uint8_t)(now >> (i * 2));
    rb_push(f);
  }
#endif

  // Drain UART as fast as possible
  CanFrameLite f;
  while (rb_pop(f)) {
    uint8_t payload[4 + 4 + 1 + 8];
    size_t idx = 0;

    // timestamp
    memcpy(&payload[idx], &f.ts_us, 4); idx += 4;
    // can id
    memcpy(&payload[idx], &f.id, 4); idx += 4;
    // dlc
    payload[idx++] = f.dlc;
    // data
    memcpy(&payload[idx], f.data, f.dlc); idx += f.dlc;

    uint8_t len = (uint8_t)idx;
    uint8_t header[3] = { SOF, len, TYPE_CAN };
    uint8_t crc = len ^ TYPE_CAN ^ xor_crc(payload, len);

    if (output_mode == OUTPUT_UART) {
      UART_PORT.write(header, sizeof(header));
      UART_PORT.write(payload, len);
      UART_PORT.write(&crc, 1);
    } else {
      USB_PORT.write(header, sizeof(header));
      USB_PORT.write(payload, len);
      USB_PORT.write(&crc, 1);
      digitalWrite(LED_USB_PIN, HIGH);
      led_usb_last_ms = millis();
    }
  }

  // Yield to background tasks
  yield();
}
