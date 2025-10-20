/*
  Receiver_ESP32.ino
  - Connects to router STA using provided credentials
  - Polls Sender at http://192.168.1.50/api/devices every 3 seconds
  - Displays up to 4 active devices on I2C 20x4 LCD (LiquidCrystal_I2C)

  🧠 ESP32 Receiver Wiring (I²C LCD 20×4)
LCD Pin	Connect to ESP32 Pin	Notes
VCC	5V or 3.3V	Most I²C LCDs work with 5V; check your module (both logic 3.3V-safe)
GND	GND	Common ground with ESP32
SDA	GPIO 21 (D21)	I²C data line
SCL	GPIO 22 (D22)	I²C clock line
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <LiquidCrystal_I2C.h>
#include <Wire.h>

// ----- USER CONFIG -----
const char* STA_SSID = "Airtel_7737476759";
const char* STA_PASS = "air49169";

const char* SENDER_HOST = "192.168.1.50"; // Sender STA IP
const unsigned long POLL_INTERVAL_MS = 3000UL; // 3 seconds

const uint8_t LCD_ADDR = 0x27; // adjust to your module (0x3F possible)
const uint8_t LCD_COLS = 20;
const uint8_t LCD_ROWS = 4;
// ------------------------

LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);

unsigned long lastPoll = 0;

struct DisplayItem {
  String label;
  float percent;
};

#define MAX_DISPLAY 4

bool ensureWiFi(unsigned long timeoutMillis = 10000) {
  if (WiFi.status() == WL_CONNECTED) return true;
  Serial.printf("Connecting to WiFi '%s' ...\n", STA_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(STA_SSID, STA_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - t0) < timeoutMillis) {
    delay(200);
    Serial.print('.');
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWiFi connected, IP=%s\n", WiFi.localIP().toString().c_str());
    return true;
  }
  Serial.println("\nWiFi connect failed");
  return false;
}

void renderLCD(DisplayItem items[], int count) {
  lcd.clear();
  if (count == 0) {
    lcd.setCursor(0, 0);
    lcd.print("No active devices");
    return;
  }
  for (int r = 0; r < LCD_ROWS; ++r) {
    lcd.setCursor(0, r);
    if (r < count) {
      String left = items[r].label;
      if (left.length() > 14) left = left.substring(0, 14);
      String pct;
      if (items[r].percent < 0) pct = "--.-%";
      else {
        char buf[8];
        snprintf(buf, sizeof(buf), "%5.1f%%", items[r].percent);
        pct = String(buf);
      }
      lcd.print(left);
      int pad = LCD_COLS - left.length() - pct.length();
      if (pad < 1) pad = 1;
      for (int i=0;i<pad;i++) lcd.print(' ');
      lcd.print(pct);
    } else {
      for (int c=0;c<LCD_COLS;c++) lcd.print(' ');
    }
  }
}

void httpPollAndDisplay() {
  if (WiFi.status() != WL_CONNECTED) {
    ensureWiFi(5000);
    if (WiFi.status() != WL_CONNECTED) {
      lcd.clear(); lcd.setCursor(0,0); lcd.print("WiFi disconnected");
      return;
    }
  }

  String url = String("http://") + SENDER_HOST + "/api/devices";
  WiFiClient client;
  HTTPClient http;
  http.begin(client, url);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("HTTP GET failed, code=%d\n", code);
    http.end();
    lcd.clear(); lcd.setCursor(0,0); lcd.print("HTTP poll failed"); lcd.setCursor(0,1); lcd.print("code: " + String(code));
    return;
  }
  String body = http.getString();
  http.end();

  const size_t CAP = 16 * 1024;
  StaticJsonDocument<CAP> doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("JSON parse failed: %s\n", err.c_str());
    lcd.clear(); lcd.setCursor(0,0); lcd.print("JSON parse error");
    return;
  }

  const unsigned long ACTIVE_THRESHOLD_SEC = 15;
  DisplayItem items[MAX_DISPLAY];
  int found = 0;

  if (doc.is<JsonArray>()) {
    JsonArray arr = doc.as<JsonArray>();
    for (JsonObject o : arr) {
      if (found >= MAX_DISPLAY) break;
      if (!o.containsKey("age_seconds") || o["age_seconds"].isNull()) continue;
      long age = o["age_seconds"].as<long>();
      if (age > (long)ACTIVE_THRESHOLD_SEC) continue;
      const char* name = o["name"] | "";
      const char* mac = o["mac"] | "";
      String label;
      if (name && strlen(name)) label = String(name);
      else if (mac && strlen(mac)) label = String(mac);
      else label = "device";
      float pct = -1;
      if (o.containsKey("percent") && !o["percent"].isNull()) pct = o["percent"].as<float>();
      items[found].label = label;
      items[found].percent = pct;
      found++;
    }
  } else {
    Serial.println("api/devices returned non-array JSON");
    lcd.clear(); lcd.setCursor(0,0); lcd.print("Bad devices JSON");
    return;
  }

  renderLCD(items, found);
  Serial.printf("Displayed %d active devices\n", found);
}

void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println("\nESP32 Receiver starting...");

  Wire.begin(); // default SDA=21, SCL=22
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Receiver starting...");

  ensureWiFi(10000);
  if (WiFi.status() == WL_CONNECTED) {
    lcd.clear(); lcd.setCursor(0,0); lcd.print("WiFi connected");
    delay(700);
  } else {
    lcd.clear(); lcd.setCursor(0,0); lcd.print("WiFi not connected");
    delay(700);
  }
  lastPoll = millis() - POLL_INTERVAL_MS;
}

void loop() {
  unsigned long now = millis();
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastTry = 0;
    if (now - lastTry > 5000) { lastTry = now; ensureWiFi(5000); }
  }
  if (now - lastPoll >= POLL_INTERVAL_MS) {
    lastPoll = now;
    httpPollAndDisplay();
  }
  delay(10);
}
