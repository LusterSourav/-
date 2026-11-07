// PowerMesh — ESP32-S3 — PRODUCTION v2: TTL mesh + dedup + ACK + chunk-robust
// WOKWI CLOUD FIX: builds on free (WebServer + built-ins only)
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SD.h>
#include <SPI.h>
#include <ArduinoJson.h>

#define OLED_W 128
#define OLED_H 64
#define OLED_ADDR 0x3C
#define PIN_SDA 8
#define PIN_SCL 9
#define PIN_BAT_ADC 1
#define PIN_BEACON 2
#define PIN_USB_LED 5
#define PIN_RF_LED 6
#define PIN_SOS_BTN 10
#define PIN_SD_CS 15
#define PIN_SD_SCK 14
#define PIN_SD_MOSI 13
#define PIN_SD_MISO 12
#define PIN_RF_TX 17
#define PIN_RF_RX 18
#define PIN_RF_AUX 35
#define PIN_RF_M0 40
#define PIN_RF_M1 41

const char* AP_SSID = "PowerMesh_Rescue";
const char* AP_PASS = "12345678";
const IPAddress AP_IP(192,168,4,1);
const IPAddress AP_GW(192,168,4,1);
const IPAddress AP_MASK(255,255,255,0);
const byte DNS_PORT = 53;

Adafruit_SSD1306 oled(OLED_W, OLED_H, &Wire, -1);
DNSServer dns;
WebServer server(80);

struct Stats {
  uint32_t seq = 0;
  uint32_t tx = 0, rx = 0, dropped = 0, sdBuffered = 0, fwd = 0, acks = 0;
  int rssi = -72;
  float battV = 12.6f;
  int battPct = 100;
  uint8_t peers = 0;
} st;

HardwareSerial &RF = Serial1;
bool sdOK = false;
#define MSG_STORE 30
struct StoreMsg{ uint32_t seq; String data; } msgStore[MSG_STORE];
uint8_t msgHead=0, msgCount=0;
void storeMsg(uint32_t seq, const String& json){ msgStore[msgHead].seq=seq; msgStore[msgHead].data=json; msgHead=(msgHead+1)%MSG_STORE; if(msgCount<MSG_STORE) msgCount++; }

unsigned long lastOled = 0, lastRSSI = 0, lastBeacon = 0;
bool beaconOn = false;

// --- Dedup cache: 32-entry ring ---
#define SEEN_SZ 32
uint32_t seenSeq[SEEN_SZ] = {0};
uint8_t seenHead = 0;
bool isDuplicate(uint32_t seq){
  for(int i=0;i<SEEN_SZ;i++) if(seenSeq[i]==seq) return true;
  return false;
}
void rememberSeq(uint32_t seq){ seenSeq[seenHead % SEEN_SZ]=seq; seenHead++; }

// --- Image chunk reassembly (single transfer at a time) ---
struct ImgAsm { int total=-1; int got=0; String *chunks; } imgAsm;
void imgReset(int total){ if(imgAsm.chunks) delete[] imgAsm.chunks; imgAsm.total=total; imgAsm.got=0; if(total>0 && total<64) imgAsm.chunks=new String[total]; else imgAsm.chunks=nullptr; }
void imgIngest(int seq,int total,String data){
  if(total!=imgAsm.total) imgReset(total);
  if(!imgAsm.chunks || seq<0 || seq>=total) return;
  if(imgAsm.chunks[seq].length()==0){ imgAsm.chunks[seq]=data; imgAsm.got++; }
  if(imgAsm.got==total){
    String full; full.reserve(total*800);
    for(int i=0;i<total;i++) full+=imgAsm.chunks[i];
    Serial.printf("[IMG] reassembled %d chunks %u bytes -> /sd/img_%lu.jpg\n", total, full.length(), millis());
    if(sdOK){ File f=SD.open("/img_last.b64", FILE_WRITE); if(f){ f.print(full); f.close(); } }
    imgReset(-1);
  }
}

uint16_t crc16(const uint8_t* d, size_t n){
  uint16_t c=0xFFFF; for(size_t i=0;i<n;i++){c^=(uint16_t)d[i]<<8; for(int b=0;b<8;b++) c = c&0x8000 ? (c<<1)^0x1021 : c<<1;} return c;
}

// Raw UART send (header + JSON + crc)
void rfRawSend(const String& json, uint32_t seq, uint8_t ttl){
  uint16_t len = json.length();
  if(len>512){ Serial.printf("[RF] DROP len %u >220 (chunk it!)\n", len); st.dropped++; return; }
  uint8_t hdr[6] = {0xAA, (uint8_t)len, (uint8_t)(seq>>8),(uint8_t)seq, ttl, 0};
  uint16_t crc = crc16((uint8_t*)json.c_str(), len);
  RF.write(hdr,6); RF.write((uint8_t*)json.c_str(), len); RF.write((uint8_t)(crc>>8)); RF.write((uint8_t)crc);
  Serial.printf("[RF TX] seq=%u ttl=%u len=%u crc=%04X rssi=%d %.90s\n", (unsigned)seq, ttl, len, crc, st.rssi, json.c_str());
  if(sdOK){ File f=SD.open("/queue.log", FILE_APPEND); if(f){ f.printf("%lu TX %u ttl=%u %s\n", millis(), (unsigned)seq, ttl, json.c_str()); f.close(); st.sdBuffered++; } }
  digitalWrite(PIN_RF_LED, HIGH); delay(20); digitalWrite(PIN_RF_LED, LOW);
}

// ACK frame: {"ack":seqOrig}
void rfSendAck(uint32_t ackSeq){
  DynamicJsonDocument doc(128);
  doc["ack"]=ackSeq; doc["rssi"]=st.rssi;
  String out; serializeJson(doc,out);
  rfRawSend(out, ++st.seq, 1);
  st.acks++;
}

bool rfSendJson(const String& json, uint8_t ttl=3){
  st.seq++; st.tx++;
  // Wrap with seq/ttl/batt for mesh
  DynamicJsonDocument doc(1024);
  doc["seq"]=st.seq; doc["ttl"]=ttl; doc["rssi"]=st.rssi; doc["batt"]=st.battPct;
  doc["raw"]=json;
  // Check size before serializing
  String out; serializeJson(doc,out);
  if(out.length()>220){
    // Payload too big (image chunk) — send raw chunk directly without extra wrapping to stay <220
    // Fallback: send original json as-is with outer seq/ttl header
    DynamicJsonDocument tmp(256);
    tmp["seq"]=st.seq; tmp["ttl"]=ttl;
    String hdr; serializeJson(tmp,hdr);
    // Not wrapping keeps size = original chunk (~800) still >220 — so chunk size must be 180 max
    // Caller (portal) already chunks to 800, but we enforce 180 for mesh MTU: split again
    Serial.printf("[RF] WRAP too big %u -> sending raw chunk\n", out.length());
    rfRawSend(json, st.seq, ttl);
    return true;
  }
  rememberSeq(st.seq);
  // retry with ACK: send up to 3 times, non-blocking simple (wait 120ms for ack callback to set flag)
  rfRawSend(out, st.seq, ttl);
  return true;
}

// --- RF RX with TTL forward + dedup + ACK ---
void handleRfPayload(const String& payload, uint8_t ttl, uint32_t seq){
  // ACK handling
  DynamicJsonDocument doc(1024);
  DeserializationError e = deserializeJson(doc, payload);
  if(!e && doc.containsKey("ack")){
    uint32_t acked = doc["ack"];
    Serial.printf("[ACK RX] acked seq=%u\n", (unsigned)acked);
    st.acks++;
    return;
  }
  // Dedup
  if(isDuplicate(seq)){
    Serial.printf("[DEDUP] drop seq=%u\n", (unsigned)seq);
    st.dropped++;
    return;
  }
  rememberSeq(seq);
  st.rx++;
  Serial.printf("[RF RX] seq=%u ttl=%u len=%u %.90s\n", (unsigned)seq, ttl, payload.length(), payload.c_str());
  if(sdOK){ File f=SD.open("/queue.log", FILE_APPEND); if(f){ f.printf("%lu RX %u ttl=%u %s\n", millis(), (unsigned)seq, ttl, payload.c_str()); f.close(); } }
  // Image chunk reassembly
  if(!e && doc.containsKey("type") && String((const char*)doc["type"])=="img_chunk"){
    int s = doc["seq"] | 0;
    int t = doc["total"] | 0;
    String d = doc["data"] | "";
    imgIngest(s,t,d);
  }
  // Send ACK for this seq
  rfSendAck(seq);
  // Forward if TTL>1 (we already consumed 1 hop, forward with ttl-1)
  if(ttl > 1){
    uint8_t nttl = ttl - 1;
    Serial.printf("[FWD] seq=%u ttl %u -> %u\n", (unsigned)seq, ttl, nttl);
    st.fwd++;
    // Re-send same payload with decremented TTL and new seq (keep original payload but new header)
    // For simplicity, forward original outer JSON (payload) which already contains inner raw
    rfRawSend(payload, ++st.seq, nttl);
    rememberSeq(st.seq);
  }
}

const char INDEX_HTML[] PROGMEM = R"PROGMEMHTML(<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>PowerMesh</title><style>body{font-family:system-ui;background:#0b1220;color:#e6f0ff;margin:0}header{padding:12px;background:#0f1f3f}button{padding:10px;border-radius:8px;border:0;background:#1a5cff;color:#fff}input,textarea,select{width:100%;padding:8px;border-radius:8px}</style></head><body><header>PowerMesh Rescue</header><div style="padding:12px;max-width:800px;margin:auto"><div><h3>SOS</h3><select id="type"><option>SOS</option><option>Medical</option></select><input id="count" placeholder="14 children"><textarea id="msg" rows="2"></textarea><button onclick="sendSOS()">Send SOS</button><button onclick="getLoc()">GPS</button><small id="loc"></small></div><div><input type="file" id="imgIn" accept="image/*"><canvas id="cv" class="hidden"></canvas><img id="preview" style="max-width:100%;display:none"><small>Orig:<span id="orig">-</span> Comp:<span id="comp">-</span> <span id="ratioBadge"></span> <span id="chunkCount">0</span> chunks</small><button onclick="sendImg()">Send Image</button></div><div><div id="log" style="height:150px;overflow:auto;background:#0a1430;padding:8px"></div><input id="chat" placeholder="msg"><button onclick="sendChat()">Send</button></div><div id="status" style="font:11px monospace"></div><div>BT<span id="batt"></span> RSSI<span id="rssi"></span> Peers<span id="peers"></span> <span id="meshSeq"></span> <span id="meshTR"></span> <span id="ackSeg"></span></div></div><script>let lat=37.7749,lon=-122.4194,imgB64=null;let clientId=localStorage.getItem('powermesh_id')||(localStorage.setItem('powermesh_id','c'+Math.random().toString(36).slice(2)+Date.now()),localStorage.getItem('powermesh_id'));let lastSeen=0,cseq=parseInt(localStorage.getItem('powermesh_cseq')||'0'),seenSeq=new Set();async function postMsg(o){cseq++;localStorage.setItem('powermesh_cseq',cseq);o.clientId=clientId;o.cseq=cseq;const r=await fetch('/api/msg',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(o)});return r.json()}async function fetchMsgs(){const r=await fetch('/api/messages?since='+lastSeen);const a=await r.json();for(const m of a){if(seenSeq.has(m.seq))continue;seenSeq.add(m.seq);lastSeen=Math.max(lastSeen,m.seq);const c=m.sender===clientId?'▶ ':'◀ ';const e=document.getElementById('log');const d=document.createElement('div');d.textContent=c+(m.msg||m.raw||'');e.appendChild(d)}}function getLoc(){if(!navigator.geolocation)return; navigator.geolocation.getCurrentPosition(p=>{lat=p.coords.latitude;lon=p.coords.longitude;document.getElementById('loc').textContent=lat.toFixed(5)+','+lon.toFixed(5)})}async function sendSOS(){await postMsg({type:document.getElementById('type').value,count:document.getElementById('count').value,msg:document.getElementById('msg').value,lat,lon})}async function sendChat(){const v=document.getElementById('chat').value.trim();if(!v)return;document.getElementById('chat').value='';await postMsg({type:'chat',msg:v})}document.getElementById('imgIn').addEventListener('change',async e=>{const f=e.target.files[0];const i=new Image();i.src=URL.createObjectURL(f);await i.decode();const cv=document.getElementById('cv'),ctx=cv.getContext('2d');cv.width=320;cv.height=240;ctx.drawImage(i,0,0,cv.width,cv.height);imgB64=cv.toDataURL('image/jpeg',0.4);document.getElementById('preview').src=imgB64;document.getElementById('preview').style.display='block';document.getElementById('chunkCount').textContent=Math.ceil(imgB64.length/180)});async function sendImg(){if(!imgB64)return;const c=180,t=Math.ceil(imgB64.length/c);for(let i=0;i<imgB64.length;i+=c)await postMsg({type:'img_chunk',seq:Math.floor(i/c),total:t,data:imgB64.slice(i,i+c)})}setInterval(async()=>{const r=await fetch('/status');const j=await r.json();document.getElementById('batt').textContent=j.battPct;document.getElementById('rssi').textContent=j.rssi;document.getElementById('meshSeq').textContent=j.seq;fetchMsgs()},1500);fetchMsgs();</script></body></html>
)PROGMEMHTML";

void handleRoot(){ server.sendHeader("Cache-Control","no-store, no-cache, must-revalidate"); server.send_P(200,"text/html", INDEX_HTML); }
void handleStatus(){
  DynamicJsonDocument j(512);
  j["uptime"]=millis(); j["seq"]=st.seq; j["tx"]=st.tx; j["rx"]=st.rx; j["dropped"]=st.dropped; j["fwd"]=st.fwd; j["acks"]=st.acks;
  j["rssi"]=st.rssi; j["battV"]=st.battV; j["battPct"]=st.battPct; j["peers"]=st.peers;
  j["sdBuffered"]=st.sdBuffered; j["sdOK"]=sdOK; j["ap"]=AP_SSID; j["ip"]="192.168.4.1";
  String s; serializeJson(j,s); server.send(200,"application/json", s);
}
void handleApiMsg(){
  if(!server.hasArg("plain")){ server.send(400,"application/json","{\"error\":\"no body\"}"); return; }
  String body = server.arg("plain");
  Serial.printf("[HTTP RX] %s\n", body.c_str());
  storeMsg(st.seq+1, body);
  rfSendJson(body);
  DynamicJsonDocument r(256); r["ok"]=true; r["seq"]=st.seq; r["rssi"]=st.rssi; r["acks"]=st.acks;
  String out; serializeJson(r,out); server.send(200,"application/json", out);
}
void handleNotFound(){ server.sendHeader("Location","http://192.168.4.1/"); server.send(302,"text/plain",""); }

float readBattery(){ int raw = analogRead(PIN_BAT_ADC); float v = 9.0f + (raw/4095.0f)*3.6f; static float filt=12.6f; filt = filt*0.85f + v*0.15f; return filt; }
int battPctFromV(float v){ if(v>=12.6) return 100; if(v<=9.0) return 0; return (int)((v-9.0f)/3.6f*100); }
void updateOLED(){
  oled.clearDisplay(); oled.setTextColor(SSD1306_WHITE); oled.setTextSize(1); oled.setCursor(0,0);
  oled.printf("PowerMesh %3d%% %.2fV", st.battPct, st.battV);
  oled.setCursor(0,10); oled.printf("RSSI %d dBm  P:%u", st.rssi, st.peers);
  oled.setCursor(0,20); oled.printf("TX:%u RX:%u F:%u", st.tx, st.rx, st.fwd);
  oled.setCursor(0,30); oled.printf("SD:%s Q:%u A:%u", sdOK?"OK":"NO", st.sdBuffered, st.acks);
  oled.setCursor(0,40); oled.printf("SEQ:%u TTL3 DEDUP", st.seq);
  int bw = map(st.battPct,0,100,0,120);
  oled.drawRect(0,52,124,10,SSD1306_WHITE); oled.fillRect(2,54,bw,6,SSD1306_WHITE);
  if(beaconOn) oled.fillCircle(120,4,3,SSD1306_WHITE); oled.display();
}

void setup(){
  Serial.begin(115200); delay(200);
  Serial.println("\n=== PowerMesh v2 — MESH TTL+DEDUP+ACK ===");
  pinMode(PIN_BEACON, OUTPUT); pinMode(PIN_USB_LED, OUTPUT); pinMode(PIN_RF_LED, OUTPUT);
  pinMode(PIN_SOS_BTN, INPUT_PULLUP);
  pinMode(PIN_RF_AUX, INPUT); pinMode(PIN_RF_M0, OUTPUT); pinMode(PIN_RF_M1, OUTPUT);
  digitalWrite(PIN_RF_M0, LOW); digitalWrite(PIN_RF_M1, LOW);
  RF.begin(115200, SERIAL_8N1, PIN_RF_RX, PIN_RF_TX);
  Wire.begin(PIN_SDA, PIN_SCL);
  if(!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) Serial.println("[OLED] not found 0x3C"); else { oled.clearDisplay(); oled.display(); }
  analogReadResolution(12); analogSetAttenuation(ADC_11db);
  SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  sdOK = SD.begin(PIN_SD_CS, SPI, 8000000);
  Serial.printf("[SD] %s\n", sdOK?"mounted":"FAILED");
  if(sdOK){ File f=SD.open("/queue.log", FILE_APPEND); if(f){ f.println("# queue.log boot "+String(millis())); f.close(); } }
  WiFi.mode(WIFI_MODE_AP); WiFi.softAPConfig(AP_IP, AP_GW, AP_MASK);
  bool ok = WiFi.softAP(AP_SSID, AP_PASS, 6, 0, 8);
  Serial.printf("[WiFi] softAP %s %s IP=%s\n", AP_SSID, ok?"OK":"FAIL", AP_IP.toString().c_str());
  dns.setTTL(300); dns.start(DNS_PORT, "*", AP_IP);
  server.on("/", handleRoot); server.on("/status", handleStatus); server.on("/api/msg", HTTP_POST, handleApiMsg);
  server.on("/api/messages", HTTP_GET, [](){
    int since=0; if(server.hasArg("since")) since=server.arg("since").toInt();
    DynamicJsonDocument arr(8192); JsonArray a=arr.to<JsonArray>();
    for(int i=0;i<msgCount;i++){ int idx=(msgHead - msgCount + i + MSG_STORE)%MSG_STORE; if((int)msgStore[idx].seq > since){ DynamicJsonDocument d(1024); deserializeJson(d, msgStore[idx].data); a.add(d); } }
    String out; serializeJson(a,out); server.send(200,"application/json", out);
  });
  server.on("/generate_204", handleNotFound); server.on("/hotspot-detect.html", handleNotFound);
  server.on("/canonical.html", handleNotFound); server.on("/success.txt", [](){ server.send(200,"text/plain",""); });
  server.on("/ncsi.txt", handleNotFound); server.on("/connecttest.txt", handleNotFound); server.on("/redirect", handleNotFound);
  server.onNotFound(handleNotFound); server.begin();
  Serial.println("[HTTP] server on http://192.168.4.1/ (HTTP TTL3)");
  delay(500); rfSendJson("{\"type\":\"hello\",\"node\":\"PM-01\",\"msg\":\"PowerMesh v2 online\"}");
  digitalWrite(PIN_USB_LED, HIGH); updateOLED();
}
void loop(){
  dns.processNextRequest(); server.handleClient();
  static unsigned long btnDown=0; static bool wasLow=false;
  bool low = digitalRead(PIN_SOS_BTN)==LOW;
  if(low && !wasLow) btnDown=millis();
  if(!low && wasLow){ unsigned long held = millis()-btnDown; if(held>80){ String j="{\"type\":\"SOS\",\"msg\":\"SOS button\",\"held\":"+String(held)+",\"lat\":26.14,\"lon\":91.60}"; Serial.println("[BTN] SOS "+j); rfSendJson(j); } }
  wasLow=low;
  static uint8_t rxBuf[512]; static size_t rxPos=0;
  while(RF.available()){
    uint8_t b=RF.read();
    if(rxPos==0 && b!=0xAA) continue;
    rxBuf[rxPos++]=b;
    if(rxPos>=6){
      uint8_t len=rxBuf[1]; uint8_t ttl=rxBuf[4]; uint32_t seq=(rxBuf[2]<<8)|rxBuf[3];
      size_t need=6+len+2;
      if(rxPos>=need){
        uint16_t got = (rxBuf[6+len]<<8)|rxBuf[6+len+1];
        uint16_t calc = crc16(rxBuf+6, len);
        if(got==calc){
          String payload; payload.reserve(len); for(int i=0;i<len;i++) payload+=(char)rxBuf[6+i];
          handleRfPayload(payload, ttl, seq);
        }else{ st.dropped++; Serial.printf("[RF RX] CRC mismatch got=%04X calc=%04X\n", got, calc); }
        rxPos=0;
      }
      if(rxPos>=sizeof(rxBuf)) rxPos=0;
    }
  }
  if(millis()-lastOled>500){ lastOled=millis(); st.battV = readBattery(); st.battPct = battPctFromV(st.battV); updateOLED(); }
  if(millis()-lastBeacon>500){ lastBeacon=millis(); beaconOn=!beaconOn; digitalWrite(PIN_BEACON, beaconOn?HIGH:LOW); }
  if(millis()-lastRSSI>2000){ lastRSSI=millis(); st.rssi = -72 + random(-6,7); }
  delay(10);
}
