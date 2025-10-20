/*
  Sender (UDP batch) for 6-tank system
  - Connects to existing Wi-Fi (SSID/PASS)
  - Measures 6 ultrasonic sensors (common TRIG, separate ECHOs)
  - Packs all 6 TankData entries into one StructMessage and sends via UDP
  - Matches receiver's packet layout:
      struct TankData { char name[16]; float levelPercent; };
      struct StructMessage { TankData tanks[6]; };
  - Edit WIFI_SSID, WIFI_PASS, receiverIp and UDP_PORT below before uploading.
*/

#include <WiFi.h>
#include <WiFiUdp.h>

// ====== USER CONFIG (EDIT) ======
const char* WIFI_SSID = "Airtel_7737476759";
const char* WIFI_PASS = "air49169";

IPAddress receiverIp(192,168,1,100); // receiver static IP (edit)
const uint16_t UDP_PORT = 4210;       // receiver UDP port (must match receiver)

// ====== PINS & TANK CONFIG ======
#define TRIG_PIN 4
const int echoPins[6] = {16, 17, 18, 19, 21, 22}; // ECHO pins per tank

struct TankCfg {
  const char* name;
  float tankHeight;   // cm
  float offsetFull;   // cm (sensor to full water level)
};

TankCfg tankCfg[6] = {
  {"Kitchen", 90.0f, 21.0f},
  {"Ground 1", 82.0f, 21.0f},
  {"Ground 2", 90.0f, 21.0f},
  {"First Single Room", 74.0f, 21.0f},
  {"First 2bhk", 91.0f, 33.0f},
  {"Yellow", 95.0f, 34.5f}
};


// ====== PACKET STRUCT ======
struct TankData { char name[16]; float levelPercent; };
struct StructMessage { TankData tanks[6]; };

// ====== ULTRASONIC / SAMPLING PARAMS ======
const unsigned int TRIG_PULSE_US = 10;      // 10 µs trigger
const int SAMPLES = 5;                      // odd -> median
const unsigned long SAMPLE_DELAY_MS = 60;   // ms between samples for same sensor
const unsigned long SENSOR_GAP_MS = 100;    // ms gap between sensors (reduce cross-talk)
const float SOUND_SPEED = 0.0343f;          // cm per microsecond
const float MAX_MEASURE_DIST_CM = 400.0f;   // used to compute timeout & sentinel

// compute timeout (µs) for given distance (round trip)
unsigned long timeoutForDistanceCm(float maxDistCm) {
  unsigned long t = (unsigned long)((2.0f * maxDistCm) / SOUND_SPEED);
  if (t < 30000ul) return 30000ul;
  if (t > 300000ul) return 300000ul;
  return t;
}

// small insertion-sort median for small arrays
int medianInt(int arr[], int n) {
  for (int i = 1; i < n; ++i) {
    int key = arr[i];
    int j = i - 1;
    while (j >= 0 && arr[j] > key) {
      arr[j + 1] = arr[j];
      --j;
    }
    arr[j + 1] = key;
  }
  return arr[n/2];
}

// single measurement using pulseIn (returns -1 on timeout/invalid)
float measureOnce(int echoPin, unsigned long timeout_us) {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(TRIG_PULSE_US);
  digitalWrite(TRIG_PIN, LOW);

  unsigned long duration = pulseIn(echoPin, HIGH, timeout_us);
  if (duration == 0) return -1.0f;
  float distance = (duration * SOUND_SPEED) / 2.0f; // cm
  // sanity filter
  if (distance < 2.0f || distance > (MAX_MEASURE_DIST_CM + 50.0f)) return -1.0f;
  return distance;
}

// robust measurement: median of SAMPLES (returns -1 if no valid)
float readDistanceOptimized(int echoPin, float maxDistanceCm = MAX_MEASURE_DIST_CM) {
  unsigned long timeout_us = timeoutForDistanceCm(maxDistanceCm);
  int results[SAMPLES];
  for (int s = 0; s < SAMPLES; ++s) {
    float d = measureOnce(echoPin, timeout_us);
    if (d < 0) results[s] = (int)(maxDistanceCm * 10.0f); // sentinel in tenths of cm
    else results[s] = (int)(d * 10.0f);
    delay(SAMPLE_DELAY_MS);
  }
  int med = medianInt(results, SAMPLES);
  float medianCm = ((float)med) / 10.0f;
  if (med >= (int)(maxDistanceCm * 10.0f)) return -1.0f;
  return medianCm;
}

float calcLevelPercent(float measuredDist, float tankHeight, float offsetFull) {
  if (measuredDist < 0) return -1.0f;
  float waterHeight = tankHeight - (measuredDist - offsetFull);
  if (waterHeight < 0) waterHeight = 0;
  if (waterHeight > tankHeight) waterHeight = tankHeight;
  float pct = (waterHeight / tankHeight) * 100.0f;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return pct;
}

// ====== NETWORK ======
WiFiUDP Udp;

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("UDP Sender (batch) starting...");

  // pins
  pinMode(TRIG_PIN, OUTPUT);
  for (int i = 0; i < 6; ++i) pinMode(echoPins[i], INPUT_PULLDOWN); // enable internal pulldown

  // WiFi connect (DHCP)
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("Connecting to WiFi '%s' ...\n", WIFI_SSID);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    Serial.print(".");
    delay(200);
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWiFi connect failed - check SSID/PASS");
    while (true) delay(1000);
  }
  Serial.println("\nWiFi connected");
  Serial.print("Local IP: "); Serial.println(WiFi.localIP());
  Serial.print("Sending to: "); Serial.print(receiverIp); Serial.print(":"); Serial.println(UDP_PORT);

  // optional: begin local UDP (not required to send)
  Udp.begin(0);
}

void loop() {
  StructMessage msg;

  // measure sequentially and fill struct
  for (int i = 0; i < 6; ++i) {
    float dist = readDistanceOptimized(echoPins[i], MAX_MEASURE_DIST_CM);
    float pct = calcLevelPercent(dist, tankCfg[i].tankHeight, tankCfg[i].offsetFull);

    strncpy(msg.tanks[i].name, tankCfg[i].name, sizeof(msg.tanks[i].name));
    msg.tanks[i].name[sizeof(msg.tanks[i].name)-1] = '\0';
    msg.tanks[i].levelPercent = pct;

    if (dist < 0) Serial.printf("%s: No echo -> %.1f%%\n", msg.tanks[i].name, pct);
    else Serial.printf("%s: Dist=%.1f cm => %.1f%%\n", msg.tanks[i].name, dist, pct);

    // gap to avoid cross-talk
    delay(SENSOR_GAP_MS);
  }

  // send packet to receiver
  Udp.beginPacket(receiverIp, UDP_PORT);
  Udp.write((const uint8_t *)&msg, sizeof(msg));
  Udp.endPacket();
  Serial.println("Batch UDP sent");

  delay(2000); // pause between cycles (adjust as needed)
}
