
// =====================================================================
// purplesword.ino — Multi-Mode Purple Sword Controller
// =====================================================================
//
// Modes (single-click cycles):
//   0 = Fire        — Melting lava + swing fireball
//   1 = Rainbow     — Paint splashes that mush & mix hilt→tip
//   2 = Lightning   — Blue storm rush + lightning strike on stop
//
// PHYSICAL LED LAYOUT — Single Folded Strip (86 total, blade only):
//
//   One strip folded at the tip creates two blade faces.
//   Side A: leds[0]  (hilt) → leds[42] (tip)   — strip runs hilt→tip
//   Side B: leds[43] (tip)  → leds[85] (hilt)  — strip runs tip→hilt
//
//   bladeSet(pos, color) maps virtual pos (0–179) to physical (0–42)
//   and mirrors to both sides simultaneously.
//
// Button (D3): Click = next mode | Hold > 0.5s = Boost | Triple+Hold = Sync
// OTA hostname: PurpleSword  |  OTA password: sword
// =====================================================================

#include "Config.h"

// ─── GLOBAL VARIABLE DEFINITIONS ──────────────────────────────────────
const char* ssid        = "CGN3-4400";
const char* password    = "251148015432";
const char* hostname    = "PurpleSword";
const char* otaHash   = "f3b462d93b24cb0538f5d864546bc3e0"; // MD5 hash of "sword"
WebServer   server(80);

bool     otaActive   = false;
uint16_t otaProgress = 0;

CRGB leds[NUM_LEDS];

bool   imuOk     = false;
float  swingMag  = 0.0f;
float  currSwing = 0.0f;
float  twistRate = 0.0f;
float  accelX    = 0.0f;
float  accelY    = 0.0f;
float  accelZ    = 0.0f;
float  gx = 0.0f, gy = 0.0f, gz = 0.0f;
int8_t tiltDir   =  1;

uint8_t  effectMode   = 0;
uint8_t  baseHue      = 190;

uint8_t  clickCount   = 0;
uint32_t lastClickMs  = 0;
bool     lastButton   = HIGH;

bool     boostMode    = false;
uint32_t buttonDownMs = 0;

float rollPhase = 0.0f;
float lastGyroZ = 0.0f;

bool fireActive = false;

bool      syncEnabled    = false;
bool      syncSearching  = false;
uint32_t  syncSearchStart = 0;
uint32_t  lastPingMs     = 0;
bool      syncAnimating  = false;
uint32_t  syncAnimStart  = 0;
volatile bool    syncGotPacket = false;
volatile uint8_t syncInType    = 0;
volatile uint8_t syncInMode    = 0;

struct SyncPacket { uint8_t type; uint8_t mode; };
uint8_t broadcastAddr[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

typedef struct { float bass, mid, treble, totalVolume; int currentMode; } MicPacket;
float    fswBass  = 0.0f, fswMid = 0.0f, fswTreb = 0.0f, fswVol = 0.0f;
bool     fswBeat  = false;
float    fswBeatI = 0.0f;
uint32_t fswLastPkt = 0;

uint8_t currentMode = MODE_FIRE;
bool    autoCycleMode = false;
uint32_t lastAutoCycleMs = 0;

// ─── IMPACT WAVES ─────────────────────────────────────────────────────
#define MAX_IMPACTS 4
struct ImpactWave { float pos, vel, intensity; bool active; };
ImpactWave impacts[MAX_IMPACTS];
int impactNext = 0;

float stabImpulse = 0.0f;
float pullImpulse = 0.0f;
float prevAZ      = 0.0f;

void spawnImpact(float startPos, float vel, float intensity) {
  impacts[impactNext] = { startPos, vel, intensity, true };
  impactNext = (impactNext + 1) % MAX_IMPACTS;
}

void updateAndRenderImpacts() {
  for (int i = 0; i < MAX_IMPACTS; i++) {
    if (!impacts[i].active) continue;
    impacts[i].pos += impacts[i].vel;
    impacts[i].intensity *= 0.88f;
    if (impacts[i].intensity < 0.05f || impacts[i].pos < -20.0f || impacts[i].pos > BLADE_LENGTH + 20.0f) {
      impacts[i].active = false; continue;
    }
    int p = (int)impacts[i].pos;
    float frac = impacts[i].pos - p;
    float inten = constrain(impacts[i].intensity, 0.0f, 1.0f);
    for (int d = -4; d <= 4; d++) {
      int pos = p + d;
      if (pos >= 0 && pos < BLADE_LENGTH) {
        float dist = fabsf((float)d + frac);
        float falloff = expf(-0.3f * dist * dist);
        uint8_t bri = (uint8_t)(inten * falloff * 255.0f);
        if (bri > 0) {
          uint8_t sat = (uint8_t)(255.0f * (1.0f - falloff));
          bladeSet(pos, bladeGet(pos) + CHSV(130, sat, bri));
        }
      }
    }
  }
}


// =====================================================================
// IMU (MPU9250 raw I2C)
// =====================================================================

static void imuWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(IMU_ADDR); Wire.write(reg); Wire.write(val); Wire.endTransmission();
}

static int16_t imuRead16(uint8_t reg) {
  Wire.beginTransmission(IMU_ADDR); Wire.write(reg); Wire.endTransmission(false);
  Wire.requestFrom(IMU_ADDR, (uint8_t)2);
  return (int16_t)((Wire.read() << 8) | Wire.read());
}

void setupIMU() {
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);
  delay(150);

  Wire.beginTransmission(0x68);
  uint8_t err = Wire.endTransmission();
  if (err != 0) {
    Serial.printf("MPU9250: no ACK at 0x68 (I2C error %u)\n", err);
    Wire.beginTransmission(0x69);
    if (Wire.endTransmission() == 0) Serial.println("  → found at 0x69 — is AD0 pin HIGH?");
    imuOk = false; return;
  }

  Wire.beginTransmission(0x68); Wire.write(0x75); err = Wire.endTransmission(false);
  if (err != 0) { Serial.printf("MPU9250: WHO_AM_I failed (%u)\n", err); imuOk = false; return; }
  Wire.requestFrom((uint8_t)0x68, (uint8_t)1);
  uint8_t id = Wire.available() ? Wire.read() : 0xFF;
  if (id != 0x71 && id != 0x73 && id != 0x70 && id != 0x68) {
    Serial.printf("MPU9250: WHO_AM_I = 0x%02X, expected 0x71/0x73\n", id); imuOk = false; return;
  }

  Wire.beginTransmission(0x68); Wire.write(0x6B); Wire.write(0x00); Wire.endTransmission();
  delay(50);
  Wire.beginTransmission(0x68); Wire.write(0x1B); Wire.write(0x08); Wire.endTransmission();
  Wire.beginTransmission(0x68); Wire.write(0x1C); Wire.write(0x00); Wire.endTransmission();

  imuOk = true;
  Serial.println("MPU9250: found and configured at 0x68");
}

void updateIMU() {
  if (!imuOk) return;
  accelX = imuRead16(0x3B) / 16384.0f;
  accelY = imuRead16(0x3D) / 16384.0f;
  accelZ = imuRead16(0x3F) / 16384.0f;

  gx = imuRead16(0x43) / 65.5f;
  gy = imuRead16(0x45) / 65.5f;
  gz = imuRead16(0x47) / 65.5f;

  currSwing = sqrtf(gx*gx + gy*gy);
  swingMag  = swingMag  * 0.55f + currSwing * 0.45f;
  twistRate = twistRate * 0.70f + gz    * 0.30f;

  if      (accelZ >  0.2f) tiltDir =  1;
  else if (accelZ < -0.2f) tiltDir = -1;

  float dAZ = accelZ - prevAZ;
  stabImpulse = fmaxf(0.0f, dAZ - 0.30f);
  pullImpulse = fmaxf(0.0f, -dAZ - 0.30f);
  prevAZ = accelZ;

  if (stabImpulse > 0.2f) spawnImpact(0.0f,           3.5f + stabImpulse * 1.5f, fminf(1.8f, 0.6f + stabImpulse * 2.5f));
  if (pullImpulse > 0.2f) spawnImpact((float)(BLADE_LENGTH-1), -(3.5f + pullImpulse * 1.5f), fminf(1.8f, 0.6f + pullImpulse * 2.5f));
}


// =====================================================================
// OVERLAYS
// =====================================================================



void updateAndRenderRollOverlay() {
  float gyroZ_abs = fabsf(twistRate);
  if (gyroZ_abs > 30.0f) {
    rollPhase += (twistRate * 0.005f);
    int numSparks = constrain((int)(gyroZ_abs / 20.0f), 1, 8);
    for (int i = 0; i < numSparks; i++) {
      int target = random(0, BLADE_LENGTH);
      float shift = sinf(rollPhase + (target * 0.1f)) + 1.0f;
      CRGB sparkColor = CHSV(baseHue + (twistRate > 0 ? 30 : -30), 120, (uint8_t)(shift * 127.0f));
      bladeSet(target, bladeGet(target) + sparkColor);
    }
  }
}

void renderBoostSparks() {
  int count = constrain(3 + (int)(swingMag * 0.06f), 3, 10);
  for (int s = 0; s < count; s++) {
    int pos = random(BLADE_LENGTH);
    bladeSet(pos, bladeGet(pos) + CHSV(baseHue + (int8_t)(random8()/4 - 32), random8(60, 200), random8(160, 255)));
  }
}


// =====================================================================
// ESP-NOW SYNC
// =====================================================================

void onSyncReceive(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len == (int)sizeof(SyncPacket)) {
    SyncPacket pkt; memcpy(&pkt, data, sizeof(SyncPacket));
    syncInType = pkt.type; syncInMode = pkt.mode; syncGotPacket = true;
  } else if (len >= (int)sizeof(MicPacket)) {
    MicPacket msg; memcpy(&msg, data, sizeof(MicPacket));
    static float enMax = 1.0f;
    if (msg.totalVolume > enMax) enMax = msg.totalVolume;
    enMax = enMax * 0.9997f + 0.0003f;
    if (enMax < 0.01f) enMax = 0.01f;
    fswVol  = constrain(msg.totalVolume / enMax,        0.0f, 1.0f);
    fswBass = constrain(msg.bass        / enMax * 2.5f, 0.0f, 1.0f);
    fswMid  = constrain(msg.mid         / enMax * 2.5f, 0.0f, 1.0f);
    fswTreb = constrain(msg.treble      / enMax * 2.0f, 0.0f, 1.0f);
    static float lastBass = 0.0f;
    float delta = fswBass - lastBass;
    lastBass = fswBass * 0.65f;
    if (delta > 0.18f && fswBass > 0.30f) { fswBeat = true; fswBeatI = constrain(delta * 3.5f, 0.3f, 1.0f); }
    fswLastPkt = millis();
  }
}

void sendSyncPacket(uint8_t type, uint8_t mode) {
  SyncPacket pkt = { type, mode };
  esp_now_send(broadcastAddr, (uint8_t*)&pkt, sizeof(SyncPacket));
}

void setupESPNow() {
  if (esp_now_init() != ESP_OK) { Serial.println("ESP-NOW init failed"); return; }
  esp_now_register_recv_cb(onSyncReceive);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, broadcastAddr, 6);
  peer.channel = 0; peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) Serial.println("ESP-NOW peer failed");
  else Serial.println("ESP-NOW ready");
}

void renderSyncAnim(float progress) {
  bladeClear();
  int lit = constrain((int)(progress * BLADE_LENGTH), 0, BLADE_LENGTH - 1);
  for (int i = 0; i <= lit; i++) bladeSet(i, CRGB(0, 50, 80));
  bladeSet(lit, CRGB(255, 255, 255));
}

void renderSyncSearching() {
  static float offset = 0.0f;
  offset += 0.3f; if (offset >= 20.0f) offset -= 20.0f;
  float   pulse = 0.55f + 0.45f * sinf((float)millis() * 0.003f);
  uint8_t val   = (uint8_t)(200.0f * pulse);
  int     off   = (int)offset;
  for (int i = 0; i < BLADE_LENGTH; i++)
    bladeSet(i, CHSV((((i + off) / 10) % 2) ? 192 : 0, 255, val));
}




// =====================================================================
// OTA VISUAL
// =====================================================================
void renderOTAMode() {
  FastLED.setBrightness(OTA_BRIGHTNESS);
  static bool     blinkState = false;
  static uint32_t lastBlink  = 0;
  if (millis() - lastBlink > 300) { blinkState = !blinkState; lastBlink = millis(); }
  bladeClear();
  // Blink every ~5 virtual positions so both sides see the pattern
  if (blinkState) {
    for (int i = 0; i < BLADE_LENGTH; i += 5) bladeSet(i, CRGB(0, 0, 150));
  }
  // OTA progress fill — mirrors onto both sides via bladeSet
  int filled = (otaProgress * BLADE_LENGTH) / 100;
  for (int i = 0; i < filled; i++) bladeSet(i, CRGB(0, 100, 255));
  FastLED.show();
}


// =====================================================================
// WIFI + OTA + WEB SERVER
// =====================================================================

void handleRoot() {
  const char* modeNames[] = { "Fire", "Rainbow Fireball", "Lightning Storm" };
  uint8_t modeIdx = currentMode < MODE_COUNT ? currentMode : 0;
  String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{background:#111;color:#eee;font-family:sans-serif;text-align:center;}";
  html += "h1{color:#fa6;} p{color:#aaa;}";
  html += "button{background:#333;color:#fff;border:1px solid #555;padding:10px 20px;margin:5px;border-radius:5px;cursor:pointer;font-size:16px;}";
  html += "button:hover{background:#444;}";
  html += ".active{background:#fa6;color:#000;border-color:#fa6;}";
  html += "</style></head><body>";
  html += "<h1>Fullsword</h1>";
  html += "<p>Mode: <b>" + String(modeNames[modeIdx]) + "</b></p>";
  
  // Mode Buttons
  html += "<div style='margin-top:20px;margin-bottom:20px;'>";
  html += "<button onclick='fetch(\"/setMode?m=0\").then(()=>location.reload())'" + String(currentMode==0 ? " class='active'" : "") + ">Fire</button>";
  html += "<button onclick='fetch(\"/setMode?m=1\").then(()=>location.reload())'" + String(currentMode==1 ? " class='active'" : "") + ">Rainbow</button>";
  html += "<button onclick='fetch(\"/setMode?m=2\").then(()=>location.reload())'" + String(currentMode==2 ? " class='active'" : "") + ">Lightning</button>";
  html += "</div>";
  
  // Auto-cycle Toggle
  html += "<div style='margin-bottom:20px;'>";
  if (autoCycleMode) {
    html += "<button onclick='fetch(\"/setMode?auto=0\").then(()=>location.reload())' style='background:#2a5;border-color:#2a5;'>Auto-Cycle: ON</button>";
  } else {
    html += "<button onclick='fetch(\"/setMode?auto=1\").then(()=>location.reload())'>Auto-Cycle: OFF</button>";
  }
  html += "</div>";

  html += "<p>aX:" + String(accelX,2) + "  aY:" + String(accelY,2) + "  aZ:" + String(accelZ,2) + "</p>";
  html += "<p>swing:" + String(swingMag,1) + "  twist:" + String(twistRate,1) + "</p>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleSetMode() {
  if (server.hasArg("m")) {
    int m = server.arg("m").toInt();
    if (m >= 0 && m < MODE_COUNT) {
      currentMode = m;
      effectMode = currentMode;
      lastAutoCycleMs = millis(); // reset timer when manually changed
      Serial.printf("Web Mode → %d\n", currentMode);
      
      // Mode switch flash
      CRGB flashColor;
      switch (currentMode) {
        case MODE_FIRE:      flashColor = CRGB(255, 80, 0);  break;
        case MODE_RAINBOW:   flashColor = CRGB(100, 0, 255); break;
        case MODE_LIGHTNING: flashColor = CRGB(0, 100, 255); break;
      }
      for (int i = 0; i < BLADE_LENGTH; i++) bladeSet(i, flashColor);
      FastLED.show(); delay(80);
      bladeClear(); FastLED.show();
    }
  }
  
  if (server.hasArg("auto")) {
    autoCycleMode = (server.arg("auto").toInt() == 1);
    lastAutoCycleMs = millis();
    Serial.printf("Auto-Cycle → %s\n", autoCycleMode ? "ON" : "OFF");
  }
  
  server.send(200, "text/plain", "OK");
}

void setupWiFiOTA() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(hostname);
  WiFi.begin(ssid, password);
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis()-t < 10000) delay(100);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi: " + WiFi.localIP().toString());
    if (MDNS.begin(hostname)) Serial.println("mDNS ready");
    server.on("/", handleRoot);
    server.on("/setMode", handleSetMode);
    server.begin();
  }
  ArduinoOTA.setHostname(hostname);
  ArduinoOTA.setPasswordHash(otaHash);
  ArduinoOTA.onStart([]()     { otaActive = true; otaProgress = 0; Serial.println("OTA Start"); });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int tot) { otaProgress = (p*100)/tot; });
  ArduinoOTA.onEnd([]()       { Serial.println("OTA End"); });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("OTA Error[%u]\n", e); });
  ArduinoOTA.begin();
  setupESPNow();
}


// =====================================================================
// SETUP
// =====================================================================
void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(BRIGHTNESS);
  FastLED.clear(true);

  setupIMU();

  for (int i = 0; i < BLADE_LENGTH; i++) {
    bladeSet(i, CRGB(200, 80, 0));
    FastLED.show();
    delay(4);
  }
  delay(200);
  bladeClear();
  FastLED.show();

  setupWiFiOTA();
  Serial.println("Ready.  Click=mode  Hold=boost  TripleHold=sync");
}


// =====================================================================
// MAIN LOOP
// =====================================================================
void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  if (otaActive) { renderOTAMode(); return; }

  // ── WiFi reconnect watchdog ─────────────────────────────────────────
  static uint32_t wifiCheckMs = 0;
  if (millis() - wifiCheckMs > 30000) {
    wifiCheckMs = millis();
    if (WiFi.status() != WL_CONNECTED) { Serial.println("WiFi lost — reconnecting..."); WiFi.reconnect(); }
  }

  updateIMU();
  updatePOV();   // integrate gyro → povAngle for the POV overlay

  // ── ESP-NOW sync packet handling ───────────────────────────────────
  if (syncGotPacket) {
    syncGotPacket = false;
    if (syncInType == SYNC_MSG_PING) {
      if (syncSearching) {
        syncSearching = false; syncEnabled = true;
        sendSyncPacket(SYNC_MSG_PING, effectMode);
        Serial.println("Sync: PAIRED");
        for (int f = 0; f < 3; f++) {
          for (int i = 0; i < BLADE_LENGTH; i++) bladeSet(i, CRGB(0, 100, 100));
          FastLED.show(); delay(150);
          bladeClear(); FastLED.show(); delay(100);
        }
      } else if (syncEnabled) {
        sendSyncPacket(SYNC_MSG_PING, effectMode);
      }
    }
  }

  // ── Button handling ─────────────────────────────────────────────────
  bool btn = digitalRead(BUTTON_PIN);
  if (btn == LOW && lastButton == HIGH) {
    uint32_t now = millis();
    if (now - lastClickMs > 40) { clickCount++; lastClickMs = now; buttonDownMs = now; }
  }

  // Triple-click + hold → sync arm
  if (!syncAnimating && clickCount >= 3 && btn == LOW && millis() - buttonDownMs > 80) {
    syncAnimating = true; syncAnimStart = millis(); syncSearching = false; clickCount = 0; boostMode = false;
  }

  // Hold → boost
  if (btn == LOW && !boostMode && !syncAnimating && clickCount < 3 && millis() - buttonDownMs > BOOST_HOLD_MS) {
    boostMode = true; clickCount = 0;
  }
  if (btn == HIGH && lastButton == LOW && boostMode) boostMode = false;

  // Single/double click → cycle mode (after DCLICK_MS timeout)
  if (!boostMode && !syncAnimating && clickCount > 0 && clickCount < 3 && millis() - lastClickMs > DCLICK_MS) {
    currentMode = (currentMode + 1) % MODE_COUNT;
    effectMode  = currentMode;
    clickCount  = 0;
    lastAutoCycleMs = millis(); // reset timer on manual switch
    Serial.printf("Mode → %d\n", currentMode);

    CRGB flashColor;
    switch (currentMode) {
      case MODE_FIRE:      flashColor = CRGB(255, 80, 0);  break;
      case MODE_RAINBOW:   flashColor = CRGB(100, 0, 255); break;
      case MODE_LIGHTNING:  flashColor = CRGB(0, 100, 255); break;
      default:             flashColor = CRGB(255, 255, 255); break;
    }
    for (int f = 0; f < 2; f++) {
      for (int i = 0; i < BLADE_LENGTH; i++) bladeSet(i, flashColor);
      FastLED.show(); delay(80);
      bladeClear(); FastLED.show(); delay(60);
    }
  }
  
  // ── 15-SECOND AUTO-CYCLE ────────────────────────────────────────────
  if (autoCycleMode && !boostMode && !syncAnimating && !fireActive && (millis() - lastAutoCycleMs > 15000)) {
    // Only auto-cycle if the sword is mostly idle (not in middle of swing)
    if (swingMag < 100.0f) {
      currentMode = (currentMode + 1) % MODE_COUNT;
      effectMode  = currentMode;
      lastAutoCycleMs = millis();
      Serial.printf("Auto-Cycle → %d\n", currentMode);
      
      // Soft single flash for auto-cycle so it's less jarring
      CRGB flashColor;
      switch (currentMode) {
        case MODE_FIRE:      flashColor = CRGB(150, 40, 0);  break;
        case MODE_RAINBOW:   flashColor = CRGB(60, 0, 150);  break;
        case MODE_LIGHTNING: flashColor = CRGB(0, 50, 150);  break;
      }
      for (int i = 0; i < BLADE_LENGTH; i++) bladeSet(i, flashColor);
      FastLED.show(); delay(80);
      bladeClear(); FastLED.show();
    } else {
      // Delay the cycle if currently swinging
      lastAutoCycleMs = millis() - 12000; 
    }
  }

  // Discard stale 3+ clicks
  if (!boostMode && !syncAnimating && clickCount >= 3 && btn == HIGH && millis() - lastClickMs > DCLICK_MS) {
    clickCount = 0;
  }

  lastButton = btn;

  // ── Sync-arm animation ──────────────────────────────────────────────
  if (syncAnimating) {
    float progress = (float)(millis() - syncAnimStart) / (float)SYNC_ANIM_MS;
    if (btn == HIGH) {
      syncAnimating = false;
    } else if (progress >= 1.0f) {
      syncAnimating = false;
      if (syncEnabled) {
        syncEnabled = false; Serial.println("Sync: OFF");
        for (int f = 0; f < 3; f++) {
          for (int i = 0; i < BLADE_LENGTH; i++) bladeSet(i, CRGB(100, 40, 0));
          FastLED.show(); delay(150); bladeClear(); FastLED.show(); delay(100);
        }
      } else {
        syncSearching = true; syncSearchStart = millis(); lastPingMs = 0;
        Serial.printf("Sync: searching 60s  MAC: %s\n", WiFi.macAddress().c_str());
        for (int f = 0; f < 3; f++) {
          for (int i = 0; i < BLADE_LENGTH; i++) bladeSet(i, CRGB(0, 80, 100));
          FastLED.show(); delay(150); bladeClear(); FastLED.show(); delay(100);
        }
      }
    } else {
      renderSyncAnim(progress);
      FastLED.setBrightness(BRIGHTNESS); FastLED.show(); delay(16); return;
    }
  }

  // ── Sync search ─────────────────────────────────────────────────────
  if (syncSearching) {
    if (millis() - syncSearchStart >= SYNC_SEARCH_MS) {
      syncSearching = false; Serial.println("Sync: timed out");
      for (int f = 0; f < 3; f++) {
        for (int i = 0; i < BLADE_LENGTH; i++) bladeSet(i, CRGB(100, 40, 0));
        FastLED.show(); delay(150); bladeClear(); FastLED.show(); delay(100);
      }
    } else if (millis() - lastPingMs > 500) {
      sendSyncPacket(SYNC_MSG_PING, effectMode); lastPingMs = millis();
    }
  }

  // ── Effect dispatch ─────────────────────────────────────────────────
  if (syncSearching) {
    renderSyncSearching();
  } else {
    switch (currentMode) {
      case MODE_FIRE:      effectFire();      break;
      case MODE_RAINBOW:   effectRainbow();   break;
      case MODE_LIGHTNING:  effectLightning(); break;
      default:             effectFire();      break;
    }
  }

  // ── Overlays ────────────────────────────────────────────────────────
  if (!syncSearching && !fireActive) updateAndRenderRollOverlay();

  if (!fireActive && boostMode) {
    renderBoostSparks();
    float pulse = 0.72f + 0.28f * sinf((float)millis() * 0.050f);
    FastLED.setBrightness((uint8_t)(BRIGHTNESS * pulse));
  } else {
    FastLED.setBrightness(BRIGHTNESS);
  }

  if (!syncSearching && !fireActive) updateAndRenderImpacts();

  // ── POV overlay — world-locked pattern when swinging hard ──────────
  if (!syncSearching && !syncAnimating)
    renderPOVOverlay(currentMode);

  FastLED.show();
  delay(16);
}
