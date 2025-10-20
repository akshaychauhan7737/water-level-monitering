/*
  ESP32 Receiver (WebSocket + HTTP fallback) -> I2C 20x4 LCD
  - Connects to router (STA) using provided credentials
  - Connects WebSocket to Sender (ws://192.168.1.50:81/)
  - Displays up to 4 active devices on a 20x4 I2C LCD
  - If WS is down, polls http://192.168.1.50/api/devices every POLL_INTERVAL_MS
*/

#include <WiFi.h>
#include <WebSocketsClient.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <LiquidCrystal_I2C.h>

// ---------------- USER CONFIG ----------------
const char* STA_SSID = "Airtel_7737476759";
const char* STA_PASS = "air49169";

// Sender address (STA IP from your Sender sketch)
const char* SENDER_HOST = "192.168.1.50";
const uint16_t SENDER_WS_PORT = 81;
const uint16_t SENDER_HTTP_PORT = 80;

const unsigned long POLL_INTERVAL_MS = 5000UL;  // fallback HTTP poll when WS disconnected
const unsigned long WS_RECONNECT_MS = 3000UL;   // try WS reconnect every N ms

// I2C LCD config - change address if your module is different (0x27 or 0x3F)
const uint8_t LCD_ADDR = 0x27;
const uint8_t LCD_COLS = 20;
const uint8_t LCD_ROWS = 4;
// ---------------------------------------------

WebSocketsClient webSocket;
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);

// store last devices message JSON for display
StaticJsonDocument<8192> lastDevicesDoc; // may be large if many devices; adjust if memory issues
bool haveDevices = false;
unsigned long lastPoll = 0;
unsigned long lastWsAttempt = 0;

// helper to ensure WiFi connected
void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.printf("Connecting to WiFi '%s' ...\n", STA_SSID);
  WiFi.begin(STA_SSID, STA_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    Serial.print('.');
    delay(300);
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi connected. IP=%s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("WiFi connect failed. Will retry in loop.");
  }
}

// Render up to 4 devices on the 20x4 display
void renderDevicesOnLCD() {
  lcd.clear();
  if (!haveDevices) {
    lcd.setCursor(0,0);
    lcd.print("No devices (yet)");
    return;
  }

  // Expect JSON: {"type":"devices","devices":[ {mac:, name:, percent:, ...}, ... ] }
  if (!lastDevicesDoc.containsKey("devices")) {
    lcd.setCursor(0,0);
    lcd.print("No devices array");
    return;
  }

  JsonArray arr = lastDevicesDoc["devices"].as<JsonArray>();
  int row = 0;
  for (JsonObject dev : arr) {
    if (row >= LCD_ROWS) break;
    const char* name = dev["name"] | "";
    const char* mac = dev["mac"] | "";
    // label: prefer name, else mac (truncate to fit)
    String left;
    if (name && strlen(name)) left = String(name);
    else if (mac && strlen(mac)) left = String(mac);
    else left = "device";

    // percent display
    String pct = "--";
    if (dev.containsKey("percent") && !dev["percent"].isNull()) {
      float p = dev["percent"].as<float>();
      char buf[8];
      snprintf(buf, sizeof(buf), "%5.1f%%", p);
      pct = String(buf);
    }

    // ensure left is not longer than 14 chars to leave space for percent at right
    if (left.length() > 14) left = left.substring(0, 14);

    // write to lcd: left at col0, percent right aligned at col 20 - len
    lcd.setCursor(0, row);
    lcd.print(left);
    // clear rest of line before printing percent
    int curLen = left.length();
    for (int c = curLen; c < LCD_COLS - 6; ++c) lcd.print(' ');
    // place percent at column 20 - 6 (fits e.g. "100.0%")
    int pctCol = LCD_COLS - 6;
    if (pctCol < (int)curLen + 1) pctCol = curLen + 1;
    lcd.setCursor(pctCol, row);
    lcd.print(pct);
    row++;
  }

  // if fewer than rows, fill remaining lines with empty
  for (int r = lastDevicesDoc["devices"].size(); r < LCD_ROWS; ++r) {
    if (r >= LCD_ROWS) break;
    lcd.setCursor(0, r);
    lcd.print("                    "); // 20 spaces
  }
}

// parse incoming WebSocket text message
void handleWsMessage(const char* payload, size_t length) {
  Serial.printf("WS RX: %.*s\n", (int)length, payload);
  // parse JSON (reuse lastDevicesDoc)
  DeserializationError err = deserializeJson(lastDevicesDoc, payload, length);
  if (err) {
    Serial.printf("JSON parse error: %s\n", err.c_str());
    return;
  }
  const char* type = lastDevicesDoc["type"] | "";
  if (strcmp(type, "devices") == 0) {
    haveDevices = true;
    renderDevicesOnLCD();
  } else if (strcmp(type, "config") == 0) {
    // config update received — optionally refresh display by requesting devices via HTTP
    // For now we'll call render if we also included a devices field or rely on subsequent devices broadcast
    Serial.println("Config update received");
  } else {
    // unknown message type - ignore
    Serial.println("Unknown WS message type");
  }
}

// WebSocket event callback
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.println("WS connected to sender");
      // optionally request immediate snapshot — but sender broadcasts on connect
      break;
    case WStype_DISCONNECTED:
      Serial.println("WS disconnected");
      break;
    case WStype_TEXT:
      handleWsMessage((const char*)payload, length);
      break;
    case WStype_ERROR:
      Serial.println("WS error");
      break;
    default:
      break;
  }
}

// attempt to connect WS to sender
void ensureWebSocket() {
  if (webSocket.isConnected()) return;
  if (WiFi.status() != WL_CONNECTED) return;
  unsigned long now = millis();
  if (now - lastWsAttempt < WS_RECONNECT_MS) return;
  lastWsAttempt = now;
  Serial.printf("WS connecting to %s:%u ...\n", SENDER_HOST, SENDER_WS_PORT);
  webSocket.begin(SENDER_HOST, SENDER_WS_PORT, "/");
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(0); // we handle reconnect ourselves
}

// fallback HTTP poll
void httpPollDevices() {
  if (WiFi.status() != WL_CONNECTED) return;
  String url = String("http://") + SENDER_HOST + "/api/devices";
  WiFiClient client;
  HTTPClient http;
  http.begin(client, url);
  int code = http.GET();
  if (code == 200) {
    String body = http.getString();
    Serial.println("HTTP poll got devices");
    DeserializationError err = deserializeJson(lastDevicesDoc, body);
    if (!err) {
      haveDevices = true;
      renderDevicesOnLCD();
    } else {
      Serial.printf("HTTP parse error: %s\n", err.c_str());
    }
  } else {
    Serial.printf("HTTP poll failed code=%d\n", code);
  }
  http.end();
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\nESP32 Receiver starting...");

  // init LCD
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Receiver starting...");

  // connect WiFi
  ensureWiFi();

  // setup websocket (not yet connected until ensureWebSocket called)
  webSocket.begin(SENDER_HOST, SENDER_WS_PORT, "/");
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(0);

  lastPoll = millis();
  lastWsAttempt = 0;
}

void loop() {
  // ensure WiFi
  if (WiFi.status() != WL_CONNECTED) {
    ensureWiFi();
  }

  // ensure WS
  ensureWebSocket();

  // process websocket (if connected)
  webSocket.loop();

  // if websocket not connected, fallback to HTTP poll every POLL_INTERVAL_MS
  if (!webSocket.isConnected()) {
    unsigned long now = millis();
    if (now - lastPoll >= POLL_INTERVAL_MS) {
      lastPoll = now;
      httpPollDevices();
    }
  }

  delay(10);
}
