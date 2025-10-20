/* Sender for SX1278 (433 MHz) - sends one packet with 6 TankData entries via LoRa
   - Ensure wiring per pin description above
   - Requires "LoRa" library by Sandeep Mistry
*/

#include <SPI.h>
#include <LoRa.h>

// ----- LoRa hardware pins (change if your wiring differs) -----
const long LORA_FREQ = 433E6;   // SX1278 typical frequency
const int ssPin = 5;            // NSS / CS
const int resetPin = 14;        // RST
const int dio0Pin = 26;         // DIO0

// ----- Sensor pins & config (unchanged) -----
#define TRIG_PIN 4
const int echoPins[6] = {16, 17, 18, 19, 21, 22};

struct TankCfg { const char* name; float tankHeight; float offsetFull; };
TankCfg tankCfg[6] = {
  {"Tank A", 90.0f, 5.0f},
  {"Tank B",125.0f, 3.0f},
  {"Tank C",110.0f, 2.0f},
  {"Tank D",150.0f, 4.0f},
  {"Tank E",200.0f, 6.0f},
  {"Tank F",175.0f, 2.5f}
};

// ----- Packet struct -----
struct TankData { char name[16]; float levelPercent; };
struct StructMessage { TankData tanks[6]; };

// ----- Ultrasonic read stuff (same optimized functions) -----
const unsigned int TRIG_PULSE_US = 10;
const int SAMPLES = 5;
const unsigned long SAMPLE_DELAY_MS = 60;
const unsigned long SENSOR_GAP_MS = 100;
const float SOUND_SPEED = 0.0343f;
const float MAX_MEASURE_DIST_CM = 400.0f;

unsigned long timeoutForDistanceCm(float maxDistCm) {
  unsigned long t = (unsigned long)((2.0f * maxDistCm) / SOUND_SPEED);
  if (t < 30000ul) return 30000ul;
  if (t > 300000ul) return 300000ul;
  return t;
}
int medianInt(int arr[], int n) {
  for (int i = 1; i < n; ++i) {
    int key = arr[i]; int j = i - 1;
    while (j >= 0 && arr[j] > key) { arr[j+1] = arr[j]; --j; }
    arr[j+1] = key;
  }
  return arr[n/2];
}
float measureOnce(int echoPin, unsigned long timeout_us) {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(TRIG_PULSE_US);
  digitalWrite(TRIG_PIN, LOW);

  unsigned long duration = pulseIn(echoPin, HIGH, timeout_us);
  if (duration == 0) return -1.0f;
  float distance = (duration * SOUND_SPEED) / 2.0f;
  if (distance < 2.0f || distance > (MAX_MEASURE_DIST_CM + 50.0f)) return -1.0f;
  return distance;
}
float readDistanceOptimized(int echoPin, float maxDistanceCm = MAX_MEASURE_DIST_CM) {
  unsigned long timeout_us = timeoutForDistanceCm(maxDistanceCm);
  int results[SAMPLES];
  for (int s = 0; s < SAMPLES; ++s) {
    float d = measureOnce(echoPin, timeout_us);
    results[s] = (d < 0) ? (int)(maxDistanceCm * 10.0f) : (int)(d * 10.0f);
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
  waterHeight = constrain(waterHeight, 0.0f, tankHeight);
  float pct = (waterHeight / tankHeight) * 100.0f;
  return constrain(pct, 0.0f, 100.0f);
}

// ----- setup & loop -----
void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);
  Serial.println("\nSX1278 Sender starting...");

  pinMode(TRIG_PIN, OUTPUT);
  for (int i = 0; i < 6; ++i) pinMode(echoPins[i], INPUT_PULLDOWN);

  // SPI begin (HSPI default pins)
  SPI.begin(18, 19, 23); // SCK, MISO, MOSI
  LoRa.setPins(ssPin, resetPin, dio0Pin);

  Serial.printf("Init LoRa at %.0f MHz ...\n", (double)LORA_FREQ/1e6);
  if (!LoRa.begin(LORA_FREQ)) {
    Serial.println("LoRa init failed - check wiring and freq (SX1278 433MHz).");
    while (true) delay(1000);
  }

  // Optional tuning (example): increase range by higher SF and narrow BW (slower)
  // LoRa.setSpreadingFactor(10);      // 7..12 (higher = longer range, slower)
  // LoRa.setSignalBandwidth(125E3);   // 7.8k..500k
  // LoRa.setCodingRate4(5);           // 5..8 (lower = more robust)
  // LoRa.setTxPower(17);              // 2..20 dBm depending on module

  Serial.println("LoRa ready (SX1278 433MHz)");
}

void loop() {
  StructMessage msg;

  for (int i = 0; i < 6; ++i) {
    float dist = readDistanceOptimized(echoPins[i], MAX_MEASURE_DIST_CM);
    float pct = calcLevelPercent(dist, tankCfg[i].tankHeight, tankCfg[i].offsetFull);
    strncpy(msg.tanks[i].name, tankCfg[i].name, sizeof(msg.tanks[i].name));
    msg.tanks[i].name[sizeof(msg.tanks[i].name)-1] = '\0';
    msg.tanks[i].levelPercent = pct;
    if (dist < 0) Serial.printf("%s: No echo -> %.1f\n", msg.tanks[i].name, pct);
    else Serial.printf("%s: Dist=%.1f cm => %.1f%%\n", msg.tanks[i].name, dist, pct);
    delay(SENSOR_GAP_MS);
  }

  // Send the whole struct as one LoRa packet
  Serial.println("Sending LoRa packet...");
  LoRa.beginPacket();
  LoRa.write((uint8_t*)&msg, sizeof(msg));
  LoRa.endPacket(); // non-blocking
  Serial.println("Packet sent via LoRa");

  delay(2000); // adjust as needed
}
