/*
  Sender_ESP32_AP+STA_HTTP_MAC_Tracker_no_tcpip_adapter.ino
  - SoftAP + STA
  - Tracks connected client MACs via esp_wifi_ap_get_sta_list()
  - HTTP API for sensors to POST readings
  - LittleFS persistence of device configs
  - No tcpip_adapter.h required (compiles with modern Arduino-ESP32 cores)
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

const bool trySTA = true;                  // try to join router in STA mode
const bool useStaticIP = true;
const char* STA_SSID = "Airtel_7737476759"; // your router SSID
const char* STA_PASS = "air49169";         // your router password
IPAddress STA_LOCAL_IP(192,168,1,50);      // desired static IP on router
IPAddress STA_GATEWAY(192,168,1,1);
IPAddress STA_SUBNET(255,255,255,0);
IPAddress STA_DNS1(8,8,8,8);
IPAddress STA_DNS2(8,8,4,4);

const int HTTP_PORT = 80;
const char* DEVICES_FILE = "/devices.json";
const int MAX_DEVICES = 128;  // track by MAC
/* ---------------------------------------- */

WebServer server(HTTP_PORT);

struct Device {
  bool used;
  uint8_t mac[6];            // zeroed if unknown
  bool macKnown;
  IPAddress ip;              // best-effort (may be 0.0.0.0)
  int8_t rssi;
  char name[32];             // optional name reported by sensor
  float percent;
  float totalHeightCm;
  float sensorToMaxCm;
  unsigned long lastSeen;    // millis()
};

Device devices[MAX_DEVICES];

/* --------------- LittleFS ---------------- */
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
    char macs[18] = "";
    if (devices[i].macKnown) {
      sprintf(macs, "%02X:%02X:%02X:%02X:%02X:%02X",
              devices[i].mac[0],devices[i].mac[1],devices[i].mac[2],
              devices[i].mac[3],devices[i].mac[4],devices[i].mac[5]);
      o["mac"] = macs;
    } else {
      o["mac"] = nullptr;
    }
    o["name"] = devices[i].name;
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

  // clear table
  for (int i=0;i<MAX_DEVICES;i++) devices[i].used = false;

  if (doc.containsKey("devices")) {
    JsonArray arr = doc["devices"].as<JsonArray>();
    for (JsonObject o : arr) {
      const char* macs = o["mac"] | "";
      int idx = -1;
      for (int i=0;i<MAX_DEVICES;i++) if (!devices[i].used) { idx = i; break; }
      if (idx==-1) break;
      devices[idx].used = true;
      devices[idx].macKnown = false;
      memset(devices[idx].mac,0,6);
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
    }
  }
  Serial.println("Loaded devices from LittleFS");
  return true;
}

/* -------------- helpers ---------------- */
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

/* ---------- refresh AP station list (MAC + mark lastSeen) ---------- */
void refreshConnectedStations() {
  wifi_sta_list_t sta_list;
  memset(&sta_list, 0, sizeof(sta_list));
  esp_err_t r = esp_wifi_ap_get_sta_list(&sta_list);
  if (r != ESP_OK) {
    // nothing to do
    return;
  }

  unsigned long now = millis();
  for (int i = 0; i < sta_list.num; ++i) {
    wifi_sta_info_t s = sta_list.sta[i];
    int idx = findDeviceByMAC(s.mac);
    if (idx == -1) {
      idx = findFreeSlot();
      if (idx == -1) continue;
      devices[idx].used = true;
      devices[idx].macKnown = true;
      memcpy(devices[idx].mac, s.mac, 6);
      devices[idx].name[0] = 0;
      devices[idx].percent = -1;
      devices[idx].totalHeightCm = 0;
      devices[idx].sensorToMaxCm = 0;
      devices[idx].ip = IPAddress(0,0,0,0); // cannot reliably get IP without additional API
      devices[idx].rssi = 0;
    }
    devices[idx].lastSeen = now;
    // Note: wifi_sta_info_t doesn't provide RSSI on all cores; we set rssi to 0 here.
  }
}

/* ---------------- HTTP handlers ---------------- */

// POST /api/report  { name, percent, totalHeightCm, sensorToMaxCm }
// The sensor may also include its mac in JSON "mac":"AA:BB:.." to ensure correct linking.
void handleReport() {
  if (server.method() != HTTP_POST) { server.send(405); return; }
  String body = server.arg("plain");
  if (body.length() == 0) { server.send(400, "text/plain", "empty"); return; }
  StaticJsonDocument<512> doc;
  auto err = deserializeJson(doc, body);
  if (err) { server.send(400, "text/plain", err.c_str()); return; }

  const char* name = doc["name"] | "";
  float percent = doc["percent"] | -1.0f;
  float totalH = doc["totalHeightCm"] | 0.0f;
  float s2m = doc["sensorToMaxCm"] | 0.0f;
  const char* macs = doc["mac"] | ""; // optional: sensor can include its own MAC string

  int idx = -1;
  uint8_t macBuf[6] = {0};
  bool macProvided = false;
  if (macs && strlen(macs) >= 17) {
    unsigned int b[6];
    if (sscanf(macs, "%02X:%02X:%02X:%02X:%02X:%02X",
               &b[0],&b[1],&b[2],&b[3],&b[4],&b[5]) == 6) {
      for (int k=0;k<6;k++) macBuf[k] = (uint8_t)b[k];
      macProvided = true;
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

  // if no mac provided or not found, try matching by name
  if (idx == -1 && strlen(name)) {
    idx = findDeviceByName(name);
  }

  // if still not found, allocate new
  if (idx == -1) {
    idx = findFreeSlot();
    if (idx == -1) { server.send(500, "application/json", "{\"ok\":false,\"msg\":\"table-full\"}"); return; }
    devices[idx].used = true;
    devices[idx].macKnown = false;
    memset(devices[idx].mac,0,6);
  }

  if (macProvided) {
    devices[idx].macKnown = true;
    memcpy(devices[idx].mac, macBuf, 6);
  }

  if (strlen(name)) strncpy(devices[idx].name, name, sizeof(devices[idx].name)-1);
  devices[idx].percent = percent;
  devices[idx].totalHeightCm = totalH;
  devices[idx].sensorToMaxCm = s2m;
  devices[idx].lastSeen = millis();
  // ip/rssi left as-is (we cannot reliably get IP with current API on all cores)

  Serial.printf("Report saved: name=%s mac=%s pct=%.1f\n", devices[idx].name, (devices[idx].macKnown?macToString(devices[idx].mac).c_str():"unknown"), percent);

  server.send(200, "application/json", "{\"ok\":true}");
}

// ---------------- fixed handleGetDevices ----------------
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

    // ip/rssi/name/percent
    o["ip"] = devices[i].ip.toString();
    o["rssi"] = devices[i].rssi;
    o["name"] = devices[i].name[0] ? devices[i].name : nullptr;
    if (devices[i].percent >= 0) o["percent"] = devices[i].percent;
    else o["percent"] = nullptr;

    // age_seconds: either null or numeric
    if (devices[i].lastSeen == 0) {
      o["age_seconds"] = nullptr;
    } else {
      o["age_seconds"] = (now - devices[i].lastSeen) / 1000UL;
    }

    o["totalHeightCm"] = devices[i].totalHeightCm;
    o["sensorToMaxCm"] = devices[i].sensorToMaxCm;
  }
  String out; serializeJson(arr, out);
  server.send(200, "application/json", out);
}

// POST /api/device  { name, totalHeightCm, sensorToMaxCm, mac (optional) }
void handleSaveDevice() {
  if (server.method() != HTTP_POST) { server.send(405); return; }
  String body = server.arg("plain");
  if (body.length() == 0) { server.send(400, "text/plain", "empty"); return; }
  StaticJsonDocument<512> doc;
  auto err = deserializeJson(doc, body);
  if (err) { server.send(400, "text/plain", err.c_str()); return; }
  const char* name = doc["name"] | "";
  float totalH = doc["totalHeightCm"] | 0.0f;
  float s2m = doc["sensorToMaxCm"] | 0.0f;
  const char* macs = doc["mac"] | "";

  int idx = -1;
  if (macs && strlen(macs) >= 17) {
    unsigned int b[6];
    if (sscanf(macs, "%02X:%02X:%02X:%02X:%02X:%02X",
               &b[0],&b[1],&b[2],&b[3],&b[4],&b[5]) == 6) {
      uint8_t macb[6];
      for (int k=0;k<6;k++) macb[k] = (uint8_t)b[k];
      idx = findDeviceByMAC(macb);
      if (idx == -1) {
        int free = findFreeSlot();
        if (free == -1) { server.send(500, "application/json", "{\"ok\":false,\"msg\":\"table full\"}"); return; }
        idx = free;
        devices[idx].used = true;
        devices[idx].macKnown = true;
        memcpy(devices[idx].mac, macb, 6);
      }
    }
  }

  if (idx == -1) { // no mac given; find by name
    idx = findDeviceByName(name);
    if (idx == -1) {
      int free = findFreeSlot();
      if (free == -1) { server.send(500, "application/json", "{\"ok\":false,\"msg\":\"table full\"}"); return; }
      idx = free;
      devices[idx].used = true;
      devices[idx].macKnown = false;
      memset(devices[idx].mac,0,6);
    }
  }

  strncpy(devices[idx].name, name, sizeof(devices[idx].name)-1);
  devices[idx].totalHeightCm = totalH;
  devices[idx].sensorToMaxCm = s2m;
  saveDevicesToFS();
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

// ---------------- fixed handleStatus ----------------
void handleStatus() {
  StaticJsonDocument<256> s;
  s["ok"] = true;
  s["ap_ssid"] = AP_SSID;
  s["ap_ip"] = WiFi.softAPIP().toString();

  if (trySTA) {
    s["sta_connected"] = (WiFi.status() == WL_CONNECTED);
    if (WiFi.status() == WL_CONNECTED) {
      s["sta_ip"] = WiFi.localIP().toString();
    } else {
      s["sta_ip"] = nullptr;
    }
  } else {
    s["sta_connected"] = false;
    s["sta_ip"] = nullptr;
  }
  String out; serializeJson(s, out);
  server.send(200, "application/json", out);
}

// web UI root
// Replace your existing handleRoot() with this function:
void handleRoot() {
  const char* html = R"rawliteral(
<!doctype html><html><head><meta charset="utf-8"><title>Sender Hotspot Devices</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
  body{font-family:system-ui,Segoe UI,Roboto,Arial;margin:12px}
  h2{margin:6px 0}
  table{width:100%;border-collapse:collapse;margin-top:8px}
  th,td{border-bottom:1px solid #ccc;padding:6px;text-align:left}
  th{background:#f3f3f3}
  input[type=text], input[type=number]{width:100%;box-sizing:border-box;padding:6px}
  button{padding:6px 8px;margin:2px;border-radius:6px;border:1px solid #ccc;background:#fff;cursor:pointer}
  .muted{color:#666;font-size:13px}
  .topbar{display:flex;gap:8px;align-items:center;margin-bottom:8px}
  .flexrow{display:flex;gap:8px;align-items:center}
  /* modal styling */
  .modal-backdrop{position:fixed;inset:0;background:rgba(0,0,0,0.45);display:none;align-items:center;justify-content:center;padding:12px;z-index:10}
  .modal{background:#fff;padding:16px;border-radius:8px;max-width:520px;width:100%;box-shadow:0 8px 24px rgba(0,0,0,0.2)}
  .modal h3{margin-top:0}
  .row{display:flex;gap:8px}
  .col{flex:1}
  .small{font-size:12px;color:#666}
</style>
</head><body>
<h2>Sender Hotspot Device Manager</h2>
<div class="topbar">
  <div class="muted">AP SSID: <b>Sender-Direct</b> &nbsp; (password <b>senderpass</b>)</div>
  <div style="flex:1"></div>
  <div class="muted">STA: <span id="staip">...</span></div>
  <button id="refreshBtn">Refresh</button>
  <button id="newBtn">New Device</button>
</div>

<table id="devTable"><thead><tr>
  <th>MAC</th><th>IP</th><th>RSSI</th><th>Name</th><th>Percent</th><th>Age(s)</th><th>H (cm)</th><th>S2M (cm)</th><th>Actions</th>
</tr></thead><tbody></tbody></table>

<!-- Modal (Edit/Create) -->
<div id="modalBackdrop" class="modal-backdrop">
  <div class="modal" role="dialog" aria-modal="true" aria-labelledby="modalTitle">
    <h3 id="modalTitle">Edit Device</h3>
    <div class="small">Edit device details here. Press Save to persist to the Sender's flash.</div>
    <div style="height:12px"></div>
    <div>
      <label>MAC (read-only when known)</label>
      <input type="text" id="m_mac" placeholder="AA:BB:CC:DD:EE:FF">
    </div>
    <div style="height:8px"></div>
    <div>
      <label>Name</label>
      <input type="text" id="m_name" placeholder="Tank-1">
    </div>
    <div style="height:8px"></div>
    <div class="row">
      <div class="col">
        <label>Total Height (cm)</label>
        <input type="number" step="0.1" id="m_totalH">
      </div>
      <div class="col">
        <label>Sensor→Max (cm)</label>
        <input type="number" step="0.1" id="m_s2m">
      </div>
    </div>
    <div style="height:12px"></div>
    <div class="flexrow" style="justify-content:flex-end">
      <button id="mSave">Save</button>
      <button id="mClose">Close</button>
    </div>
  </div>
</div>

<script>
let devices = [];
let refreshing = true;
let refreshTimer = null;
let modalOpen = false;
let editIndex = -1; // index of edit in 'devices' array, or -1 for new

async function fetchStatus() {
  try {
    const st = await fetch('/status').then(r => r.json());
    document.getElementById('staip').innerText = st.sta_ip || 'none';
  } catch (e) {
    document.getElementById('staip').innerText = 'err';
  }
}

async function loadDevices() {
  if (modalOpen) {
    // prefer not to change modal contents while editing, but still refresh the background data
    try {
      const res = await fetch('/api/devices');
      devices = await res.json();
      renderTable(false); // update table only
    } catch (e) {
      console.error('loadDevices error', e);
    }
    return;
  }
  try {
    const res = await fetch('/api/devices');
    devices = await res.json();
    renderTable(true); // update table and keep focus
  } catch (e) {
    console.error('loadDevices error', e);
  }
}

function renderTable(updateInputs) {
  const tbody = document.querySelector('#devTable tbody');
  tbody.innerHTML = '';
  if (!devices || devices.length === 0) {
    tbody.innerHTML = '<tr><td colspan="9">No devices</td></tr>';
    return;
  }
  devices.forEach((d, idx) => {
    const mac = d.mac || '';
    const ip = d.ip || '';
    const rssi = (d.rssi !== undefined) ? d.rssi : '';
    const name = d.name || '';
    const pct = (d.percent == null) ? '--' : (parseFloat(d.percent).toFixed(1) + '%');
    const age = d.age_seconds || '';
    const h = (d.totalHeightCm!=null)?d.totalHeightCm:'';
    const s2m = (d.sensorToMaxCm!=null)?d.sensorToMaxCm:'';
    const tr = document.createElement('tr');
    tr.innerHTML = `<td>${mac}</td><td>${ip}</td><td>${rssi}</td>
      <td>${escapeHtml(name)}</td><td>${pct}</td><td>${age}</td>
      <td>${h}</td><td>${s2m}</td>
      <td>
        <button onclick="openEdit(${idx})">Edit</button>
      </td>`;
    tbody.appendChild(tr);
  });
}

// simple escape to avoid injection (UI only)
function escapeHtml(s) {
  if (!s) return '';
  return s.replaceAll('&','&amp;').replaceAll('<','&lt;').replaceAll('>','&gt;');
}

function openEdit(index) {
  modalOpen = true;
  editIndex = (typeof index === 'number') ? index : -1;
  const mb = document.getElementById('modalBackdrop');
  const macField = document.getElementById('m_mac');
  const nameField = document.getElementById('m_name');
  const hField = document.getElementById('m_totalH');
  const sField = document.getElementById('m_s2m');

  if (editIndex >= 0 && devices[editIndex]) {
    const d = devices[editIndex];
    macField.value = d.mac || '';
    nameField.value = d.name || '';
    hField.value = (d.totalHeightCm!=null)?d.totalHeightCm:'';
    sField.value = (d.sensorToMaxCm!=null)?d.sensorToMaxCm:'';
    if (d.mac) {
      macField.disabled = true; // don't allow editing known MAC
    } else {
      macField.disabled = false;
    }
    document.getElementById('modalTitle').innerText = 'Edit Device';
  } else {
    // new device
    macField.disabled = false;
    macField.value = '';
    nameField.value = '';
    hField.value = '';
    sField.value = '';
    document.getElementById('modalTitle').innerText = 'New Device';
  }

  mb.style.display = 'flex';
  // focus name input
  setTimeout(()=>nameField.focus(), 150);
}

function closeModal() {
  modalOpen = false;
  editIndex = -1;
  document.getElementById('modalBackdrop').style.display = 'none';
}

async function saveModal() {
  const mac = document.getElementById('m_mac').value.trim();
  const name = document.getElementById('m_name').value.trim();
  const totalH = parseFloat(document.getElementById('m_totalH').value) || 0;
  const s2m = parseFloat(document.getElementById('m_s2m').value) || 0;
  if (!name) { alert('Name is required'); return; }

  const payload = { name: name, totalHeightCm: totalH, sensorToMaxCm: s2m };
  if (mac) payload.mac = mac;

  try {
    const res = await fetch('/api/device', {
      method: 'POST',
      headers: {'Content-Type':'application/json'},
      body: JSON.stringify(payload)
    });
    if (!res.ok) {
      const txt = await res.text();
      alert('Save failed: ' + txt);
      return;
    }
    // success: close modal and refresh
    closeModal();
    await loadDevices();
  } catch (e) {
    alert('Save error: ' + e);
  }
}

// UI controls
document.getElementById('mClose').addEventListener('click', closeModal);
document.getElementById('mSave').addEventListener('click', saveModal);
document.getElementById('refreshBtn').addEventListener('click', loadDevices);
document.getElementById('newBtn').addEventListener('click', ()=>openEdit(-1));

// auto-refresh logic: refresh every 2s unless page hidden
function startAutoRefresh() {
  if (refreshTimer) clearInterval(refreshTimer);
  refreshTimer = setInterval(() => {
    if (!modalOpen) loadDevices();
    else loadDevices(); // still update table in background while modal is open
  }, 2000);
}

// initial load
fetchStatus();
loadDevices();
startAutoRefresh();

// close modal when clicking backdrop outside modal content
document.getElementById('modalBackdrop').addEventListener('click', (evt)=>{
  if (evt.target.id === 'modalBackdrop') closeModal();
});
</script>
</body></html>
  )rawliteral";
  server.send(200, "text/html", html);
}

/* ---------------- setup & loop ---------------- */
void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println("\nSender ESP32 AP+STA HTTP starting...");

  if (!initFileSystem()) Serial.println("LittleFS init failed");

  // init device table
  for (int i=0;i<MAX_DEVICES;i++) {
    devices[i].used = false;
    devices[i].ip = IPAddress(0,0,0,0);
    devices[i].rssi = 0;
    devices[i].name[0] = 0;
    devices[i].percent = -1;
    devices[i].totalHeightCm = 0;
    devices[i].sensorToMaxCm = 0;
    devices[i].lastSeen = 0;
    devices[i].macKnown = false;
    memset(devices[i].mac, 0, 6);
  }

  loadDevicesFromFS();

  // Start SoftAP first (fix channel for sensors)
  WiFi.mode(WIFI_AP_STA); // allow AP + STA simultaneously
  bool apok = WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL, false);
  if (!apok) Serial.println("softAP start failed");
  else {
    WiFi.softAPConfig(AP_IP, AP_IP, AP_NETMASK); // set softAP IP/GW
    Serial.printf("SoftAP started SSID='%s' IP=%s channel=%u\n", AP_SSID, WiFi.softAPIP().toString().c_str(), AP_CHANNEL);
  }

  // Try to join router (STA) with static IP if configured
  if (trySTA) {
    if (useStaticIP) {
      if (!WiFi.config(STA_LOCAL_IP, STA_GATEWAY, STA_SUBNET, STA_DNS1, STA_DNS2)) {
        Serial.println("STA static IP config failed; will use DHCP");
      } else {
        Serial.print("Configured STA static IP: ");
        Serial.println(STA_LOCAL_IP.toString());
      }
    }
    WiFi.begin(STA_SSID, STA_PASS);
    Serial.printf("Attempting STA connect to '%s' ...\n", STA_SSID);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - t0) < 10000) {
      Serial.print('.');
      delay(300);
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("STA connected. IP=%s  channel=%d\n", WiFi.localIP().toString().c_str(), WiFi.channel());
    } else {
      Serial.println("STA not connected (will keep AP up).");
    }
  }

  // start mDNS hostname 'sender' for LAN (works only when STA connected)
  if (MDNS.begin("sender")) Serial.println("mDNS responder started: http://sender.local/");
  else Serial.println("mDNS start failed (ok if unsupported)");

  // HTTP server routes
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
  delay(10);
}
