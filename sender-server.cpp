/*
  Sender_ESP32.ino
  - SoftAP + STA (static STA IP by default)
  - HTTP API for sensors and web UI
  - Persist device configs to LittleFS
  - No WebSockets; clients poll API
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <esp_wifi.h>
#include <ESPmDNS.h>

/* ---------------- CONFIG ---------------- */
const char* AP_SSID = "Sender-Direct";
const char* AP_PASS = "senderpass";
const uint8_t AP_CHANNEL = 6;
const IPAddress AP_IP(192,168,4,1);
const IPAddress AP_NETMASK(255,255,255,0);

const bool trySTA = true;
const bool useStaticIP = true;
const char* STA_SSID = "Airtel_7737476759";
const char* STA_PASS = "air49169";
IPAddress STA_LOCAL_IP(192,168,1,50);
IPAddress STA_GATEWAY(192,168,1,1);
IPAddress STA_SUBNET(255,255,255,0);
IPAddress STA_DNS1(8,8,8,8);
IPAddress STA_DNS2(8,8,4,4);

const int HTTP_PORT = 80;
const char* DEVICES_FILE = "/devices.json";
const int MAX_DEVICES = 128;
const unsigned long ACTIVE_THRESHOLD_SEC = 15; // for receiver display to consider active
/* ---------------------------------------- */

WebServer server(HTTP_PORT);

struct Device {
  bool used;
  bool macKnown;
  uint8_t mac[6];
  IPAddress ip;
  int8_t rssi;
  char name[32];
  float percent;
  float totalHeightCm;
  float sensorToMaxCm;
  unsigned long lastSeen;
};

Device devices[MAX_DEVICES];

/* LittleFS helpers */
bool initFileSystem() {
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS begin failed!");
    return false;
  }
  return true;
}

bool saveDevicesToFS() {
  StaticJsonDocument<16384> doc;
  JsonArray arr = doc.createNestedArray("devices");
  for (int i=0;i<MAX_DEVICES;i++){
    if (!devices[i].used) continue;
    JsonObject o = arr.createNestedObject();
    if (devices[i].macKnown) {
      char macs[18];
      sprintf(macs, "%02X:%02X:%02X:%02X:%02X:%02X",
              devices[i].mac[0],devices[i].mac[1],devices[i].mac[2],
              devices[i].mac[3],devices[i].mac[4],devices[i].mac[5]);
      o["mac"] = macs;
    } else o["mac"] = nullptr;
    o["name"] = devices[i].name[0] ? devices[i].name : nullptr;
    o["totalHeightCm"] = devices[i].totalHeightCm;
    o["sensorToMaxCm"] = devices[i].sensorToMaxCm;
  }
  File f = LittleFS.open(DEVICES_FILE, "w");
  if (!f) { Serial.println("Failed open devices file for write"); return false; }
  if (serializeJsonPretty(doc, f) == 0) { Serial.println("Failed writing JSON"); f.close(); return false; }
  f.close();
  Serial.println("Saved devices to LittleFS");
  return true;
}

bool loadDevicesFromFS() {
  if (!LittleFS.exists(DEVICES_FILE)) {
    Serial.println("devices.json not found; starting fresh");
    return false;
  }
  File f = LittleFS.open(DEVICES_FILE, "r");
  if (!f) { Serial.println("Failed to open devices file"); return false; }
  size_t sz = f.size();
  std::unique_ptr<char[]> buf(new char[sz+1]);
  f.readBytes(buf.get(), sz);
  buf[sz] = 0;
  f.close();

  StaticJsonDocument<16384> doc;
  auto err = deserializeJson(doc, buf.get());
  if (err) { Serial.printf("Failed parse devices.json: %s\n", err.c_str()); return false; }

  for (int i=0;i<MAX_DEVICES;i++) devices[i].used = false;

  if (doc.containsKey("devices")) {
    JsonArray arr = doc["devices"].as<JsonArray>();
    int idx = 0;
    for (JsonObject o : arr) {
      if (idx >= MAX_DEVICES) break;
      devices[idx].used = true;
      devices[idx].macKnown = false;
      memset(devices[idx].mac,0,6);
      const char* macs = o["mac"] | "";
      if (macs && strlen(macs) >= 17) {
        unsigned int b[6];
        if (sscanf(macs, "%02X:%02X:%02X:%02X:%02X:%02X",
                   &b[0],&b[1],&b[2],&b[3],&b[4],&b[5]) == 6) {
          for (int k=0;k<6;k++) devices[idx].mac[k] = (uint8_t)b[k];
          devices[idx].macKnown = true;
        }
      }
      const char* name = o["name"] | "";
      strncpy(devices[idx].name, name, sizeof(devices[idx].name)-1);
      devices[idx].totalHeightCm = o["totalHeightCm"] | 0.0f;
      devices[idx].sensorToMaxCm = o["sensorToMaxCm"] | 0.0f;
      devices[idx].percent = -1;
      devices[idx].ip = IPAddress(0,0,0,0);
      devices[idx].rssi = 0;
      devices[idx].lastSeen = 0;
      idx++;
    }
  }
  Serial.println("Loaded devices from LittleFS");
  return true;
}

/* helpers */
String macToString(const uint8_t* mac) {
  char buf[18];
  sprintf(buf, "%02X:%02X:%02X:%02X:%02X:%02X",
          mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
  return String(buf);
}

int findDeviceByMAC(const uint8_t mac[6]) {
  for (int i=0;i<MAX_DEVICES;i++) {
    if (!devices[i].used) continue;
    if (devices[i].macKnown && memcmp(devices[i].mac, mac, 6)==0) return i;
  }
  return -1;
}

int findDeviceByName(const char* name) {
  for (int i=0;i<MAX_DEVICES;i++) {
    if (!devices[i].used) continue;
    if (strlen(devices[i].name) && strcmp(devices[i].name, name)==0) return i;
  }
  return -1;
}

int findFreeSlot() {
  for (int i=0;i<MAX_DEVICES;i++) if (!devices[i].used) return i;
  return -1;
}

/* try to refresh AP station list so we at least mark 'lastSeen' for connected AP clients */
void refreshConnectedStations() {
  wifi_sta_list_t sta_list;
  memset(&sta_list, 0, sizeof(sta_list));
  esp_err_t r = esp_wifi_ap_get_sta_list(&sta_list);
  if (r != ESP_OK) return;
  unsigned long now = millis();
  for (int i=0;i<sta_list.num;i++) {
    wifi_sta_info_t s = sta_list.sta[i];
    int idx = findDeviceByMAC(s.mac);
    if (idx == -1) {
      idx = findFreeSlot();
      if (idx == -1) continue;
      devices[idx].used = true;
      devices[idx].macKnown = true;
      memcpy(devices[idx].mac, s.mac, 6);
      devices[idx].name[0]=0;
      devices[idx].percent = -1;
      devices[idx].totalHeightCm = 0;
      devices[idx].sensorToMaxCm = 0;
      devices[idx].ip = IPAddress(0,0,0,0);
      devices[idx].rssi = 0;
    }
    devices[idx].lastSeen = now;
  }
}

/* HTTP handlers */

// POST /api/report  { name, percent, totalHeightCm, sensorToMaxCm, mac (optional) }
void handleReport() {
  if (server.method() != HTTP_POST) { server.send(405); return; }
  String body = server.arg("plain");
  if (body.length() == 0) { server.send(400, "text/plain", "empty"); return; }
  StaticJsonDocument<512> doc;
  auto err = deserializeJson(doc, body);
  if (err) { server.send(400, "text/plain", "json"); return; }
  const char* name = doc["name"] | "";
  float percent = doc["percent"] | -1.0f;
  float totalH = doc["totalHeightCm"] | 0.0f;
  float s2m = doc["sensorToMaxCm"] | 0.0f;
  const char* macs = doc["mac"] | "";

  int idx = -1;
  uint8_t macBuf[6] = {0};
  if (macs && strlen(macs) >= 17) {
    unsigned int b[6];
    if (sscanf(macs, "%02X:%02X:%02X:%02X:%02X:%02X",
               &b[0],&b[1],&b[2],&b[3],&b[4],&b[5])==6) {
      for (int k=0;k<6;k++) macBuf[k] = (uint8_t)b[k];
      idx = findDeviceByMAC(macBuf);
      if (idx == -1) {
        int free = findFreeSlot();
        if (free != -1) {
          idx = free;
          devices[idx].used = true;
          devices[idx].macKnown = true;
          memcpy(devices[idx].mac, macBuf, 6);
        }
      }
    }
  }

  if (idx == -1 && name && strlen(name)) idx = findDeviceByName(name);
  if (idx == -1) {
    idx = findFreeSlot();
    if (idx == -1) { server.send(500, "application/json", "{\"ok\":false,\"msg\":\"table-full\"}"); return; }
    devices[idx].used = true;
    devices[idx].macKnown = false;
    memset(devices[idx].mac,0,6);
  }

  if (macs && strlen(macs) >= 17) {
    devices[idx].macKnown = true;
    memcpy(devices[idx].mac, macBuf, 6);
  }
  if (name && strlen(name)) strncpy(devices[idx].name, name, sizeof(devices[idx].name)-1);
  devices[idx].percent = percent;
  devices[idx].totalHeightCm = totalH;
  devices[idx].sensorToMaxCm = s2m;
  devices[idx].lastSeen = millis();
  devices[idx].ip = server.client().remoteIP();

  Serial.printf("Report: idx=%d name=%s mac=%s ip=%s pct=%.1f\n", idx, devices[idx].name,
                devices[idx].macKnown?macToString(devices[idx].mac).c_str():"unknown",
                devices[idx].ip.toString().c_str(),
                devices[idx].percent);

  server.send(200, "application/json", "{\"ok\":true}");
}

// GET /api/devices
void handleGetDevices() {
  refreshConnectedStations();
  StaticJsonDocument<16384> arrdoc;
  JsonArray arr = arrdoc.to<JsonArray>();
  unsigned long now = millis();
  for (int i=0;i<MAX_DEVICES;i++) {
    if (!devices[i].used) continue;
    JsonObject o = arr.createNestedObject();
    if (devices[i].macKnown) o["mac"] = macToString(devices[i].mac);
    else o["mac"] = nullptr;
    o["ip"] = devices[i].ip.toString();
    o["rssi"] = devices[i].rssi;
    o["name"] = devices[i].name[0] ? devices[i].name : nullptr;
    if (devices[i].percent >= 0) o["percent"] = devices[i].percent; else o["percent"] = nullptr;
    if (devices[i].lastSeen==0) o["age_seconds"] = nullptr; else o["age_seconds"] = (now - devices[i].lastSeen) / 1000UL;
    o["totalHeightCm"] = devices[i].totalHeightCm;
    o["sensorToMaxCm"] = devices[i].sensorToMaxCm;
  }
  String out; serializeJson(arr, out);
  server.send(200, "application/json", out);
}

// POST /api/device (save config) { name, totalHeightCm, sensorToMaxCm, mac (optional) }
void handleSaveDevice() {
  if (server.method() != HTTP_POST) { server.send(405); return; }
  String body = server.arg("plain");
  if (body.length() == 0) { server.send(400, "text/plain", "empty"); return; }
  StaticJsonDocument<512> doc;
  auto err = deserializeJson(doc, body);
  if (err) { server.send(400, "text/plain", "json"); return; }
  const char* name = doc["name"] | "";
  float totalH = doc["totalHeightCm"] | 0.0f;
  float s2m = doc["sensorToMaxCm"] | 0.0f;
  const char* macs = doc["mac"] | "";

  int idx = -1;
  uint8_t macBuf[6] = {0};
  if (macs && strlen(macs) >= 17) {
    unsigned int b[6];
    if (sscanf(macs, "%02X:%02X:%02X:%02X:%02X:%02X",
               &b[0],&b[1],&b[2],&b[3],&b[4],&b[5])==6) {
      for (int k=0;k<6;k++) macBuf[k] = (uint8_t)b[k];
      idx = findDeviceByMAC(macBuf);
      if (idx == -1) {
        int free = findFreeSlot();
        if (free == -1) { server.send(500, "application/json", "{\"ok\":false,\"msg\":\"table full\"}"); return; }
        idx = free;
        devices[idx].used = true;
        devices[idx].macKnown = true;
        memcpy(devices[idx].mac, macBuf, 6);
      }
    }
  }

  if (idx == -1) {
    idx = findDeviceByName(name);
    if (idx == -1) {
      int free = findFreeSlot();
      if (free == -1) { server.send(500, "application/json", "{\"ok\":false,\"msg\":\"table full\"}"); return; }
      idx = free;
      devices[idx].used = true;
    }
  }

  if (macs && strlen(macs) >= 17) {
    devices[idx].macKnown = true;
    memcpy(devices[idx].mac, macBuf, 6);
  }
  strncpy(devices[idx].name, name, sizeof(devices[idx].name)-1);
  devices[idx].totalHeightCm = totalH;
  devices[idx].sensorToMaxCm = s2m;
  saveDevicesToFS();

  Serial.printf("Saved device idx=%d name=%s mac=%s\n", idx, devices[idx].name, devices[idx].macKnown?macToString(devices[idx].mac).c_str():"unknown");

  server.send(200, "application/json", "{\"ok\":true}");
}

// GET /api/config?name=...
void handleGetConfig() {
  if (!server.hasArg("name")) { server.send(400, "text/plain", "name required"); return; }
  String name = server.arg("name");
  int idx = findDeviceByName(name.c_str());
  if (idx == -1) { server.send(404, "application/json", "{\"ok\":false,\"msg\":\"unknown\"}"); return; }
  StaticJsonDocument<256> d;
  d["name"] = devices[idx].name;
  d["totalHeightCm"] = devices[idx].totalHeightCm;
  d["sensorToMaxCm"] = devices[idx].sensorToMaxCm;
  String out; serializeJson(d, out);
  server.send(200, "application/json", out);
}

void handleStatus() {
  StaticJsonDocument<256> s;
  s["ok"] = true;
  s["ap_ssid"] = AP_SSID;
  s["ap_ip"] = WiFi.softAPIP().toString();
  s["sta_connected"] = (WiFi.status() == WL_CONNECTED);
  s["sta_ip"] = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : String("");
  String out; serializeJson(s, out);
  server.send(200, "application/json", out);
}

/* simple web UI with modal editor (replace if you have your own UI) */
void handleRoot() {
  const char* html = R"rawliteral(
<!doctype html><html><head><meta charset="utf-8"><title>Sender Manager</title>
<meta name="viewport" content="width=device-width,initial-scale=1"><style>
body{font-family:system-ui;margin:12px}table{width:100%;border-collapse:collapse}th,td{border-bottom:1px solid #ccc;padding:6px}th{background:#eee}input{width:100%;box-sizing:border-box}
.modal-backdrop{position:fixed;inset:0;background:rgba(0,0,0,0.45);display:none;align-items:center;justify-content:center;padding:12px;z-index:10}
.modal{background:#fff;padding:16px;border-radius:8px;max-width:520px;width:100%}
</style></head><body>
<h2>Sender Device Manager</h2>
<div style="display:flex;gap:8px;align-items:center">
  <div>AP: <b>Sender-Direct</b> &nbsp; STA IP: <b id="staip">...</b></div>
  <div style="flex:1"></div>
  <button id="refreshBtn">Refresh</button>
  <button id="newBtn">New Device</button>
</div>
<table id="tbl"><thead><tr><th>MAC</th><th>IP</th><th>RSSI</th><th>Name</th><th>Percent</th><th>Age(s)</th><th>H (cm)</th><th>S2M (cm)</th><th>Actions</th></tr></thead><tbody></tbody></table>

<div id="modalBackdrop" class="modal-backdrop">
  <div class="modal">
    <h3 id="modalTitle">Edit Device</h3>
    <label>MAC</label><input id="m_mac" placeholder="AA:BB:...">
    <label>Name</label><input id="m_name" placeholder="Tank-1">
    <div style="display:flex;gap:8px"><div style="flex:1"><label>Total Height (cm)</label><input id="m_totalH" type="number" step="0.1"></div><div style="flex:1"><label>Sensor->Max (cm)</label><input id="m_s2m" type="number" step="0.1"></div></div>
    <div style="display:flex;justify-content:flex-end;gap:8px;margin-top:8px"><button id="mSave">Save</button><button id="mClose">Close</button></div>
  </div>
</div>

<script>
let devices = [];
let modalOpen = false;
let editIndex = -1;
async function fetchStatus(){ try{let s=await fetch('/status').then(r=>r.json()); document.getElementById('staip').innerText = s.sta_ip || 'none';}catch(e){document.getElementById('staip').innerText='err';}}
async function load(){ if(modalOpen){ try{devices = await (await fetch('/api/devices')).json(); renderTable(false);}catch(e){} return;} try{devices = await (await fetch('/api/devices')).json(); renderTable(true);}catch(e){console.error(e);} }
function renderTable(updateInputs){ const tb=document.querySelector('#tbl tbody'); tb.innerHTML=''; if(!devices || devices.length==0){tb.innerHTML='<tr><td colspan=9>No devices</td></tr>';return;} devices.forEach((x,i)=>{ const mac=x.mac||''; const ip=x.ip||''; const rssi=x.rssi||''; const name=x.name||''; const pct=(x.percent==null)?'--':(parseFloat(x.percent).toFixed(1)+'%'); const age=x.age_seconds||''; const h=x.totalHeightCm||''; const s2m=x.sensorToMaxCm||''; tb.innerHTML+=`<tr><td>${mac}</td><td>${ip}</td><td>${rssi}</td><td>${escapeHtml(name)}</td><td>${pct}</td><td>${age}</td><td>${h}</td><td>${s2m}</td><td><button onclick="openEdit(${i})">Edit</button></td></tr>`; }); }
function escapeHtml(s){ if(!s) return ''; return s.replaceAll('&','&amp;').replaceAll('<','&lt;').replaceAll('>','&gt;'); }
function openEdit(index){ modalOpen=true; editIndex=index; const macF=document.getElementById('m_mac'); const nameF=document.getElementById('m_name'); const hF=document.getElementById('m_totalH'); const sF=document.getElementById('m_s2m'); if(index>=0 && devices[index]){ const d=devices[index]; macF.value=d.mac||''; nameF.value=d.name||''; hF.value=d.totalHeightCm||''; sF.value=d.sensorToMaxCm||''; macF.disabled = !!d.mac; document.getElementById('modalTitle').innerText='Edit Device'; }else{ macF.disabled=false; macF.value=''; nameF.value=''; hF.value=''; sF.value=''; document.getElementById('modalTitle').innerText='New Device'; } document.getElementById('modalBackdrop').style.display='flex'; setTimeout(()=>nameF.focus(),150); }
function closeModal(){ modalOpen=false; editIndex=-1; document.getElementById('modalBackdrop').style.display='none'; }
async function saveModal(){ const mac=document.getElementById('m_mac').value.trim(); const name=document.getElementById('m_name').value.trim(); const totalH=parseFloat(document.getElementById('m_totalH').value)||0; const s2m=parseFloat(document.getElementById('m_s2m').value)||0; if(!name){alert('Name required');return;} const payload={name:name,totalHeightCm:totalH,sensorToMaxCm:s2m}; if(mac) payload.mac=mac; const res=await fetch('/api/device',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)}); if(!res.ok){alert('Save failed');return;} closeModal(); load(); }
document.getElementById('mClose').addEventListener('click', closeModal); document.getElementById('mSave').addEventListener('click', saveModal); document.getElementById('refreshBtn').addEventListener('click', load); document.getElementById('newBtn').addEventListener('click', ()=>openEdit(-1));
fetchStatus(); load(); setInterval(()=>{ fetchStatus(); load(); },2000); document.getElementById('modalBackdrop').addEventListener('click',(evt)=>{ if(evt.target.id==='modalBackdrop') closeModal(); });
</script>
</body></html>
  )rawliteral";
  server.send(200, "text/html", html);
}

/* setup & loop */
void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println("\nSender ESP32 HTTP starting...");

  if (!initFileSystem()) Serial.println("LittleFS init failed");
  loadDevicesFromFS();

  for (int i=0;i<MAX_DEVICES;i++) {
    // ensure clean defaults for any unused slots
    if (!devices[i].used) {
      devices[i].used = false;
      devices[i].macKnown = false;
      memset(devices[i].mac,0,6);
      devices[i].ip = IPAddress(0,0,0,0);
      devices[i].rssi = 0;
      devices[i].name[0] = 0;
      devices[i].percent = -1;
      devices[i].totalHeightCm = 0;
      devices[i].sensorToMaxCm = 0;
      devices[i].lastSeen = 0;
    }
  }

  WiFi.mode(WIFI_AP_STA);
  bool apok = WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL, false);
  if (!apok) Serial.println("softAP start failed");
  else {
    WiFi.softAPConfig(AP_IP, AP_IP, AP_NETMASK);
    Serial.printf("SoftAP started SSID='%s' IP=%s channel=%u\n", AP_SSID, WiFi.softAPIP().toString().c_str(), AP_CHANNEL);
  }

  if (trySTA) {
    if (useStaticIP) {
      if (!WiFi.config(STA_LOCAL_IP, STA_GATEWAY, STA_SUBNET, STA_DNS1, STA_DNS2)) {
        Serial.println("STA static IP config failed; will attempt DHCP");
      } else Serial.printf("Configured STA static IP: %s\n", STA_LOCAL_IP.toString().c_str());
    }
    WiFi.begin(STA_SSID, STA_PASS);
    Serial.printf("Attempting STA connect to '%s' ...\n", STA_SSID);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - t0) < 10000) { Serial.print('.'); delay(300); }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) Serial.printf("STA connected. IP=%s\n", WiFi.localIP().toString().c_str());
    else Serial.println("STA not connected (AP still up)");
  }

  if (MDNS.begin("sender")) Serial.println("mDNS responder started: http://sender.local/");
  else Serial.println("mDNS start failed (ok if unsupported)");

  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/api/devices", HTTP_GET, handleGetDevices);
  server.on("/api/report", HTTP_POST, handleReport);
  server.on("/api/device", HTTP_POST, handleSaveDevice);
  server.on("/api/config", HTTP_GET, handleGetConfig);
  server.begin();
  Serial.printf("HTTP server started (port %d)\n", HTTP_PORT);
  Serial.printf("AP URL: http://%s/\n", WiFi.softAPIP().toString().c_str());
  if (WiFi.status() == WL_CONNECTED) Serial.printf("STA URL: http://%s/\n", WiFi.localIP().toString().c_str());
}

void loop() {
  server.handleClient();
  // refresh AP-connected stations list occasionally to update lastSeen
  static unsigned long lastRefresh = 0;
  if (millis() - lastRefresh > 3000) {
    lastRefresh = millis();
    refreshConnectedStations();
  }
  delay(10);
}
