/*
  ESP8266 HTTP Sensor (complete)
  - Posts to Sender /api/report (includes MAC)
  - Polls /api/config?name=... for updates
  - Persist config (name, totalHeightCm, sensorToMaxCm) to EEPROM
  - Uses new HTTPClient API: http.begin(WiFiClient, url)
  ⚡ ESP8266 (Tank Sensor) — HC-SR04 Wiring
HC-SR04 Pin	ESP8266 (NodeMCU) Pin	Notes
VCC	5V	Sensor requires 5V power
GND	GND	Common ground
TRIG	D5 (GPIO14)	Trigger pin
ECHO	D6 (GPIO12)	Echo pin → ⚠ Level shift to 3.3V using resistor divider (10k + 4.7k)
ESP8266 VCC	3.3V	(ESP board itself runs on 3.3V)
*/

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <EEPROM.h>

/* ------------- USER CONFIG (edit per board) ------------- */
char DEFAULT_NAME[] = "Tank-1";   // change per device: "Tank-1", "Tank-2", "Tank-3"
const uint8_t TRIG_PIN = 14;      // D5 (GPIO14)
const uint8_t ECHO_PIN = 12;      // D6 (GPIO12)

const unsigned long REPORT_INTERVAL_MS = 2500;       // how often to POST sensor reading
const unsigned long CONFIG_POLL_INTERVAL_MS = 15000; // how often to poll config

// Sender AP
const char* SENDER_AP_SSID = "Sender-Direct";
const char* SENDER_AP_PASS = "senderpass";
const IPAddress SENDER_AP_IP(192,168,4,1);

// Router fallback (optional)
const bool TRY_ROUTER_FALLBACK = true;
const char* ROUTER_SSID = "Airtel_7737476759";
const char* ROUTER_PASS = "air49169";
const IPAddress ROUTER_SENDER_IP(192,168,1,50); // Sender STA IP when on router
/* ------------------------------------------------------- */

#define EEPROM_SIZE 128
#define EEPROM_ADDR 0
const uint32_t CONFIG_MAGIC = 0xA5A5A5A5;

typedef struct {
  char name[16];
  float totalHeightCm;
  float sensorToMaxCm;
  uint32_t magic;
} persisted_config_t;

persisted_config_t cfg;
unsigned long lastReport = 0;
unsigned long lastConfigPoll = 0;
uint32_t seqno = 0;

/* ---------------- EEPROM helpers ---------------- */
void saveConfigToEEPROM() {
  cfg.magic = CONFIG_MAGIC;
  EEPROM.begin(EEPROM_SIZE);
  uint8_t* p = (uint8_t*)&cfg;
  for (size_t i=0;i<sizeof(cfg);i++) EEPROM.write(EEPROM_ADDR + i, p[i]);
  EEPROM.commit();
  EEPROM.end();
  Serial.println("Config saved to EEPROM");
}

bool loadConfigFromEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
  uint8_t* p = (uint8_t*)&cfg;
  for (size_t i=0;i<sizeof(cfg);i++) p[i] = EEPROM.read(EEPROM_ADDR + i);
  EEPROM.end();
  if (cfg.magic == CONFIG_MAGIC) {
    cfg.name[sizeof(cfg.name)-1] = 0;
    return true;
  }
  return false;
}

/* ---------------- HC-SR04 helpers ---------------- */
float read_hcsr04_cm(unsigned long &duration_us) {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  // 38ms timeout (~6.5m)
  duration_us = pulseIn(ECHO_PIN, HIGH, 38000UL);
  if (duration_us == 0) return -1.0f;
  float cm = (duration_us / 2.0f) / 29.1f;
  return cm;
}

float compute_percent_from_distance(float measured_cm, float total_height_cm, float sensor_to_max_cm) {
  if (measured_cm < 0) return -1.0f;
  float d_max_to_surface = sensor_to_max_cm + measured_cm;
  float filled = total_height_cm - d_max_to_surface;
  if (filled < 0) filled = 0;
  if (filled > total_height_cm) filled = total_height_cm;
  return (filled / total_height_cm) * 100.0f;
}

/* ---------------- Network helpers ---------------- */
bool connectToSenderAP(unsigned long timeoutMs=5000) {
  Serial.printf("Connecting to Sender AP '%s' ...\n", SENDER_AP_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(SENDER_AP_SSID, SENDER_AP_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - t0) < timeoutMs) {
    Serial.print('.');
    delay(200);
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Connected to Sender AP. IP=%s  channel=%d\n", WiFi.localIP().toString().c_str(), WiFi.channel());
    return true;
  }
  Serial.println("Failed to join Sender AP");
  WiFi.disconnect(true);
  return false;
}

bool connectToRouter(unsigned long timeoutMs=8000) {
  Serial.printf("Trying router '%s' ...\n", ROUTER_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ROUTER_SSID, ROUTER_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - t0) < timeoutMs) {
    Serial.print('.');
    delay(250);
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Connected to router. IP=%s channel=%d\n", WiFi.localIP().toString().c_str(), WiFi.channel());
    return true;
  }
  Serial.println("Router connect failed");
  WiFi.disconnect(true);
  return false;
}

/* ---------------- HTTP report / config ---------------- */
String getMacString() {
  return WiFi.macAddress(); // "AA:BB:CC:DD:EE:FF"
}

bool postReport(float percent) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("No WiFi connection for report");
    return false;
  }

  String serverUrl;
  IPAddress local = WiFi.localIP();
  if (local[0] == 192 && local[1] == 168 && local[2] == 4) {
    serverUrl = String("http://") + SENDER_AP_IP.toString() + "/api/report";
  } else {
    serverUrl = String("http://") + ROUTER_SENDER_IP.toString() + "/api/report";
  }

  WiFiClient client;
  HTTPClient http;
  http.begin(client, serverUrl);  // new API (ESP8266 core >=3.0)
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<256> doc;
  doc["name"] = cfg.name;
  if (percent >= 0) doc["percent"] = percent;
  doc["seq"] = seqno++;
  doc["totalHeightCm"] = cfg.totalHeightCm;
  doc["sensorToMaxCm"] = cfg.sensorToMaxCm;
  doc["mac"] = getMacString();

  String payload;
  serializeJson(doc, payload);

  Serial.printf("POST %s -> %s\n", payload.c_str(), serverUrl.c_str());
  int httpCode = http.POST(payload);
  if (httpCode > 0) {
    String resp = http.getString();
    Serial.printf("HTTP %d, resp: %s\n", httpCode, resp.c_str());
    http.end();
    return (httpCode == 200 || httpCode == 201);
  } else {
    Serial.printf("HTTP POST failed, error: %s\n", http.errorToString(httpCode).c_str());
    http.end();
    return false;
  }
}

bool pollConfigFromServer() {
  if (WiFi.status() != WL_CONNECTED) return false;
  String serverUrl;
  IPAddress local = WiFi.localIP();
  if (local[0] == 192 && local[1] == 168 && local[2] == 4) {
    serverUrl = String("http://") + SENDER_AP_IP.toString() + "/api/config?name=" + String(cfg.name);
  } else {
    serverUrl = String("http://") + ROUTER_SENDER_IP.toString() + "/api/config?name=" + String(cfg.name);
  }

  WiFiClient client;
  HTTPClient http;
  http.begin(client, serverUrl);
  int httpCode = http.GET();
  if (httpCode == 200) {
    String body = http.getString();
    http.end();
    StaticJsonDocument<256> doc;
    auto err = deserializeJson(doc, body);
    if (!err) {
      const char* name = doc["name"] | "";
      float th = doc["totalHeightCm"] | cfg.totalHeightCm;
      float s2m = doc["sensorToMaxCm"] | cfg.sensorToMaxCm;
      bool changed = false;
      if (strlen(name) && strcmp(name, cfg.name) != 0) {
        strncpy(cfg.name, name, sizeof(cfg.name)-1);
        cfg.name[sizeof(cfg.name)-1] = 0;
        changed = true;
      }
      if (fabs(cfg.totalHeightCm - th) > 0.001) { cfg.totalHeightCm = th; changed = true; }
      if (fabs(cfg.sensorToMaxCm - s2m) > 0.001) { cfg.sensorToMaxCm = s2m; changed = true; }
      if (changed) {
        saveConfigToEEPROM();
        Serial.println("Config updated from server");
      } else Serial.println("Config poll: no changes");
      return true;
    } else {
      Serial.printf("Config JSON parse err: %s\n", err.c_str());
      return false;
    }
  } else {
    if (httpCode > 0) Serial.printf("Config poll HTTP %d\n", httpCode);
    else Serial.printf("Config poll failed: %s\n", http.errorToString(httpCode).c_str());
    http.end();
    return false;
  }
}

/* ---------------- setup / loop ---------------- */
void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println("\nESP8266 HTTP Sensor starting...");

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  if (!loadConfigFromEEPROM()) {
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.name, DEFAULT_NAME, sizeof(cfg.name)-1);
    cfg.totalHeightCm = 80.0f;
    cfg.sensorToMaxCm = 2.0f;
    cfg.magic = CONFIG_MAGIC;
    saveConfigToEEPROM();
    Serial.println("Wrote default config to EEPROM");
  } else {
    Serial.printf("Loaded config: name='%s' H=%.1f S2M=%.1f\n", cfg.name, cfg.totalHeightCm, cfg.sensorToMaxCm);
  }

  // Try connect to Sender AP first
  bool joinedAP = connectToSenderAP(5000);
  if (!joinedAP && TRY_ROUTER_FALLBACK) {
    bool joinedRouter = connectToRouter(8000);
    if (!joinedRouter) {
      Serial.println("No WiFi connection available. Will retry in loop.");
    }
  }

  lastReport = millis();
  lastConfigPoll = millis();
}

void loop() {
  // ensure wifi; try to connect if not connected
  if (WiFi.status() != WL_CONNECTED) {
    // try Sender AP briefly
    if (!connectToSenderAP(3000)) {
      if (TRY_ROUTER_FALLBACK) connectToRouter(5000);
    }
  }

  unsigned long now = millis();

  if (now - lastReport >= REPORT_INTERVAL_MS) {
    lastReport = now;
    unsigned long dur;
    float dcm = read_hcsr04_cm(dur);
    float pct = -1;
    if (dcm < 0) {
      Serial.println("HC-SR04 timeout");
    } else {
      pct = compute_percent_from_distance(dcm, cfg.totalHeightCm, cfg.sensorToMaxCm);
      Serial.printf("Measured %.2f cm => %.1f%% (raw %lu us)\n", dcm, pct, dur);
    }
    bool ok = postReport(pct);
    if (!ok) Serial.println("Report failed");
  }

  if (now - lastConfigPoll >= CONFIG_POLL_INTERVAL_MS) {
    lastConfigPoll = now;
    bool ok = pollConfigFromServer();
    if (!ok) Serial.println("Config poll failed or no change");
  }

  yield(); // allow background tasks
}
