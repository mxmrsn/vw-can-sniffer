/*
  Teensy 4.1 CAN sniffer -> UART streamer
  Protocol documented in docs/uart_protocol.md

  Dependencies:
  - FlexCAN_T4 (Teensyduino Library Manager)
*/

#include <Arduino.h>
#include <FlexCAN_T4.h>

// ---------- Config ----------
static const uint32_t UART_BAUD = 921600;
static const uint32_t CAN_BAUD  = 500000; // typical VW CAN speed; adjust if needed

// External heartbeat LED (Teensy A0 / pin 14)
#define LED_CAN_PIN 14
static const uint32_t LED_PERIOD_MS = 500;
static uint32_t led_last_ms = 0;

// Set to 1 to generate synthetic frames on a timer (bench testing without CAN)
#define TEST_MODE 0
#define TEST_RATE_HZ 50

// UART device
#define UART_PORT Serial1

// Optional CAN transceiver silent mode (Teensy pin 21 -> TJA1051 S)
#define CAN_SILENT_PIN 21

// CAN device (Teensy 4.1 has CAN1/CAN2/CAN3)
FlexCAN_T4<CAN1, RX_SIZE_256, TX_SIZE_16> Can0;

// ---------- Protocol ----------
static const uint8_t SOF  = 0xA5;
static const uint8_t TYPE_CAN = 0x01;

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

static inline bool rb_is_full() {
  return ((rb_head + 1) % RB_SIZE) == rb_tail;
}

static inline bool rb_is_empty() {
  return rb_head == rb_tail;
}

static inline void rb_push(const CanFrameLite &f) {
  if (rb_is_full()) {
    // Drop newest if full (simple policy)
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

void setup() {
  UART_PORT.begin(UART_BAUD);
  while (!UART_PORT && millis() < 2000) {}

  pinMode(LED_CAN_PIN, OUTPUT);
  digitalWrite(LED_CAN_PIN, LOW);

  pinMode(CAN_SILENT_PIN, OUTPUT);
  digitalWrite(CAN_SILENT_PIN, LOW); // normal mode (LOW = normal, HIGH = silent)

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
  });
}

void loop() {
  // Heartbeat LED
  uint32_t now_ms = millis();
  if (now_ms - led_last_ms >= LED_PERIOD_MS) {
    led_last_ms = now_ms;
    digitalWrite(LED_CAN_PIN, !digitalRead(LED_CAN_PIN));
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

    UART_PORT.write(header, sizeof(header));
    UART_PORT.write(payload, len);
    UART_PORT.write(&crc, 1);
  }

  // Yield to background tasks
  yield();
}
