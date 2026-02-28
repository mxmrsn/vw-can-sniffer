/*
  Teensy 4.1 CAN simulator/spoofer
  Sends periodic CAN frames for bench testing.

  Dependencies:
  - FlexCAN_T4 (Teensyduino Library Manager)
*/

#include <Arduino.h>
#include <FlexCAN_T4.h>

// ---------- Config ----------
static const uint32_t CAN_BAUD = 500000; // adjust as needed
static const bool USE_EXTENDED_IDS = false;

// Message schedule (ms)
static const uint32_t BASE_PERIOD_MS = 10;

// ---------- CAN device ----------
FlexCAN_T4<CAN1, RX_SIZE_256, TX_SIZE_16> Can0;

// ---------- Message List ----------
struct SimMsg {
  uint32_t id;
  uint8_t dlc;
  uint8_t data[8];
  uint16_t period_ms;
  uint32_t next_ms;
};

static SimMsg msgs[] = {
  {0x100, 8, {0x10,0x22,0x33,0x44,0x55,0x66,0x77,0x88}, 10, 0},
  {0x120, 4, {0x01,0x02,0x03,0x04,0,0,0,0}, 50, 0},
  {0x1A0, 8, {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x00,0x11}, 100, 0},
};

static const size_t MSG_COUNT = sizeof(msgs) / sizeof(msgs[0]);

static void send_msg(const SimMsg &m) {
  CAN_message_t out;
  out.id = m.id;
  out.flags.extended = USE_EXTENDED_IDS;
  out.len = m.dlc;
  for (uint8_t i = 0; i < m.dlc; i++) out.buf[i] = m.data[i];
  Can0.write(out);
}

void setup() {
  Can0.begin();
  Can0.setBaudRate(CAN_BAUD);

  uint32_t now = millis();
  for (size_t i = 0; i < MSG_COUNT; i++) {
    msgs[i].next_ms = now + (i * 5); // slight staggering
  }
}

void loop() {
  uint32_t now = millis();
  for (size_t i = 0; i < MSG_COUNT; i++) {
    if ((int32_t)(now - msgs[i].next_ms) >= 0) {
      send_msg(msgs[i]);
      msgs[i].next_ms = now + msgs[i].period_ms;
    }
  }

  delay(1);
}
