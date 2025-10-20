// Receiver (static IP) - UDP listener -> display one tank per page on 20x4 LCD
#include <Wire.h>
#include <hd44780.h>
#include <hd44780ioClass/hd44780_I2Cexp.h>
#include <WiFi.h>
#include <WiFiUdp.h>

hd44780_I2Cexp lcd;

// ====== NETWORK CONFIG - EDIT THESE ======
const char* WIFI_SSID = "Airtel_7737476759";
const char* WIFI_PASS = "air49169";

// receiver static IP settings (edit for your LAN)
IPAddress RECV_LOCAL_IP(192,168,29,100); // receiver static IP
IPAddress RECV_GATEWAY(192,168,29,1);
IPAddress RECV_SUBNET(255,255,255,0);

const uint16_t UDP_PORT = 4210; // port to listen on

// ====== APPLICATION CONFIG ======
const int NUM_TANKS = 6;
const unsigned long PAGE_DELAY_MS = 3000;
const unsigned long STALE_MS = 8000;
const unsigned long RECENT_MS = 1000;

// ====== PACKET STRUCT (must match sender) ======
struct TankData { char name[16]; float levelPercent; };
struct StructMessage { TankData tanks[NUM_TANKS]; };

// ====== INTERNAL STORAGE ======
struct Slot {
  char name[16];
  float levelPercent;
  unsigned long lastUpdate;
};
Slot slots[NUM_TANKS];
volatile bool anyDataReceived = false;

WiFiUDP Udp;

// --- helpers
void initSlots() {
  for (int i=0;i<NUM_TANKS;i++){
    slots[i].name[0] = '\0';
    slots[i].levelPercent = -1.0f;
    slots[i].lastUpdate = 0;
  }
}

void safePrintLine(int row, const char* txt){
  lcd.setCursor(0,row);
  char buf[21];
  memset(buf,' ',20);
  buf[20]='\0';
  int len = strlen(txt);
  if (len>20) len = 20;
  memcpy(buf, txt, len);
  lcd.print(buf);
}

// === SHOW NO DATA SCREEN (with MAC + IP) ===
void showNoDataInfo(){
  lcd.clear();
  safePrintLine(0,"No Data");
  // MAC on line 1
  safePrintLine(1, WiFi.macAddress().c_str());
  // IP on line 2
  String ip = WiFi.localIP().toString();
  safePrintLine(2, ip.c_str());
  // Waiting message on line 3
  safePrintLine(3,"Waiting for packets...");
}

void showTankPage(int idx){
  lcd.clear();
  if (slots[idx].name[0] == '\0') {
    char s[21];
    snprintf(s,sizeof(s),"Tank %d (empty)", idx+1);
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
    safePrintLine(1, "No data received");
    safePrintLine(2, "");
    safePrintLine(3, "");
    return;
  }

  // Level line
  char line1[21];
  char pct[12];
  if (slots[idx].levelPercent < 0) strcpy(pct, "--.-");
  else dtostrf(slots[idx].levelPercent, 5, 1, pct);
  snprintf(line1, sizeof(line1), "Level: %s %%", pct);
  safePrintLine(1, line1);
  safePrintLine(2, "");

  // Updated age
  char line3[21];
  if (age <= RECENT_MS) snprintf(line3,sizeof(line3),"Updated: <1s ago");
  else {
    unsigned long secs = age / 1000;
    if (secs < 60) snprintf(line3,sizeof(line3),"Updated: %lus ago", secs);
    else {
      unsigned long mins = secs/60;
      snprintf(line3,sizeof(line3),"Updated: %lumin ago", mins);
    }
  }
  safePrintLine(3,line3);
}

// ====== UDP receive handler ======
void handleUdp() {
  int packetSize = Udp.parsePacket();
  if (packetSize <= 0) return;
  if (packetSize != (int)sizeof(StructMessage)) {
    Serial.printf("UDP packet size mismatch %d != %d\n", packetSize, (int)sizeof(StructMessage));
    Udp.read(); 
    return;
  }
  StructMessage msg;
  int len = Udp.read((uint8_t*)&msg, sizeof(msg));
  if (len != (int)sizeof(msg)) { Serial.println("UDP read size mismatch"); return; }
  unsigned long now = millis();
  for (int i=0;i<NUM_TANKS;i++){
    msg.tanks[i].name[15]=0;
    strncpy(slots[i].name, msg.tanks[i].name, sizeof(slots[i].name));
    slots[i].name[sizeof(slots[i].name)-1]=0;
    slots[i].levelPercent = msg.tanks[i].levelPercent;
    slots[i].lastUpdate = now;
    Serial.printf("Slot %d <= %s = %.1f\n", i, slots[i].name, slots[i].levelPercent);
  }
  anyDataReceived = true;
}

// ====== SETUP / LOOP ======
unsigned long lastPageMs = 0;
int currentPage = 0;
unsigned long lastRefresh = 0;
void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println("Receiver (UDP) starting...");

  Wire.begin(21,22);
  lcd.begin(20,4);
  lcd.backlight();
  lcd.clear();

  initSlots();

  // configure static IP
  if (!WiFi.config(RECV_LOCAL_IP, RECV_GATEWAY, RECV_SUBNET)) {
    Serial.println("WiFi.config() failed");
  }
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("Connecting to %s ...\n", WIFI_SSID);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis()-start < 15000) {
    delay(200); Serial.print(".");
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWiFi connect failed");
    safePrintLine(0,"WiFi failed");
    while(true) delay(1000);
  }
  Serial.println("\nWiFi connected");
  Serial.print("IP: "); Serial.println(WiFi.localIP());
  Serial.print("MAC: "); Serial.println(WiFi.macAddress());

  if (Udp.begin(UDP_PORT) != 1) {
    Serial.printf("Udp.begin(%d) failed\n", UDP_PORT);
  } else Serial.printf("Listening UDP port %d\n", UDP_PORT);

  // initial info screen
  showNoDataInfo();
  lastPageMs = millis();
  lastRefresh = millis();
}

void loop() {
  handleUdp();

  unsigned long now = millis();
  if (!anyDataReceived) {
    if (now - lastRefresh > 1000) { showNoDataInfo(); lastRefresh = now; }
    return;
  }

  // paging
  if (now - lastPageMs >= PAGE_DELAY_MS) {
    currentPage++;
    if (currentPage >= NUM_TANKS) currentPage = 0;
    showTankPage(currentPage);
    lastPageMs = now;
    lastRefresh = now;
  } else {
    if (now - lastRefresh >= 1000) { showTankPage(currentPage); lastRefresh = now; }
  }
}
