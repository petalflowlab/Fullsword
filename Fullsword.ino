
// =====================================================================
// Fullsword_IMU.ino — TestIMU-Only Slim Build (OTA SAFE)
// =====================================================================
//
// This is a stripped-down version of Fullsword.ino that keeps all
// infrastructure (WiFi, OTA, WebServer, ESP-NOW, IMU) but only runs
// the effectTestIMU mode. All other effects have been removed.
//
// LED layout (184 total):
//   leds[0..3]   = Accent LEDs (base of strip)
//   leds[4..183] = Blade (hilt→tip in bladeSet/bladeGet coordinate)
//
// IMU axes (MPU9250 at 0x68):
//   accelY < -0.4g → tip pointing UP   → fire pulses
//   accelY > +0.4g → tip pointing DOWN → white chaser
//   accelY ≈ 0     → flat/horizontal   → blue lightning
//   swingMag spike → swing detected    → swing fireball
//   accelY jerk    → thrust detected   → thrust fireball
//
// Button (D3): Hold > 0.5s = Boost  |  Triple+Hold = ESP-NOW sync arm
// OTA hostname: Fullsword  |  OTA password: sword
// =====================================================================

#include <FastLED.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <esp_now.h>

// ─── PIN CONFIGURATION ────────────────────────────────────────────────
#define LED_PIN       D7
#define BUTTON_PIN    D3
#define SDA_PIN       D4
#define SCL_PIN       D5

// ─── LED CONFIGURATION ────────────────────────────────────────────────
#define NUM_LEDS      184
#define BLADE_LENGTH  180     // usable blade positions (bladeSet coords)
#define HILT_LEDS     15      // first 15 positions reserved as hilt
#define BLADE_START   HILT_LEDS
#define BLADE_END     BLADE_LENGTH
#define BLADE_PIXELS  (BLADE_END - BLADE_START)   // = 165
#define BRIGHTNESS    180
#define OTA_BRIGHTNESS 60

// ─── IMU ──────────────────────────────────────────────────────────────
#define IMU_ADDR      0x68

// ─── WIFI / OTA ───────────────────────────────────────────────────────
const char* ssid        = "CGN3-4400";
const char* password    = "251148015432";
const char* hostname    = "Fullsword";
const char* otaPassword = "sword";
WebServer   server(80);

bool     otaActive   = false;
uint16_t otaProgress = 0;

// ─── GLOBAL LED ARRAY ─────────────────────────────────────────────────
CRGB leds[NUM_LEDS];

// ─── IMU STATE ────────────────────────────────────────────────────────
bool   imuOk     = false;
float  swingMag  = 0.0f;   // filtered transverse gyro magnitude (deg/s)
float  currSwing = 0.0f;   // raw transverse gyro magnitude (deg/s)
float  twistRate = 0.0f;   // filtered axial gyro gz (deg/s)
float  accelX    = 0.0f;
float  accelY    = 0.0f;
float  accelZ    = 0.0f;
float  gx = 0.0f, gy = 0.0f, gz = 0.0f;
int8_t tiltDir   =  1;

// ─── EFFECTS STATE ────────────────────────────────────────────────────
uint8_t  effectMode   = 0;     // Always 0 (TestIMU) in this slim build
uint8_t  baseHue      = 190;   // purple-blue base hue

uint8_t  clickCount   = 0;
uint32_t lastClickMs  = 0;
bool     lastButton   = HIGH;
#define  DCLICK_MS    350

bool     boostMode    = false;
uint32_t buttonDownMs = 0;
#define  BOOST_HOLD_MS  500
#define  SYNC_ANIM_MS   2000
#define  SYNC_MSG_MODE  0x01
#define  SYNC_MSG_PING  0x02
#define  SYNC_SEARCH_MS 60000

// ─── ROLLING OVERLAY PHYSICS ──────────────────────────────────────────
float rollPhase = 0.0f;
float lastGyroZ = 0.0f;

// ─── FIRE PRIORITY FLAG ───────────────────────────────────────────────
// True from swing trigger through full tipGlow fade. All other effects
// and overlays must check this and yield immediately.
bool fireActive = false;

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

// ─── ESP-NOW SYNC STATE ────────────────────────────────────────────────
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

// ─── MIC ESP-NOW AUDIO ────────────────────────────────────────────────
typedef struct { float bass, mid, treble, totalVolume; int currentMode; } MicPacket;
float    fswBass  = 0.0f, fswMid = 0.0f, fswTreb = 0.0f, fswVol = 0.0f;
bool     fswBeat  = false;
float    fswBeatI = 0.0f;
uint32_t fswLastPkt = 0;


// =====================================================================
// BLADE HELPERS
// =====================================================================

inline void bladeSet(int pos, CRGB color) {
  if (pos < 0 || pos >= BLADE_LENGTH) return;
  leds[4 + pos] = color;
}

inline CRGB bladeGet(int pos) {
  if (pos < 0 || pos >= BLADE_LENGTH) return CRGB::Black;
  return leds[4 + pos];
}

void bladeClear() {
  fill_solid(leds + 4, NUM_LEDS - 4, CRGB::Black);
}

void renderAccents() {
  float pulse = (sinf((float)millis() * 0.002f) + 1.0f) * 0.5f;
  uint8_t bri = (uint8_t)(pulse * 150.0f + 50.0f);
  CRGB accentColor = CHSV(baseHue + 20, 200, bri);
  leds[0] = leds[1] = leds[2] = leds[3] = accentColor;
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
  Wire.beginTransmission(0x68); Wire.write(0x1B); Wire.write(0x08); Wire.endTransmission(); // Gyro ±500dps
  Wire.beginTransmission(0x68); Wire.write(0x1C); Wire.write(0x00); Wire.endTransmission(); // Accel ±2g

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
  swingMag  = swingMag  * 0.55f + currSwing * 0.45f; // Much faster response for "flicks"
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
// EFFECT 0 — TEST IMU
// Calibration and Verification Mode.
// 
// CALIBRATION NOTES (IMU parallel to blade, Y-axis is longitudinal):
// - Sword Pointing UP:   accelY < -0.4G → fire pulses hilt→tip
// - Sword Pointing DOWN: accelY > +0.4G → white chaser hilt→tip
// - Sword FLAT:          accelY ~  0    → blue lightning chaser
// - Quick Twist:         twistRate spike → white strobe
// - Swing:               swingMag spike  → fireball hilt→tip
// - Thrust/Pull:         accelY jerk     → fireball hilt→tip
// =====================================================================
void effectTestIMU() {
  const float dt = 0.016f;
  static float fizzle = 0.0f;
  static float lastSwing = 0.0f;
  static float hiltFlash = 0.0f;

  // ── FIRE HIERARCHY: declare state first so fireActive can be computed ──
  static uint32_t fireStartMs  = 0;
  static float    fireSpeed    = 0.0f;
  static bool     fireLive     = false;
  static float    prevSwingMag = 0.0f;
  static bool     fireCharge   = false;
  static uint32_t chargeStart  = 0;
  static bool     fireCompressing = false;
  static uint32_t compressStartMs = 0;
  static float    lavaIntensity   = 0.0f;
  static uint32_t stopFlashMs     = 0;
  static uint32_t diagStopMs      = 0;  // DIAGNOSTIC blue flash
  static uint32_t diagFadeMs      = 0;  // DIAGNOSTIC purple flash
  static float    lastLavaInt     = 0.0f; 

  static float   fireTrail[BLADE_LENGTH] = {0};
  static uint8_t trailHue[BLADE_LENGTH]   = {0};
  static float   trailMaxHalf = 0.0f;
  
  // ── LAVA STATE: moved to top for trigger access ────────────────
  static float lava[BLADE_LENGTH] = {};
  static float lavaPhase    = 0.0f;
  static float hbPhase      = 0.0f; // heartbeat phase
  static float shimmerPhase = 0.0f; // heat shimmer phase

  // ── Strike Phases: Block background only during high-priority live/charge ──
  bool strikeBlocking = fireCharge || fireLive;
  bool fizzleActive   = fireCompressing || (trailMaxHalf > 0.01f);
  fireActive          = strikeBlocking || fizzleActive;

  // ── Blade clearance: Only clear the full blade during the blocking phase ──
  if (strikeBlocking) {
    bladeClear();
  } else {
    // Standard hilt clear for idle/fizzle
    for (int i = 0; i < HILT_LEDS; i++) bladeSet(i, CRGB::Black);
  }

  // ── Hilt flash (suppress during fire sequence) ────────────────────────
  if (!fireActive) {
    if (swingMag > 200.0f || fabsf(twistRate) > 300.0f) hiltFlash = 1.0f;
    if (hiltFlash > 0.01f) {
      uint8_t bri = (uint8_t)(hiltFlash * 220.0f);
      CRGB hiltColor;
      if (fabsf(twistRate) > 300.0f) hiltColor = CRGB::White;
      else if (accelY < -0.4f)       hiltColor = CRGB(bri, bri/4, 0); // Orange pointing up
      else                            hiltColor = CRGB(bri, bri, bri/2); // Gold-ish white for standard swing
      for (int i = 0; i < HILT_LEDS; i++) bladeSet(i, hiltColor);
      hiltFlash *= 0.80f;
    }
  } else {
    hiltFlash = 0.0f;  
    // Only reset if it's not already in a negative "strike delay" phase
    if (lavaIntensity > 0.0f) lavaIntensity = 0.0f; 
  }

  // ── DIAGNOSTIC HILT FLASHES: Blue (Stop) and Purple (Fade-in) ──────────
  if (diagStopMs && (millis() - diagStopMs < 300)) {
    float t = (float)(millis() - diagStopMs) / 300.0f;
    uint8_t bri = (uint8_t)((1.0f - t) * 255.0f);
    for (int i = 0; i < HILT_LEDS; i++) bladeSet(i, CRGB(0, 0, bri));
  } else if (diagFadeMs && (millis() - diagFadeMs < 300)) {
    float t = (float)(millis() - diagFadeMs) / 300.0f;
    uint8_t bri = (uint8_t)((1.0f - t) * 255.0f);
    for (int i = 0; i < HILT_LEDS; i++) bladeSet(i, CRGB(bri, 0, bri));
  }

  // ── 1. Orientation & Twist — SKIPPED while strike is blocking ────────
  bool twisting = (!strikeBlocking) && (fabsf(twistRate) > 600.0f);

  if (!strikeBlocking && twisting) {
    if ((millis() / 30) % 2) fill_solid(leds + 4 + HILT_LEDS, BLADE_PIXELS, CRGB::White);
    else                      fill_solid(leds + 4 + HILT_LEDS, BLADE_PIXELS, CRGB::Black);
  } else if (!strikeBlocking) {
      // High-energy fizzle-in: starts at 40% and fills in 4 frames
      lavaIntensity = fminf(1.0f, lavaIntensity + 0.15f);
      if (lastLavaInt <= 0.0f && lavaIntensity > 0.0f) {
        diagFadeMs = millis();
      }
      lastLavaInt = lavaIntensity;

      float drawIntensity = fmaxf(0.0f, lavaIntensity);

      // Dynamic sway metric: combines hilt tilt and rotation speed
      float sway = (fabsf(gx) + fabsf(gy) + fabsf(gz)) * 0.0001f;
      lavaPhase    += 0.022f + sway * 2.0f; 
      hbPhase      += 0.008f; 
      shimmerPhase += 0.065f; // fast shimmer

      // ── Heartbeat modulation: pulsing life ──────────────────────────
      // hb travels from 0.0 (red/inhale) to 1.0 (yellow/exhale)
      float hb = 0.5f + 0.5f * sinf(hbPhase); 
      
      // ── Step 1: Inject heat at the hilt (source) ─────────────────────
      // Heartbeat drives the pulse intensity
      float hiltHeat = (0.55f + 0.45f * hb) + fminf(sway * 5.0f, 0.2f);
      for (int i = HILT_LEDS; i < HILT_LEDS + 8; i++) {
        float fade = 1.0f - (float)(i - HILT_LEDS) / 8.0f;
        lava[i] = fmaxf(lava[i], hiltHeat * fade);
      }

      // ── Step 2: Diffuse heat upward (tip direction) with mild cooling──
      // Cooling is more aggressive when heartbeat is low (inhaling heat)
      for (int i = BLADE_LENGTH - 1; i > HILT_LEDS; i--) {
        float pos = (float)(i - HILT_LEDS) / BLADE_PIXELS;
        // Base cooling is now 0.982 (slightly faster than before)
        // Pulsing cooling prevents heat accumulation
        float coolRate = (0.978f - pos * 0.015f) + (hb * 0.010f); 
        float diffusionBias = 0.040f + fminf(sway * 0.1f, 0.05f);
        
        lava[i] = lava[i] * coolRate + lava[i - 1] * diffusionBias;
        
        // Wobble frequency and amplitude scale with sway
        float wobbleAmp = 0.015f + sway * 0.4f + swingMag * 0.0001f;
        lava[i] += wobbleAmp * sinf(lavaPhase * 2.1f + (float)i * (0.18f + sway));
        lava[i] = constrain(lava[i], 0.0f, 1.0f);
      }

        // ── Step 3: Render heat → lava palette ───────────────────────────
        float pulseR = 0.88f + 0.12f * sinf(shimmerPhase * 0.70f);
        float pulseO = 0.86f + 0.14f * sinf(shimmerPhase * 1.20f + 1.0f);
        float pulseY = 0.84f + 0.16f * sinf(shimmerPhase * 1.80f + 2.0f);

        for (int i = HILT_LEDS; i < BLADE_LENGTH; i++) {
          float h = lava[i];
          // Heat Shimmer: uneven, multi-frequency brightness pulse
          float shimmer = 0.86f + 0.14f * (sinf(shimmerPhase) * sinf(shimmerPhase * 0.43f));
          float flicker = 0.90f + 0.10f * sinf(lavaPhase * 4.7f + (float)i * 0.31f);
          h *= (flicker * shimmer);
          h = constrain(h, 0.0f, 1.0f);

          CRGB color;
          if (h < 0.55f) {
            // Bright Red (expanded range)
            uint8_t r = (uint8_t)(fminf(1.0f, h / 0.55f + 0.2f) * 200 * pulseR);
            color = CRGB(r, 0, 0);
          } else if (h < 0.80f) {
            // Deep Orange-Red
            float t  = (h - 0.55f) / 0.25f;
            uint8_t r = (uint8_t)((180 + t * 65) * pulseO);
            uint8_t g = (uint8_t)((t * t * 45) * pulseO);
            color = CRGB(r, g, 0);
          } else if (h < 0.90f) {
            // Hot Orange
            float t  = (h - 0.80f) / 0.10f;
            uint8_t r = (uint8_t)((245 + t * 10) * pulseO);
            uint8_t g = (uint8_t)((45  + t * 85) * pulseO);
            color = CRGB(r, g, 0);
          } else {
            // Yellow only on extreme peaks
            float t  = (h - 0.90f) / 0.10f;
            uint8_t r = (uint8_t)(255 * pulseY);
            uint8_t g = (uint8_t)((130 + t * 125) * pulseY);
            uint8_t b = (uint8_t)((t * 70) * pulseY);
            color = CRGB(r, g, b);
          }
        
        // Stochastic Fizzle Mask: pixels pop in at full brightness
        if (random8() > (uint8_t)(lavaIntensity * 255.0f)) {
           bladeSet(i, CRGB::Black);
           continue;
        }

        // Apply smooth but FAST fade-in to the pixels that ARE showing
        color.nscale8((uint8_t)(fmaxf(0.5f, lavaIntensity) * 255.0f));
        bladeSet(i, color);
      }

      // ── Step 4: Occasional glowing ember spark ────────────────────────
      if (random8() < 8) {
        int pos = HILT_LEDS + random16(BLADE_PIXELS / 2);
        bladeSet(pos, bladeGet(pos) + CRGB(random8(30, 80), random8(5, 20), 0));
      }
    }

  // ── 2. Swing Fireball — charge blast at hilt → 2-phase launch ──────────
  // fireCharge / fireLive / chargeStart already declared above for hierarchy.
  #define CHARGE_MS 120

  // ── SENSOR CALCULATION ──────────────────────────────────────────────
  static uint32_t lastTriggerMs = 0;
  float dSwing = swingMag - prevSwingMag;
  prevSwingMag = swingMag;

  // ── SUDDEN STOP DETECTION: High sensitivity ──────────────────────────
  static uint32_t lastStopDetectMs = 0;
  // Refined: Aggressive jerk detection (dSwing < -15) and raw speed check (currSwing < 40)
  if (swingMag > 50.0f && dSwing < -15.0f && (currSwing < 40.0f) && (millis() - lastStopDetectMs > 500)) {
    stopFlashMs     = millis();
    diagStopMs      = millis();
    lastStopDetectMs = millis();
    lastTriggerMs    = millis() + 450; // Block accidental swing re-trigger
    
    // RECOVERY SEQUENCE START: IMMEDIATE + PRE-WARM
    lavaIntensity   = 0.4f; // START FIRING IMMEDIATELY
    lastLavaInt     = 0.0f;
    
    // Pop-in base heat across the whole blade to ensure immediate glow
    for (int i = HILT_LEDS; i < BLADE_LENGTH; i++) {
       lava[i] = (float)random8(20, 60) / 100.0f; 
    }
    
    // Instant Recovery: Kill all fire animations
    fireCharge      = false;
    fireLive        = false;
    fireCompressing = false;
    memset(fireTrail, 0, sizeof(fireTrail));
  }

  // ── FIRE HILT-CHARGE: Start a new strike ──────────────────────────

  // Reduced jerk threshold (25.0) to catch mid-swing pumps
  bool swingPredict = (swingMag > 90.0f && dSwing > 25.0f);
  bool swingStrong  = (swingMag > 260.0f);

  // canRetrigger: fireball is either finished or in trail-fade
  bool animateDone = !fireCharge && !fireLive && !fireCompressing;
  
  // jerkResets: sudden "flick" always resets (except during the 120ms charge)
  // strongRetriggers: sustained swing fires again after 350ms cooldown
  if ((swingPredict && !fireCharge) || (swingStrong && animateDone && (millis() - lastTriggerMs > 350))) {
    // GLOBAL RESET: Purge all active effects immediately for a fresh strike
    lastTriggerMs = millis();
    fireCharge  = true;
    chargeStart = millis();
    fireLive    = false;
    fireCompressing = false;
    trailMaxHalf  = 0.0f;
    lavaIntensity = 0.4f;    // START FIRING IMMEDIATELY
    lastLavaInt   = 0.0f;    // Reset for recovery
    hiltFlash     = 1.0f;
    // Pop-in base heat across the whole blade
    for (int i = HILT_LEDS; i < BLADE_LENGTH; i++) {
       lava[i] = (float)random8(20, 60) / 100.0f; 
    }
    memset(fireTrail, 0, sizeof(fireTrail));
  }

  if (fireCharge && !fireLive && (millis() - chargeStart > CHARGE_MS)) {
    fireCharge  = false;
    fireLive    = true;
    fireStartMs = millis();
    // Targeted for ~1.0s total: 5 * fastMs (fastMs = 200ms)
    // 200ms = 82.5 / (fireSpeed * 60) -> fireSpeed = 0.006875
    fireSpeed   = 0.006f + swingMag * 0.00001f;
    if (fireSpeed > 0.012f) fireSpeed = 0.012f;
  }

  // ── CHARGE PHASE: pulsing hilt blast ─────────────────────────────────
  if (fireCharge) {
    // Redundant clear removed; bladeClear() at start handles it
    float chargeT = (float)(millis() - chargeStart) / (float)CHARGE_MS;
    int chargeWidth = 12 + (int)(chargeT * 8);
    for (int j = 0; j < chargeWidth; j++) {
      int idx = HILT_LEDS + j;
      if (idx >= BLADE_LENGTH) break;
      float falloff = 1.0f - (float)j / chargeWidth;
      float twinkle = random8(160, 255) / 255.0f;
      if (j < 5) {
        bladeSet(idx, CRGB(255, (uint8_t)(220 * falloff * twinkle), 0));
      } else {
        bladeSet(idx, CRGB((uint8_t)(255 * falloff * twinkle), (uint8_t)(70 * falloff * falloff * twinkle), 0));
      }
    }
  }

  // ── LIVE PHASE: 30px fireball with 2-phase speed ──────────────────────
  if (fireLive) {
    float elapsed  = (float)(millis() - fireStartMs);
    float midPoint = (float)BLADE_PIXELS * 0.50f;
    float fastMs   = midPoint / (fireSpeed * 60.0f); // time to reach midpoint at full speed

    float pos;
    if (elapsed <= fastMs) {
      // PHASE 1: Full speed — hilt to halfway
      pos = elapsed * fireSpeed * 60.0f;
    } else {
      // PHASE 2: sqrt-eased decel — halfway to tip (takes 4x as long)
      float t2 = (elapsed - fastMs) / (fastMs * 4.0f);
      t2 = fminf(t2, 1.0f);
      pos = midPoint + (float)BLADE_PIXELS * 0.50f * sqrtf(t2);
    }
    pos += (float)HILT_LEDS;

    if (pos >= (float)(BLADE_LENGTH - 1)) {
      fireLive        = false;
      fireCompressing = true;
      compressStartMs = millis();
    } else {
      int center = (int)pos;

      // 30px body: [0-9] yellow core | [10-19] orange | [20-29] red outer
      for (int j = 0; j < 30; j++) {
        int idx = center - j;
        if (idx < HILT_LEDS || idx >= BLADE_LENGTH) continue;
        CRGB color;
        if (j < 10) {
          // 10px yellow-white core (front of fireball)
          float coreFrac = (float)j / 10.0f;
          float twinkle  = random8(200, 255) / 255.0f;
          uint8_t g = (uint8_t)((1.0f - coreFrac * 0.6f) * 220.0f * twinkle);
          color = CRGB(255, g, 0);
        } else if (j < 20) {
          // 10px orange mid-band
          float t = (float)(j - 10) / 10.0f;
          float twinkle = random8(180, 255) / 255.0f;
          uint8_t g = (uint8_t)((1.0f - t * 0.75f) * 110.0f * twinkle);
          color = CRGB(255, g, 0);
        } else {
          // 10px red outer tail
          float t = (float)(j - 20) / 10.0f;
          uint8_t r = (uint8_t)((1.0f - t) * 200.0f);
          color = CRGB(r, 0, 0);
        }
        
        if (color) {
          bladeSet(idx, color); // Fireball OVERWRITES background for punchy core
          // Trail energy: always set to max (1.0) while fireball is passing
          fireTrail[idx] = 1.0f;
          trailHue[idx]  = (j < 15) ? 32 : 5; // amber core, deep red tail
        }
      }
    }
  }

  // ── COMPRESSION PHASE: fire smushes into the tip ───────────────────────
  if (fireCompressing) {
    float compressT = (float)(millis() - compressStartMs) / 500.0f; // 500ms compression
    if (millis() - compressStartMs > 500) {
      fireCompressing = false;
    } else {
      int center = BLADE_LENGTH - 1;
      int width  = (int)(30.0f * (1.0f - compressT));
      
      for (int j = 0; j < width; j++) {
        int idx = center - j;
        if (idx < HILT_LEDS) continue;
        
        // As width shrinks, the bands compress and merge
        // T=0: 0-9 yellow, 10-19 orange, 20-29 red
        // T=0.5: 0-4 yellow, 5-9 orange, 10-14 red
        float p = (float)j / (float)(width > 0 ? width : 1);
        CRGB color;
        if (p < 0.33f) {
          color = CRGB(255, (uint8_t)(220 * (1.0f - p*3)), 0); // Yellow-ish
        } else if (p < 0.66f) {
          float t = (p - 0.33f) * 3.0f;
          color = CRGB(255, (uint8_t)(110 * (1.0f - t)), 0);   // Orange-ish
        } else {
          float t = (p - 0.66f) * 3.0f;
          color = CRGB((uint8_t)(200 * (1.0f - t)), 0, 0);     // Red-ish
        }
        bladeSet(idx, bladeGet(idx) + color);
      }
    }
  }

  // ── TRAIL RENDERING: sparkling disintegration ──────────────────────────
  // NEW: Hard-limit trail to EXACTLY 0.5s after fireball hits or stop occurs
  if (fireActive && !strikeBlocking && (millis() - compressStartMs > 500)) {
     memset(fireTrail, 0, sizeof(fireTrail));
  }
  
  trailMaxHalf = 0.0f;
  if (fireActive) {
    for (int i = HILT_LEDS; i < BLADE_LENGTH; i++) {
      if (fireTrail[i] > 0.01f) {
        // Accelerated decay (vanish within ~1.5s)
        float posFrac = (float)(i - HILT_LEDS) / (float)BLADE_PIXELS;
        float decayRate = (0.84f + posFrac * 0.10f);
        if (swingMag < 60.0f || fireTrail[i] < 0.70f) { // Drop priority if swing is low or trail is weak
          decayRate *= 0.85f; // Make it decay faster
        }
        fireTrail[i] *= decayRate;
        
        // Track energy specifically for the first half (for early exit)
        if (i < (HILT_LEDS + BLADE_PIXELS / 2)) {
          if (fireTrail[i] > trailMaxHalf) trailMaxHalf = fireTrail[i];
        }
        
        // High-contrast sparkle
        float sparkle = 0.35f + (float)random8(165) / 255.0f;
        uint8_t bri = (uint8_t)(fireTrail[i] * 240.0f * sparkle);
        if (bri > 5) {
          uint8_t sat = 210 + random8(45);
          bladeSet(i, bladeGet(i) + CHSV(trailHue[i], sat, bri));
        }
      }
    }
  }


  // ── 3. Thrust / Pull also triggers fireball ────────────────────────────
  static float prevAY = 0.0f;
  float dAY = accelY - prevAY;
  prevAY = accelY;
  if (fabsf(dAY) > 0.5f && swingMag < 80.0f && !fireLive && !fireCharge) {
    fireCharge  = true;
    chargeStart = millis();
    fireSpeed   = 0.50f;
    hiltFlash   = 1.0f;
  }

  // ── 4. PERMANENT HILT/BASE FIRE: always visible ──────────────────────
  // This renders 12 pixels of core bubbling fire at the base
  {
    static float hiltPhase = 0.0f;
    hiltPhase += 0.035f;
    for (int j = 0; j < 12; j++) {
      int idx = HILT_LEDS + j;
      if (idx >= BLADE_LENGTH) break;
      float falloff = 1.0f - (float)j / 12.0f;
      float pulse = 0.85f + 0.15f * sinf(hiltPhase + (float)j*0.4f);
      float twinkle = random8(200, 255) / 255.0f;
      
      CRGB hiltColor;
      if (j < 4) {
        hiltColor = CRGB(255, (uint8_t)(200 * falloff * pulse * twinkle), 0);
      } else {
        hiltColor = CRGB((uint8_t)(255 * falloff * pulse), (uint8_t)(60 * falloff * falloff), 0);
      }
      
      // Blend OVER existing base color
      CRGB existing = bladeGet(idx);
      bladeSet(idx, existing.lerp8(hiltColor, (uint8_t)(falloff * 255)));
    }
  }

  // ── 5. SUDDEN STOP FLASH: High-Visibility center-out pulse ───────────
  uint32_t stopAge = millis() - stopFlashMs;
  if (stopAge < 250) {
    float t = (float)stopAge / 250.0f;
    int center = HILT_LEDS + BLADE_PIXELS/2;
    float radius = t * (BLADE_PIXELS / 2 + 15);
    float width = 14.0f * (1.0f - t); 
    
    for (int i = HILT_LEDS; i < BLADE_LENGTH; i++) {
      float dist = fabsf((float)(i - center) - radius);
      float dist2 = fabsf((float)(i - center) + radius);
      float proximity = fminf(dist, dist2);
      
      if (proximity < width) {
        float intensity = (1.0f - proximity / width) * (1.0f - t);
        // Ultra-High-Intensity Additive White
        CRGB flashColor = CRGB(255, 255, 255); 
        CRGB current = bladeGet(i);
        // Additive blend with saturation
        bladeSet(i, current + flashColor.nscale8((uint8_t)(intensity * 255.0f)));
      }
    }
  }

  // ── 6. Quick Stop & Fade ──────────────────────────────────────────────
  if (lastSwing > 180.0f && swingMag < 80.0f) fizzle = 1.0f;
  if (fizzle > 0.01f) {
    for (int i = 0; i < BLADE_LENGTH; i++) {
      if (random8() < 50 * fizzle) { CRGB c = bladeGet(i); c.nscale8(180); bladeSet(i, c); }
    }
    fizzle *= 0.92f;
  }
  lastSwing = swingMag;

  // ── 5. Serial Readout for Calibration ────────────────────────────────
  static uint32_t lastPrint = 0;
  if (millis() - lastPrint > 500) {
    Serial.printf("TEST IMU | aX:%.2f aY:%.2f aZ:%.2f | swing:%.1f twist:%.1f\n",
                  accelX, accelY, accelZ, swingMag, twistRate);
    lastPrint = millis();
  }
}


// ─── ROLLING SPARKLE OVERLAY ──────────────────────────────────────────
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

// ─── BOOST SPARKS ─────────────────────────────────────────────────────
void renderBoostSparks() {
  int count = constrain(3 + (int)(swingMag * 0.06f), 3, 10);
  for (int s = 0; s < count; s++) {
    int pos = random(BLADE_LENGTH);
    bladeSet(pos, bladeGet(pos) + CHSV(baseHue + (int8_t)(random8()/4 - 32), random8(60, 200), random8(160, 255)));
  }
}


// =====================================================================
// ESP-NOW SYNC — receive callback, send helper, setup
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

void renderSyncIndicator() {
  float   pulse = (sinf((float)millis() * 0.004f) + 1.0f) * 0.5f;
  uint8_t bri   = (uint8_t)(pulse * 70.0f + 15.0f);
  leds[0]            += CRGB(0, bri, bri);
  leds[NUM_LEDS - 1] += CRGB(0, bri, bri);
}


// =====================================================================
// OTA VISUAL
// =====================================================================
void renderOTAMode() {
  FastLED.setBrightness(OTA_BRIGHTNESS);
  static bool     blinkState = false;
  static uint32_t lastBlink  = 0;
  if (millis() - lastBlink > 300) { blinkState = !blinkState; lastBlink = millis(); }
  FastLED.clear();
  for (int i = 0; i < NUM_LEDS; i += 5) if (blinkState) leds[i] = CRGB(0, 0, 150);
  int filled = (otaProgress * BLADE_LENGTH) / 100;
  for (int i = 0; i < filled; i++) bladeSet(i, CRGB(0, 100, 255));
  FastLED.show();
}


// =====================================================================
// WIFI + OTA SETUP
// =====================================================================
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
    server.begin();
  }
  ArduinoOTA.setHostname(hostname);
  ArduinoOTA.setPassword(otaPassword);
  ArduinoOTA.onStart([]()     { otaActive = true; otaProgress = 0; Serial.println("OTA Start"); });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int tot) { otaProgress = (p*100)/tot; });
  ArduinoOTA.onEnd([]()       { Serial.println("OTA End"); });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("OTA Error[%u]\n", e); });
  ArduinoOTA.begin();
  setupESPNow();
}

// =====================================================================
// WEB SERVER HANDLERS
// =====================================================================
void handleRoot() {
  String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{background:#111;color:#eee;font-family:sans-serif;text-align:center;}";
  html += "h1{color:#fa6;} p{color:#aaa;}</style></head><body>";
  html += "<h1>Fullsword IMU Mode</h1>";
  html += "<p>Running: <b>Test IMU</b> (Fireball + Orientation)</p>";
  html += "<p>aX:" + String(accelX,2) + "  aY:" + String(accelY,2) + "  aZ:" + String(accelZ,2) + "</p>";
  html += "<p>swing:" + String(swingMag,1) + "  twist:" + String(twistRate,1) + "</p>";
  html += "</body></html>";
  server.send(200, "text/html", html);
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

  // Startup sweep — travels from hilt to tip in amber
  for (int i = 0; i < BLADE_LENGTH; i++) {
    bladeSet(i, CRGB(200, 80, 0));
    FastLED.show();
    delay(4);
  }
  delay(200);
  bladeClear();
  FastLED.show();

  setupWiFiOTA();
  Serial.println("Ready.  TripleHold=sync  Hold=boost");
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

  // ── Handle incoming ESP-NOW sync packet ────────────────────────────
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

  // ── Button: HOLD=boost  TRIPLE-CLICK+HOLD=sync arm ─────────────────
  bool btn = digitalRead(BUTTON_PIN);
  if (btn == LOW && lastButton == HIGH) {
    uint32_t now = millis();
    if (now - lastClickMs > 40) { clickCount++; lastClickMs = now; buttonDownMs = now; }
  }
  if (!syncAnimating && clickCount >= 3 && btn == LOW && millis() - buttonDownMs > 80) {
    syncAnimating = true; syncAnimStart = millis(); syncSearching = false; clickCount = 0; boostMode = false;
  }
  if (btn == LOW && !boostMode && !syncAnimating && clickCount < 3 && millis() - buttonDownMs > BOOST_HOLD_MS) {
    boostMode = true; clickCount = 0;
  }
  if (btn == HIGH && lastButton == LOW && boostMode) boostMode = false;
  lastButton = btn;

  // Discard single clicks silently (no mode change in this slim build)
  if (!boostMode && !syncAnimating && clickCount > 0 && millis() - lastClickMs > DCLICK_MS) {
    clickCount = 0;
  }

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

  // ── Sync search: ping every 500ms, timeout after 60s ───────────────
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
    effectTestIMU();
  }

  // ── Overlays — all suppressed while fire sequence is active ──────────
  if (!syncSearching && !fireActive) updateAndRenderRollOverlay();

  if (!fireActive && boostMode) {
    renderBoostSparks();
    float pulse = 0.72f + 0.28f * sinf((float)millis() * 0.050f);
    FastLED.setBrightness((uint8_t)(BRIGHTNESS * pulse));
  } else {
    FastLED.setBrightness(BRIGHTNESS);
  }

  if (syncEnabled) renderSyncIndicator();
  if (!syncSearching && !fireActive) updateAndRenderImpacts();
  renderAccents();

  FastLED.show();
  delay(16);   // ~60 fps
}