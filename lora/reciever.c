#include <SPI.h>
#include <LoRa.h>
#include <Wire.h>
#include <hd44780.h>
#include <hd44780ioClass/hd44780_I2Cexp.h>
#include <WiFi.h>  // only used to read MAC address

hd44780_I2Cexp lcd;

const long LORA_FREQ = 433E6;   // SX1278 = 433 MHz
const int ssPin    = 5;         // NSS / CS
const int resetPin = 14;        // RST
const int dio0Pin  = 26;        // DIO0
const int LORA_LED = 2;         // ESP32 onboard LED (GPIO2)

// Config
const int NUM_TANKS = 6;
const unsigned long PAGE_DELAY_MS = 3000;
const unsigned long STALE_MS = 8000;
const unsigned long RECENT_MS = 1000;

// Tank storage
struct Slot { char name[16]; float levelPercent; unsigned long lastUpdate; };
Slot slots[NUM_TANKS];
volatile bool anyDataReceived = false;
unsigned long lastPageMs = 0;
int currentPage = 0;

// Packet struct (must match sender)
struct TankData { char name[16]; float levelPercent; };
struct StructMessage { TankData tanks[NUM_TANKS]; };

// --- Helpers ---
void initSlots() {
  for (int i = 0; i < NUM_TANKS; i++) {
    slots[i].name[0] = 0;
    slots[i].levelPercent = -1;
    slots[i].lastUpdate = 0;
  }
}
void safePrintLine(int row, const char* txt) {
  lcd.setCursor(0, row);
  char buf[21]; memset(buf,' ',20); buf[20] = 0;
  int len = strlen(txt); if (len > 20) len = 20;
  memcpy(buf, txt, len);
  lcd.print(buf);
}
void showNoDataInfo() {
  lcd.clear();
  safePrintLine(0, "No Data");
  safePrintLine(1, WiFi.macAddress().c_str());
  char l2[21]; snprintf(l2,sizeof(l2),"LoRa %.0f MHz", (double)LORA_FREQ/1e6);
  safePrintLine(2, l2);
  safePrintLine(3, "Waiting for LoRa...");
}
void showTankPage(int idx) {
  lcd.clear();
  if (slots[idx].name[0] == '\0') {
    char s[21]; snprintf(s,sizeof(s),"Tank %d (empty)", idx+1);
    safePrintLine(0,s);
    safePrintLine(1,"No data received");
    safePrintLine(2,"");
    safePrintLine(3,"");
    return;
  }
  safePrintLine(0, slots[idx].name);
  unsigned long now = millis();
  unsigned long age = (slots[idx].lastUpdate==0)? ULONG_MAX : (now - slots[idx].lastUpdate);
  if (slots[idx].lastUpdate == 0 || age > STALE_MS) {
    safePrintLine(1,"No data received");
    safePrintLine(2,"");
    safePrintLine(3,"");
    return;
  }
  // Level
  char line1[21]; char pct[12];
  if (slots[idx].levelPercent < 0) strcpy(pct,"--.-");
  else dtostrf(slots[idx].levelPercent,5,1,pct);
  snprintf(line1,sizeof(line1),"Level: %s %%", pct);
  safePrintLine(1, line1);
  safePrintLine(2,"");
  char line3[21];
  if (age <= RECENT_MS) snprintf(line3,sizeof(line3),"Updated: <1s ago");
  else {
    unsigned long secs = age/1000;
    if (secs < 60) snprintf(line3,sizeof(line3),"Updated: %lus ago", secs);
    else snprintf(line3,sizeof(line3),"Updated: %lumin ago", secs/60);
  }
  safePrintLine(3, line3);
}

// --- LoRa receive ---
void onLoRaReceivePacket() {
  int packetSize = LoRa.parsePacket();
  if (packetSize <= 0) return;
  if (packetSize != sizeof(StructMessage)) {
    Serial.printf("LoRa pkt size mismatch %d != %d\n", packetSize, (int)sizeof(StructMessage));
    while (LoRa.available()) LoRa.read();
    return;
  }
  StructMessage msg;
  int idx = 0;
  uint8_t *buf = (uint8_t*)&msg;
  while (LoRa.available() && idx < (int)sizeof(msg)) buf[idx++] = (uint8_t)LoRa.read();
  if (idx != (int)sizeof(msg)) { Serial.println("Read size mismatch"); return; }
  unsigned long now = millis();
  Serial.println("LoRa packet received:");
  for (int i=0; i<NUM_TANKS; i++) {
    msg.tanks[i].name[15]=0;
    strncpy(slots[i].name, msg.tanks[i].name, sizeof(slots[i].name));
    slots[i].name[sizeof(slots[i].name)-1]=0;
    slots[i].levelPercent = msg.tanks[i].levelPercent;
    slots[i].lastUpdate = now;
    Serial.printf(" %d) %s = %.1f%%\n", i+1, slots[i].name, slots[i].levelPercent);
  }
  anyDataReceived = true;
  // Blink LED on packet received
  digitalWrite(LORA_LED, HIGH);
  delay(50);
  digitalWrite(LORA_LED, LOW);
}

void setup() {
  Serial.begin(115200);
  while(!Serial) delay(10);
  Serial.println("SX1278 Receiver starting...");

  pinMode(LORA_LED, OUTPUT);

  Wire.begin(21,22);
  lcd.begin(20,4);
  lcd.backlight();
  lcd.clear();

  initSlots();

  // SPI and LoRa init
  SPI.begin(18, 19, 23);
  LoRa.setPins(ssPin, resetPin, dio0Pin);
  Serial.printf("Init LoRa at %.0f MHz\n", (double)LORA_FREQ/1e6);
  if (!LoRa.begin(LORA_FREQ)) {
    Serial.println("LoRa init failed - check wiring/freq");
    while(true) delay(1000);
  }
  Serial.println("LoRa ready (SX1278)");

  showNoDataInfo();
  lastPageMs = millis();
}

void loop() {
  onLoRaReceivePacket();
  unsigned long now = millis();

  if (!anyDataReceived) {
    static unsigned long lastRefresh=0;
    if (now - lastRefresh > 1000) { showNoDataInfo(); lastRefresh = now; }
    return;
  }

  if (now - lastPageMs >= PAGE_DELAY_MS) {
    currentPage++;
    if (currentPage >= NUM_TANKS) currentPage = 0;
    showTankPage(currentPage);
    lastPageMs = now;
  } else {
    static unsigned long lastRefresh=0;
    if (now - lastRefresh >= 1000) { showTankPage(currentPage); lastRefresh = now; }
  }
}
