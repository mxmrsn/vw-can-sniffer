/*
  ESP32 UART -> WebSocket gateway for Teensy CAN stream
  Protocol: docs/uart_protocol.md

  Dependencies (Arduino Library Manager):
  - WebSockets ("WebSockets by Markus Sattler")

  Works with the built-in WebServer from ESP32 core and SPIFFS.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <SPIFFS.h>

// ---------- Wi-Fi Config ----------
static const char *WIFI_SSID = "YOUR_SSID";
static const char *WIFI_PASS = "YOUR_PASSWORD";

// If STA fails, start AP for quick access
static const bool ENABLE_SOFTAP_FALLBACK = true;
static const char *AP_SSID = "vw-can-sniffer";
static const char *AP_PASS = "canlogger"; // >= 8 chars

// ---------- UART Config ----------
static const uint32_t UART_BAUD = 921600;
#define UART_PORT Serial2
static const int UART_RX_PIN = 16;
static const int UART_TX_PIN = 17;

// ---------- Protocol ----------
static const uint8_t SOF = 0xA5;
static const uint8_t TYPE_CAN = 0x01;

// ---------- Servers ----------
WebServer http(80);
WebSocketsServer ws(81);

// ---------- LED ----------
static const int LED_PIN = LED_BUILTIN;
static const uint32_t LED_PERIOD_MS = 500;
static uint32_t led_last_ms = 0;


// ---------- UART Parser ----------
struct Parser {
  uint8_t state = 0;
  uint8_t len = 0;
  uint8_t type = 0;
  uint8_t idx = 0;
  uint8_t crc = 0;
  uint8_t payload[32];
} parser;

// ---------- Stats ----------
static uint32_t stat_rx_ok = 0;
static uint32_t stat_rx_bad = 0;
static uint32_t stat_bytes = 0;
static uint32_t stat_last_ms = 0;

static inline uint8_t xor_crc(const uint8_t *buf, size_t len) {
  uint8_t c = 0;
  for (size_t i = 0; i < len; i++) c ^= buf[i];
  return c;
}

static void handle_can_payload(const uint8_t *p, uint8_t len) {
  if (len < 9) return; // ts(4)+id(4)+dlc(1)
  stat_rx_ok++;

  uint32_t ts = 0, id = 0;
  memcpy(&ts, p, 4);
  memcpy(&id, p + 4, 4);
  uint8_t dlc = p[8];
  if (dlc > 8) dlc = 8;

  char data_hex[3 * 8 + 1];
  data_hex[0] = '\0';
  for (uint8_t i = 0; i < dlc; i++) {
    char tmp[4];
    snprintf(tmp, sizeof(tmp), "%02X ", p[9 + i]);
    strncat(data_hex, tmp, sizeof(data_hex) - strlen(data_hex) - 1);
  }

  // JSON message
  char out[160];
  snprintf(out, sizeof(out), "{\"ts\":%lu,\"id\":%lu,\"dlc\":%u,\"data\":\"%s\"}",
           (unsigned long)ts, (unsigned long)id, (unsigned)dlc, data_hex);
  ws.broadcastTXT(out);
}

static void parser_feed(uint8_t b) {
  stat_bytes++;
  switch (parser.state) {
    case 0: // wait SOF
      if (b == SOF) { parser.state = 1; }
      break;
    case 1: // LEN
      parser.len = b;
      parser.crc = b;
      parser.state = 2;
      break;
    case 2: // TYPE
      parser.type = b;
      parser.crc ^= b;
      parser.idx = 0;
      if (parser.len > sizeof(parser.payload)) {
        parser.state = 0; // invalid
      } else if (parser.len == 0) {
        parser.state = 4; // CRC next
      } else {
        parser.state = 3; // payload
      }
      break;
    case 3: // PAYLOAD
      parser.payload[parser.idx++] = b;
      parser.crc ^= b;
      if (parser.idx >= parser.len) parser.state = 4;
      break;
    case 4: // CRC
      if (parser.crc == b) {
        if (parser.type == TYPE_CAN) {
          handle_can_payload(parser.payload, parser.len);
        }
      } else {
        stat_rx_bad++;
      }
      parser.state = 0;
      break;
    default:
      parser.state = 0;
      break;
  }
}

// ---------- Web Handlers ----------
static void setup_static_routes() {
  http.serveStatic("/", SPIFFS, "/index.html");
  http.serveStatic("/app.css", SPIFFS, "/app.css");
  http.serveStatic("/app.js", SPIFFS, "/app.js");
}

void setup() {
  Serial.begin(115200);
  UART_PORT.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  if (!SPIFFS.begin(true)) {
    // If this fails, the web UI won't load; UART/WebSocket still works.
    Serial.println("SPIFFS mount failed");
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) {
    delay(100);
  }

  if (WiFi.status() != WL_CONNECTED && ENABLE_SOFTAP_FALLBACK) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
  }

  setup_static_routes();
  http.begin();

  ws.begin();
  ws.onEvent([](uint8_t num, WStype_t type, uint8_t *payload, size_t length) {
    (void)num; (void)payload; (void)length;
    if (type == WStype_CONNECTED) {
      // nothing needed
    }
  });
}

void loop() {
  http.handleClient();
  ws.loop();

  while (UART_PORT.available()) {
    uint8_t b = (uint8_t)UART_PORT.read();
    parser_feed(b);
  }

  // LED heartbeat
  uint32_t now_ms = millis();
  if (now_ms - led_last_ms >= LED_PERIOD_MS) {
    led_last_ms = now_ms;
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
  }

  // Broadcast stats once per second
  if (now_ms - stat_last_ms >= 1000) {
    stat_last_ms = now_ms;
    char out[128];
    snprintf(out, sizeof(out), "{\"type\":\"stat\",\"rx\":%lu,\"bad\":%lu,\"bytes\":%lu}",
             (unsigned long)stat_rx_ok, (unsigned long)stat_rx_bad, (unsigned long)stat_bytes);
    ws.broadcastTXT(out);
  }
}
