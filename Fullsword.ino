// =====================================================================
// PurpleSword.ino — Mirrored IMU-driven LED Sword (OTA SAFE)
// =====================================================================
//
// LED layout (86 total, 43 per side):
//   leds[0..42]  = Side A, hilt→tip
//   leds[43..85] = Side B, tip→hilt
//   bladeSet(pos, color) writes pos on A and the mirror on B simultaneously.
//
// IMU axes (MPU6050/9250 at 0x68):
//   az > 0   → tip pointing up   → chaser moves tip-ward
//   az < 0   → tip pointing down → chaser moves hilt-ward
//   gx/gy    → swing magnitude   → boosts chaser speed / ripple energy
//   gz       → twist rate        → shifts hue
//
//   *** If directions feel inverted, negate az/gz below in updateIMU(). ***
//
// Button (D8): cycles Chase → Pulse → Ripple → Chase
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
#define BLADE_LENGTH  180     // single stripped blade (no mirror)
#define HILT_LEDS     15      // first 15 positions reserved as hilt (normally off)
#define BLADE_START   HILT_LEDS  // effects start from here
#define BLADE_END     BLADE_LENGTH
#define BLADE_PIXELS  (BLADE_END - BLADE_START)   // = 165
#define BRIGHTNESS    180
#define OTA_BRIGHTNESS 60

// ─── IMU ──────────────────────────────────────────────────────────────
#define IMU_ADDR      0x68

// ─── WIFI / OTA ───────────────────────────────────────────────────────
const char* ssid      = "CGN3-4400";
const char* password  = "251148015432";
const char* hostname  = "Fullsword";
const char* otaPassword = "sword"; // Plain text OTA password
WebServer   server(80);

bool     otaActive   = false;
uint16_t otaProgress = 0;

// ─── GLOBAL LED ARRAY ─────────────────────────────────────────────────
CRGB leds[NUM_LEDS];

// ─── IMU STATE ────────────────────────────────────────────────────────
bool  imuOk    = false;
float swingMag = 0.0f;   // filtered transverse gyro magnitude (deg/s)
float twistRate= 0.0f;   // filtered axial gyro gz (deg/s)
float accelX   = 0.0f;   // Transverse X (g)
float accelY   = 0.0f;   // Transverse Y (g)
float accelZ   = 0.0f;   // filtered accel along blade axis (g) — drives painter
int8_t tiltDir =  1;     // +1 = tip-up → chase toward tip; -1 = tip-down

// ─── EFFECTS STATE ────────────────────────────────────────────────────
uint8_t  effectMode   = 0;     // Cycle: TestIMU, Painter, PingPong, FireStorm, OceanWaves, PlasmaStorm, MusicModes...
uint8_t  baseHue      = 190;   // purple-blue base (~190 in FastLED HSV)

uint8_t  clickCount   = 0;     // button clicks waiting to be dispatched
uint32_t lastClickMs  = 0;     // millis() of most recent press
bool     lastButton   = HIGH;
#define  DCLICK_MS    350      // double-click detection window (ms)

bool     boostMode    = false; // true while D8 is held > BOOST_HOLD_MS
uint32_t buttonDownMs = 0;     // millis() when button last went LOW
#define  BOOST_HOLD_MS 500     // hold threshold (ms)
#define  SYNC_ANIM_MS  2000    // triple-click+hold animation duration (ms)
#define  SYNC_MSG_MODE   0x01    // ESP-NOW packet: mode changed
#define  SYNC_MSG_PING   0x02    // ESP-NOW packet: presence beacon
#define  SYNC_SEARCH_MS  60000   // discovery window: 60 seconds

// ─── ROLLING OVERLAY PHYSICS ──────────────────────────────────────────
float rollPhase = 0.0f;
float lastGyroZ = 0.0f;

// ─── SWORD PAINTER STATE ──────────────────────────────────────────────
struct {
  float buf[BLADE_LENGTH];  // per-pixel hue (0–255 float)
  float paintCenter;      // 0=hilt, 1=tip (normalised)
  float paintVelocity;
  float centerHue;
  float domHue;           // captured hue when twist starts
  float domFade;          // 0=off, 2=full
  float domWidth;         // half-width of dominant zone (pixels)
  float shiftAccum;       // fractional band-shift accumulator
  bool  ready;
} pnt;                    // zero-init by C++ default for globals

// Compression state: driven by swing, squashes hue bands toward tip
float pntCompressPos = 0.0f;  // 0=normal  1=all bands piled at tip
float pntCompressVel = 0.0f;

// ─── BOUNCING BALL STATE ──────────────────────────────────────────────
#define NUM_BALLS 4
struct BallState {
  float   pos;    // blade position: 0=hilt  BLADE_HALF-1=tip
  float   vel;    // velocity (blade units/frame)
  uint8_t hue;    // colour hue (0=white normally, colourful after collision)
  uint8_t sat;    // saturation: 0=white → 255=full colour
  uint8_t width;  // gaussian radius (pixels)
  float   flash;  // explosion flash intensity 0–1 (decays each frame)
};
void spawnExplosion(BallState& a, BallState& b);
BallState balls[NUM_BALLS];
bool      ballsReady  = false;
float     whirlPhase  = 0.0f;   // accumulates twist → whirlwind hue wave

float     stabImpulse = 0.0f;   // one-shot stab push magnitude
float     pullImpulse = 0.0f;   // one-shot pull push magnitude
float     prevAZ      = 0.0f;   // previous accelZ sample for derivative

// ─── IMPACT WAVES ─────────────────────────────────────────────────────
#define MAX_IMPACTS 4
struct ImpactWave {
  float pos;
  float vel;
  float intensity;  // fades to 0
  bool active;
};
ImpactWave impacts[MAX_IMPACTS];
int impactNext = 0;

void spawnImpact(float startPos, float vel, float intensity) {
  impacts[impactNext].pos = startPos;
  impacts[impactNext].vel = vel;
  impacts[impactNext].intensity = intensity;
  impacts[impactNext].active = true;
  impactNext = (impactNext + 1) % MAX_IMPACTS;
}

void updateAndRenderImpacts() {
  for (int i = 0; i < MAX_IMPACTS; i++) {
    if (!impacts[i].active) continue;
    impacts[i].pos += impacts[i].vel;
    impacts[i].intensity *= 0.88f; 
    if (impacts[i].intensity < 0.05f || impacts[i].pos < -20.0f || impacts[i].pos > BLADE_LENGTH + 20.0f) {
      impacts[i].active = false;
      continue;
    }
    int p = (int)impacts[i].pos;
    float frac = impacts[i].pos - p;
    float intensity = constrain(impacts[i].intensity, 0.0f, 1.0f);
    for (int d = -4; d <= 4; d++) {
      int pos = p + d;
      if (pos >= 0 && pos < BLADE_LENGTH) {
        float dist = fabsf((float)d + frac);
        float falloff = expf(-0.3f * dist * dist);
        uint8_t bri = (uint8_t)(intensity * falloff * 255.0f);
        if (bri > 0) {
          uint8_t sat = (uint8_t)(255.0f * (1.0f - falloff));
          bladeSet(pos, bladeGet(pos) + CHSV(130, sat, bri)); 
        }
      }
    }
  }
}

// ─── ESP-NOW SYNC STATE ────────────────────────────────────────────────
bool      syncEnabled    = false;   // true = paired with another sword
bool      syncSearching  = false;   // true during 60-second discovery window
uint32_t  syncSearchStart = 0;
uint32_t  lastPingMs     = 0;
bool      syncAnimating  = false;   // true during 2-second arm animation
uint32_t  syncAnimStart  = 0;
volatile bool    syncGotPacket = false;
volatile uint8_t syncInType    = 0;
volatile uint8_t syncInMode    = 0;
struct SyncPacket { uint8_t type; uint8_t mode; };
uint8_t broadcastAddr[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ─── MIC ESP-NOW AUDIO (received from circle/mic ESP) ─────────────────
// Struct must match the sender's struct_message exactly.
typedef struct { float bass, mid, treble, totalVolume; int currentMode; } MicPacket;
float    fswBass    = 0.0f;
float    fswMid     = 0.0f;
float    fswTreb    = 0.0f;
float    fswVol     = 0.0f;
bool     fswBeat    = false;
float    fswBeatI   = 0.0f;    // 0–1 beat intensity
uint32_t fswLastPkt = 0;       // millis() of last mic packet (for stale detection)


// =====================================================================
// BLADE HELPERS
// =====================================================================

// Write colour to physical blade position pos (0=hilt, BLADE_LENGTH-1=tip)
inline void bladeSet(int pos, CRGB color) {
  if (pos < 0 || pos >= BLADE_LENGTH) return;
  leds[4 + pos] = color;
}

// Read colour from physical blade position pos (0=hilt, BLADE_LENGTH-1=tip)
inline CRGB bladeGet(int pos) {
  if (pos < 0 || pos >= BLADE_LENGTH) return CRGB::Black;
  return leds[4 + pos];
}

void bladeClear() {
  fill_solid(leds + 4, NUM_LEDS - 4, CRGB::Black);
}

void renderAccents() {
  // Slowly pulse the 4 accent LEDs at the base of the strip
  float pulse = (sinf((float)millis() * 0.002f) + 1.0f) * 0.5f;
  uint8_t bri = (uint8_t)(pulse * 150.0f + 50.0f);
  CRGB accentColor = CHSV(baseHue + 20, 200, bri); // Slightly shifted hue
  
  leds[0] = accentColor;
  leds[1] = accentColor;
  leds[2] = accentColor;
  leds[3] = accentColor;
}


// =====================================================================
// IMU (MPU6050 / MPU9250 raw I2C)
// =====================================================================

static void imuWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(IMU_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

static int16_t imuRead16(uint8_t reg) {
  Wire.beginTransmission(IMU_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom(IMU_ADDR, (uint8_t)2);
  return (int16_t)((Wire.read() << 8) | Wire.read());
}

void setupIMU() {
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);   // 100 kHz
  delay(150);              

  // ── Step 1: I2C presence check — does 0x68 ACK at all? ───────────
  Wire.beginTransmission(0x68);
  uint8_t err = Wire.endTransmission();
  if (err != 0) {
    Serial.printf("MPU9250: no ACK at 0x68 (I2C error %u) — check SDA/SCL wiring\n", err);
    Wire.beginTransmission(0x69);
    if (Wire.endTransmission() == 0)
      Serial.println("  → found something at 0x69 — is AD0 pin HIGH? Change IMU_ADDR to 0x69");
    imuOk = false;
    return;
  }

  // ── Step 2: Read WHO_AM_I — MPU9250 returns 0x71 or 0x73 (or 0x70 for MPU6500) ─────────────
  Wire.beginTransmission(0x68);
  Wire.write(0x75);
  err = Wire.endTransmission(false);
  if (err != 0) {
    Serial.printf("MPU9250: WHO_AM_I write failed (I2C error %u)\n", err);
    imuOk = false;
    return;
  }
  Wire.requestFrom((uint8_t)0x68, (uint8_t)1);
  uint8_t id = Wire.available() ? Wire.read() : 0xFF;
  if (id != 0x71 && id != 0x73 && id != 0x70 && id != 0x68) {
    Serial.printf("MPU9250: WHO_AM_I = 0x%02X, expected 0x71/0x73 (MPU9250)\n", id);
    imuOk = false;
    return;
  }

  // ── Step 3: Wake from sleep (PWR_MGMT_1 = 0x00) ──────────────────
  Wire.beginTransmission(0x68);
  Wire.write(0x6B);
  Wire.write(0x00);
  Wire.endTransmission();
  delay(50);   // let sensor stabilise after wake

  // ── Step 4: Configure ranges ──────────────────────────────────────
  Wire.beginTransmission(0x68);
  Wire.write(0x1B);
  Wire.write(0x08);   // Gyro ±500 dps
  Wire.endTransmission();

  Wire.beginTransmission(0x68);
  Wire.write(0x1C);
  Wire.write(0x00);   // Accel ±2 g
  Wire.endTransmission();

  imuOk = true;
  Serial.println("MPU9250: found and configured at 0x68");
}

void updateIMU() {
  if (!imuOk) return;

  // Accel registers 0x3B–0x40 (AX, AY, AZ)
  accelX = imuRead16(0x3B) / 16384.0f;
  accelY = imuRead16(0x3D) / 16384.0f;
  accelZ = imuRead16(0x3F) / 16384.0f;     // ±2g scale → g (global, used by painter)

  // Gyro registers 0x43–0x48 (GX, GY, GZ)
  float gx = imuRead16(0x43) / 65.5f;      // ±500dps scale → deg/s
  float gy = imuRead16(0x45) / 65.5f;
  float gz = imuRead16(0x47) / 65.5f;

  // Swing = rotation perpendicular to blade axis (gx, gy)
  float swing = sqrtf(gx*gx + gy*gy);
  swingMag   = swingMag   * 0.8f + swing * 0.2f;
  twistRate  = twistRate  * 0.8f + gz    * 0.2f;

  // Tilt direction with hysteresis — change only beyond ±0.2g deadband
  // Negate accelZ here if chase direction feels backwards on your hardware.
  if      (accelZ >  0.2f) tiltDir =  1;
  else if (accelZ < -0.2f) tiltDir = -1;

  // Stab detection: sharp positive derivative of accelZ (thrust tip-ward)
  // Pull detection: sharp negative derivative of accelZ (yank hilt-ward)
  float dAZ   = accelZ - prevAZ;
  stabImpulse = fmaxf(0.0f, dAZ - 0.30f);
  pullImpulse = fmaxf(0.0f, -dAZ - 0.30f);
  prevAZ      = accelZ;
  
  if (stabImpulse > 0.2f) {
    spawnImpact(0.0f, 3.5f + stabImpulse * 1.5f, fminf(1.8f, 0.6f + stabImpulse * 2.5f));
  }
  if (pullImpulse > 0.2f) {
    spawnImpact((float)(BLADE_LENGTH - 1), -(3.5f + pullImpulse * 1.5f), fminf(1.8f, 0.6f + pullImpulse * 2.5f));
  }
}


// =====================================================================
// EFFECT 0 — TEST IMU
// Calibration and Verification Mode.
// 
// CALIBRATION NOTES (IMU parallel to blade, Y-axis is longitudinal):
// - Sword Pointing UP: Y-axis is negative (<-0.4G). Pulse of red/white fire.
// - Sword Pointing DOWN: Y-axis is positive (>0.4G). White Chaser to tip.
// - Sword FLAT (Horizontal): Y-axis is near zero (between -0.4 and 0.4).
//   - Includes flat on face, back, or side. Displays Blue Lightning.
// - Quick Twist: GZ (twistRate) spikes. Strobe White.
// - Swing: Transverse acceleration (AX/AZ) spikes during sweep. "Whooosh" UP.
// - Thrust: Longitudinal acceleration (AY) spikes. "Push" BACK/DOWN.
// =====================================================================
void effectTestIMU() {
  const float dt = 0.016f;
  static float fizzle = 0.0f;
  static float whooshPos = -1.0f;
  static float whooshInt = 0.0f;
  static float pushBack = 0.0f;
  static float lastSwing = 0.0f;
  static float hiltFlash = 0.0f;
  static float tipGlow   = 0.0f;  // 0-1: glowing orange-red gradient at tip after fireball
  
  // Always clear hilt zone by default
  for (int i = 0; i < HILT_LEDS; i++) bladeSet(i, CRGB::Black);
  
  // ── Hilt Flash: momentary glow on swing and roll events ──────────────────
  if (swingMag > 200.0f || fabsf(twistRate) > 300.0f) hiltFlash = 1.0f;
  if (hiltFlash > 0.01f) {
    uint8_t bri = (uint8_t)(hiltFlash * 220.0f);
    CRGB hiltColor;
    if (fabsf(twistRate) > 300.0f)  hiltColor = CRGB::White;
    else if (accelY < -0.4f)        hiltColor = CRGB(bri, bri/4, 0); // Fire orange
    else                             hiltColor = CRGB(0, bri/2, bri); // Blue/cyan
    for (int i = 0; i < HILT_LEDS; i++) bladeSet(i, hiltColor);
    hiltFlash *= 0.80f;
  }
  
  // ── 1. Orientation & Twist ──────────────────────────────────────────────
  bool twisting = (fabsf(twistRate) > 600.0f); // Fast wrist snap
  
  if (twisting) {
    // Strobe: blade only, hilt handled by hiltFlash above
    if ((millis() / 30) % 2) fill_solid(leds + 4 + HILT_LEDS, BLADE_PIXELS, CRGB::White);
    else fill_solid(leds + 4 + HILT_LEDS, BLADE_PIXELS, CRGB::Black);
  } else {
    // Orientation logic (Y-axis is longitudinal along the blade)
    if (accelY < -0.4f && !fireLive) {
      // Sword Pointing UP - Fire pulses, same smooth millis model as the blue bolt
      // 4 independent chasers at different speeds, all in fire palette
      fill_solid(leds + 4 + HILT_LEDS, BLADE_PIXELS, CRGB::Black);

      // Shared helper: paint a fire bolt centered at pos with given width and heat
      // heat 0-255: 0=dark red ember, 128=orange, 255=yellow-white hot
      struct FireBolt {
        uint32_t period;   // ms per full cycle
        int      width;    // gradient half-width in pixels
        uint8_t  hotness;  // 0-255: peak brightness / color
      };
      const FireBolt bolts[] = {
        { 700,  28, 255 },   // Fastest, hottest — thin white/yellow burst
        { 1100, 40, 210 },   // Medium speed, orange
        { 1700, 55, 165 },   // Slow, red-orange
        { 2600, 70, 110 },   // Very slow, deep red ember wave
      };
      const int NUM_BOLTS = 4;

      uint32_t ms = millis();

      for (int b = 0; b < NUM_BOLTS; b++) {
        int center = HILT_LEDS + (int)((ms % bolts[b].period) * BLADE_PIXELS / bolts[b].period);
        int width  = bolts[b].width;
        uint8_t hot = bolts[b].hotness;

        for (int j = 0; j < width; j++) {
          int idx = center - j;
          if (idx < HILT_LEDS || idx >= BLADE_LENGTH) continue;

          float t = (float)j / width;  // 0 at leading edge, 1 at tail end
          CRGB color;

          if (j < width / 6) {
            // Leading edge: white-hot to orange (brighter end)
            uint8_t sat = (uint8_t)(t * 6 * 200);
            color = CHSV(30, sat, hot);
          } else {
            // Tail: orange fading through red to dark ember
            float tailT = (float)(j - width/6) / (width - width/6);
            uint8_t hue  = (uint8_t)(tailT * 8);          // Orange→Red
            uint8_t bri  = (uint8_t)((1.0f - tailT) * hot);
            uint8_t sat  = 220 + (uint8_t)(tailT * 35);
            color = CHSV(hue, sat, bri);
          }

          bladeSet(idx, bladeGet(idx) + color);
        }
      }

      // Occasional random sparks for liveliness
      if (random8() < 15) bladeSet(HILT_LEDS + random16(BLADE_PIXELS), CHSV(20, 200, random8(120, 255)));


    } else if (accelY > 0.4f) {
      // Sword Pointing DOWN - White chaser on blade only
      fill_solid(leds + 4 + HILT_LEDS, BLADE_PIXELS, CRGB::Black);
      int p = ((millis() / 15) % BLADE_PIXELS) + HILT_LEDS;
      for(int j=0; j<15; j++) {
         int idx = p - j;
         if (idx >= HILT_LEDS && idx < BLADE_LENGTH) bladeSet(idx, CHSV(0, 0, 255 - j * 16));
      }
    } else {
      // Sword FLAT/HORIZONTAL - Blue lightning chaser on blade only
      fill_solid(leds + 4 + HILT_LEDS, BLADE_PIXELS, CRGB::Black);
      int p = ((millis() / 8) % BLADE_PIXELS) + HILT_LEDS;
      for(int j=0; j<20; j++) {
         int idx = p - j;
         if (idx >= HILT_LEDS && idx < BLADE_LENGTH) {
            if (j < 3) bladeSet(idx, CRGB::White);
            else if (j < 8) bladeSet(idx, CHSV(160, 150, 255 - j*10));
            else bladeSet(idx, CHSV(160, 255, 200 - j*8));
         }
      }
      if (random8() < 20) bladeSet(HILT_LEDS + random16(BLADE_PIXELS), CRGB(100, 200, 255));
    }
  }

  // ── 2. Fireball Rush (chaser-style smooth motion + fire heatmap) ──────────
  static uint32_t fireStartMs  = 0;
  static float    fireSpeed    = 0.0f;
  static bool     fireLive     = false;
  static float    prevSwingMag = 0.0f;

  // PREDICTIVE TRIGGER: fire early when swing is accelerating, not at peak
  // dSwing = rate of change of swingMag per frame
  float dSwing = swingMag - prevSwingMag;
  prevSwingMag = swingMag;

  // Trigger if:
  //   a) Swing has passed moderate threshold AND is still rising fast (early prediction)
  //   b) OR swing is already very strong (safety net for fast swings)
  bool swingPredict = (swingMag > 30.0f && dSwing > 25.0f);  // Rising fast from a low base
  bool swingStrong  = (swingMag > 80.0f);                     // Safety net
  
  if ((swingPredict || swingStrong) && !fireLive) {
    fireLive    = true;
    fireStartMs = millis();
    // Speed scaled to the predicted final swing magnitude (dSwing as proxy)
    fireSpeed   = 0.010f + (swingMag + dSwing * 2.0f) * 0.00012f;
    if (fireSpeed > 0.020f) fireSpeed = 0.020f;
  }

  if (fireLive) {
    float elapsed = (float)(millis() - fireStartMs);
    float center  = (float)HILT_LEDS + elapsed * fireSpeed * 60.0f;

    if (center > BLADE_LENGTH + 25) {
      fireLive = false;
      tipGlow  = 1.0f;  // Fireball "lands" at tip — trigger the tip glow
    } else {
      for (int i = HILT_LEDS; i < BLADE_LENGTH; i++) {
        float d = center - (float)i;
        CRGB color = CRGB::Black;

        if (d >= -8.0f && d <= 0.0f) {
          float t = (-d) / 8.0f;
          uint8_t sat = (uint8_t)(t * 200);
          color = CHSV(30, sat, 255);
        } else if (d > 0.0f && d < 50.0f) {
          float t = d / 50.0f;
          uint8_t hue = (uint8_t)(t * 6);
          uint8_t bri = (uint8_t)((1.0f - t) * 255.0f);
          uint8_t sat = 200 + (uint8_t)(t * 55);
          color = CHSV(hue, sat, bri);
          if (t > 0.6f && random8() < (uint8_t)((1.0f - t) * 90.0f))
            color += CRGB(random8(60), random8(20), 0);
        }
        if (color) bladeSet(i, bladeGet(i) + color);
      }
    }
  }

  // ── Tip Glow: orange→red gradient on upper half blade after fireball lands ─
  if (tipGlow > 0.01f) {
    // Cover the upper half of the blade (tip end)
    int glowStart = HILT_LEDS + BLADE_PIXELS / 2;  // Midpoint to tip
    for (int i = glowStart; i < BLADE_LENGTH; i++) {
      // t=0 at glow start (orange), t=1 at tip (red)
      float t = (float)(i - glowStart) / (float)(BLADE_LENGTH - glowStart);
      uint8_t hue = (uint8_t)((1.0f - t) * 18.0f);  // Orange(18) → Red(0)
      uint8_t bri = (uint8_t)(tipGlow * (200.0f + t * 55.0f));
      uint8_t sat = 230 + (uint8_t)(t * 25);
      bladeSet(i, bladeGet(i) + CHSV(hue, sat, bri));
    }
    // Occasional sparks in the glow zone as it fades
    if (tipGlow < 0.6f && random8() < (uint8_t)(tipGlow * 50.0f))
      bladeSet(glowStart + random16(BLADE_PIXELS/2), CHSV(10, 220, random8(80,180)));
    tipGlow *= 0.975f;  // Slow, smooth fade
  }





  // ── 3. Thrust → same Fireball as Swing ─────────────────────────────────
  // Detect a sharp Y-axis acceleration spike (thrust/pull)
  static float prevAY = 0.0f;
  float dAY = accelY - prevAY;
  prevAY = accelY;
  if (fabsf(dAY) > 0.5f && swingMag < 80.0f && !fireLive) {
    fireLive    = true;
    fireStartMs = millis();
    fireSpeed   = 0.018f;  // Slightly slower for a heavier thrust feel
  }


  // ── 4. Quick Stop & Fade ───────────────────────────────────────────────
  if (lastSwing > 180.0f && swingMag < 80.0f) fizzle = 1.0f;
  if (fizzle > 0.01f) {
      for (int i = 0; i < BLADE_LENGTH; i++) {
        if (random8() < 50 * fizzle) { CRGB c = bladeGet(i); c.nscale8(180); bladeSet(i, c); }
      }
      fizzle *= 0.92f;
  }
  lastSwing = swingMag;

  // ── 5. Serial Readout for Calibration ───────────────────────────────────
  static uint32_t lastPrint = 0;
  if (millis() - lastPrint > 500) {
    Serial.printf("TEST IMU | aX:%.2f aY:%.2f aZ:%.2f | swing:%.1f twist:%.1f\n", 
                  accelX, accelY, accelZ, swingMag, twistRate);
    lastPrint = millis();
  }
}

// =====================================================================
// EFFECT 1 — CHASER
// A glowing blob races up or down the blade.
// • Tilt drives direction (tiltDir)
// • Swing boosts speed  (swingMag)
// • Twist shifts hue    (twistRate)
// =====================================================================
void effectChaser() {}


// =====================================================================
// EFFECT 1 — PULSE WAVE
// A sine wave travels the full blade length.
// • Tilt reverses travel direction
// • Swing speeds up the wave
// • Twist shifts hue
// =====================================================================
void effectPulse() {}


// =====================================================================
// EFFECT 2 — RIPPLE
// Swings launch bright pulses that travel along the blade.
// • Tilt sets which way the ripples travel
// • Swing magnitude determines ripple energy (speed + brightness)
// • Twist shifts hue
// =====================================================================

#define MAX_RIPPLES 4
static float   ripplePos[MAX_RIPPLES];
static float   rippleSpd[MAX_RIPPLES];
static uint8_t rippleHue[MAX_RIPPLES];
static int     rippleNext = 0;
static float   lastSwingForRipple = 0.0f;

void effectRipple() {}


// =====================================================================
// EFFECT 3 — SWORD PAINTER
// Port of C3Staff RainbowPainterEffect, adapted for a 43-LED mirrored blade.
//
// • accelZ (tilt along blade) drives the paint-blob position
// • twistRate (gz) shifts hue bands sideways and grows a dominant-colour zone
// • Fast motion generates paint-drop splatter
// • bladeSet() ensures both sides stay in sync
// =====================================================================

void effectPainter() {
  const float dt = 0.016f;

  if (!pnt.ready) {
    for (int i = 0; i < BLADE_LENGTH; i++)
      pnt.buf[i] = (float)(i * 256) / BLADE_LENGTH;
    pnt.paintCenter   = 0.5f;
    pnt.paintVelocity = 0.0f;
    pnt.centerHue     = (float)baseHue;
    pnt.domFade       = 0.0f;
    pnt.domWidth      = 0.0f;
    pnt.shiftAccum    = 0.0f;
    pnt.ready         = true;
  }

  // ── Move paint-blob along blade via tilt (accelZ) ────────────────────
  if (imuOk) {
    float ty = constrain(accelZ * 0.4f, -1.0f, 1.0f);
    pnt.paintVelocity = pnt.paintVelocity * 0.85f + ty * dt * 6.0f;
    pnt.paintCenter  += pnt.paintVelocity * dt;
    if (pnt.paintCenter < 0.05f) { pnt.paintCenter = 0.05f; pnt.paintVelocity = -pnt.paintVelocity * 0.7f; }
    if (pnt.paintCenter > 0.95f) { pnt.paintCenter = 0.95f; pnt.paintVelocity = -pnt.paintVelocity * 0.7f; }
  } else {
    static float dp = 0.0f;
    dp += dt * 0.5f;
    pnt.paintCenter   = 0.5f + sinf(dp) * 0.25f;
    pnt.paintVelocity = cosf(dp) * 0.1f;
  }

  // ── Hue drift ─────────────────────────────────────────────────────────
  pnt.centerHue += dt * 80.0f;
  if (pnt.centerHue >= 256.0f) pnt.centerHue -= 256.0f;

  int ci = constrain((int)(pnt.paintCenter * BLADE_LENGTH), 0, BLADE_LENGTH - 1);
  int br = max(3, (int)((0.12f + fabsf(pnt.paintVelocity) * 0.15f) * BLADE_LENGTH));

  // ── Twist → shift hue bands sideways + capture dominant zone ─────────
  bool twisting = imuOk && fabsf(twistRate) > 30.0f;
  if (twisting) {
    if (pnt.domFade <= 0.0f) pnt.domHue = pnt.buf[ci];
    pnt.domFade    = 2.0f;
    pnt.domWidth   = constrain(pnt.domWidth + dt * fabsf(twistRate) * 0.05f,
                               0.0f, (float)(BLADE_LENGTH / 2 - 2));
    pnt.shiftAccum += twistRate * dt * 0.12f;
  } else {
    pnt.domFade    = constrain(pnt.domFade  - dt,      0.0f, 2.0f);
    pnt.domWidth  *= (1.0f - dt * 0.4f);
    pnt.shiftAccum *= (1.0f - dt * 3.0f);
  }
  while (pnt.shiftAccum >= 1.0f) {
    float sv = pnt.buf[BLADE_LENGTH - 1];
    for (int i = BLADE_LENGTH - 1; i > 0; i--) pnt.buf[i] = pnt.buf[i - 1];
    pnt.buf[0] = sv;
    pnt.shiftAccum -= 1.0f;
  }
  while (pnt.shiftAccum <= -1.0f) {
    float sv = pnt.buf[0];
    for (int i = 0; i < BLADE_LENGTH - 1; i++) pnt.buf[i] = pnt.buf[i + 1];
    pnt.buf[BLADE_LENGTH - 1] = sv;
    pnt.shiftAccum += 1.0f;
  }

  // ── Paint: blend pixels near brush toward centerHue ───────────────────
  for (int i = 0; i < BLADE_LENGTH; i++) {
    int d = abs(i - ci);
    if (d > br) continue;
    float ms = 1.0f - (float)d / (br + 1); ms = ms * ms * ms;
    float hd = pnt.centerHue - pnt.buf[i];
    if (hd >  128.0f) hd -= 256.0f;
    if (hd < -128.0f) hd += 256.0f;
    pnt.buf[i] += hd * ms * dt * (10.0f + fabsf(pnt.paintVelocity) * 15.0f);
    while (pnt.buf[i] >= 256.0f) pnt.buf[i] -= 256.0f;
    while (pnt.buf[i] <    0.0f) pnt.buf[i] += 256.0f;
  }

  // ── Diffusion: gentle hue spread to neighbours ────────────────────────
  static float tb[BLADE_LENGTH];
  memcpy(tb, pnt.buf, sizeof(tb));
  float diffR = 0.03f * dt;
  for (int i = 1; i < BLADE_LENGTH - 1; i++) {
    float ld = tb[i-1] - tb[i], rd = tb[i+1] - tb[i];
    if (ld >  128.0f) ld -= 256.0f; if (ld < -128.0f) ld += 256.0f;
    if (rd >  128.0f) rd -= 256.0f; if (rd < -128.0f) rd += 256.0f;
    pnt.buf[i] += (ld + rd) * diffR;
    while (pnt.buf[i] >= 256.0f) pnt.buf[i] -= 256.0f;
    while (pnt.buf[i] <    0.0f) pnt.buf[i] += 256.0f;
  }

  // ── Dominant zone: pull buffer toward snapped hue ─────────────────────
  if (pnt.domFade > 0.0f && pnt.domWidth > 0.5f) {
    float str = constrain(pnt.domFade / 2.0f, 0.0f, 1.0f);
    int dw    = (int)(pnt.domWidth + 0.5f);
    for (int i = max(0, ci - dw); i <= min(BLADE_LENGTH - 1, ci + dw); i++) {
      float dist = fabsf((float)(i - ci)) / (float)(dw + 1);
      float inf  = (1.0f - dist * dist) * str * 0.5f;
      float hd   = pnt.domHue - pnt.buf[i];
      if (hd >  128.0f) hd -= 256.0f;
      if (hd < -128.0f) hd += 256.0f;
      pnt.buf[i] += hd * inf;
      while (pnt.buf[i] >= 256.0f) pnt.buf[i] -= 256.0f;
      while (pnt.buf[i] <    0.0f) pnt.buf[i] += 256.0f;
    }
  }

  // ── Compression physics ────────────────────────────────────────────────
  // Hard swing squashes hue bands toward tip using a power-curve remap.
  // Deadband at 40 dps so casual handling doesn't fire; hard slashes slam it.
  // pnt.buf[] stays untouched — compression only changes how it is sampled.
  {
    float excess = fmaxf(0.0f, swingMag - 40.0f);
    float push   = excess * (boostMode ? 0.005f : 0.0025f);
    push        += stabImpulse * 0.8f;   // stab slams bands to tip
    push        -= pullImpulse * 0.8f;   // pull slams bands to hilt
    float spring = -0.035f * pntCompressPos;
    pntCompressVel = pntCompressVel * 0.88f + push + spring;
    pntCompressPos += pntCompressVel;
    if (pntCompressPos > 1.0f) { pntCompressPos = 1.0f; pntCompressVel = -fabsf(pntCompressVel) * 0.25f; }
    if (pntCompressPos < -1.0f) { pntCompressPos = -1.0f; pntCompressVel =  fabsf(pntCompressVel) * 0.25f; }
  }

  // ── Render to mirrored blade ────────────────────────────────────────────
  // Power-curve remap: compress > 0 crushes bands towards tip; compress < 0 toward hilt.
  uint32_t now = millis();
  float compPower = 1.0f;
  if (pntCompressPos >= 0.0f) {
    compPower = 1.0f + pntCompressPos * 2.0f + (boostMode ? 0.6f : 0.0f);
  } else {
    // pntCompressPos is negative. 
    compPower = 1.0f / (1.0f - pntCompressPos * 2.0f + (boostMode ? 0.6f : 0.0f));
  }

  for (int i = 0; i < BLADE_LENGTH; i++) {
    // Compressed sample index via power curve
    float t    = (float)i / (float)(BLADE_LENGTH - 1);
    float fidx = (i == 0) ? 0.0f : powf(t, compPower) * (float)(BLADE_LENGTH - 1);
    int   lo   = (int)fidx;
    int   hix  = min(lo + 1, BLADE_LENGTH - 1);
    float ifrc = fidx - lo;

    // Shortest-path hue interpolation (wraps at 0/256 boundary)
    float h0 = pnt.buf[lo];
    float h1 = pnt.buf[hix];
    float hd = h1 - h0;
    if (hd >  128.0f) hd -= 256.0f;
    if (hd < -128.0f) hd += 256.0f;
    float fhue = h0 + hd * ifrc;
    while (fhue >= 256.0f) fhue -= 256.0f;
    while (fhue <    0.0f) fhue += 256.0f;
    uint8_t ph = (uint8_t)fhue;

    uint8_t s = 255;
    uint8_t v = (uint8_t)(120.0f * (sinf(i * 0.2f + now * 0.0008f) * 0.3f + 0.7f));

    // Compression brightens blade and slightly bleaches colour (neon crushed look)
    float c2 = pntCompressPos * pntCompressPos;
    v = (uint8_t)constrain((int)v + (int)(c2 * 110.0f), 0, 255);
    s = (uint8_t)constrain((int)s - (int)(fabsf(pntCompressPos) * 75.0f), 155, 255);

    // Paint brush glow
    int d = abs(i - ci);
    if (d <= br) {
      float bhi = 1.0f - (float)d / (br + 1); bhi *= bhi;
      v = (uint8_t)constrain((int)(v + bhi * 120), 0, 255);
      if (fabsf(pnt.paintVelocity) > 0.15f) {
        if (sinf(now * 0.012f + i * 0.4f) > 0.5f) { v = 255; s = (uint8_t)constrain((int)s - 80, 120, 255); }
      }
      if (d < 2) { v = 255; s = (uint8_t)constrain((int)s - 50, 150, 255); }
    }
    if (pnt.domFade > 0.0f) {
      int dw = (int)(pnt.domWidth + 0.5f);
      if (d <= dw) {
        float dist  = (float)d / (float)(dw + 1);
        float boost = (1.0f - dist * dist) * constrain(pnt.domFade / 2.0f, 0.0f, 1.0f);
        v = (uint8_t)constrain((int)(v + boost * 55.0f), 0, 255);
      }
    }
    bladeSet(i, CHSV(ph, s, v));
  }

  // ── Paint-drop splatter when moving fast ──────────────────────────────
  if (fabsf(pnt.paintVelocity) > 0.2f && random8() < 70) {
    int dd = (pnt.paintVelocity > 0) ? -1 : 1;
    int di = ci + dd * (br + (int)random(3));
    if (di >= 0 && di < BLADE_LENGTH) {
      float dh = pnt.centerHue + (float)random(-20, 21);
      while (dh >= 256.0f) dh -= 256.0f;
      while (dh <    0.0f) dh += 256.0f;
      CRGB drp = CHSV((uint8_t)dh, 255, 200);
      bladeSet(di, blend(bladeGet(di), drp, 180));
      if (di > 0 && di < BLADE_LENGTH - 1) {
        bladeSet(di - 1, blend(bladeGet(di - 1), drp, 120));
        bladeSet(di + 1, blend(bladeGet(di + 1), drp, 120));
      }
    }
  }
}


// =====================================================================
// BOUNCING BALL MODE (topMode == 2)
// White balls physics-driven along the blade:
//  • Gravity from tilt (accelZ) — balls fall toward whichever end is down
//  • Swing impulse rushes balls toward tip (like g-force blob)
//  • Stab (sharp accelZ spike) slams all balls to tip
//  • Ball–ball elastic collisions with colorful explosion flash
//  • Twist → whirlwind sine-wave of hue across the background
//  • Boost: faster balls, bigger explosions, scattered sparks
// =====================================================================

void spawnExplosion(BallState& a, BallState& b) {
  uint8_t eh = random8();
  a.hue = eh;
  b.hue = eh + 128;
  a.sat = 255;  b.sat = 255;
  a.flash = 1.0f; b.flash = 1.0f;
  a.width = 5;    b.width = 5;
}

void initBalls() {
  for (int b = 0; b < NUM_BALLS; b++) {
    balls[b].pos   = (float)(b + 1) * ((float)BLADE_LENGTH / (NUM_BALLS + 1.0f));
    balls[b].vel   = 0.0f;
    balls[b].hue   = 0;
    balls[b].sat   = 0;
    balls[b].width = 3;  // Slightly wider for 110-led strip
    balls[b].flash = 0.0f;
  }
  whirlPhase = 0.0f;
  ballsReady = true;
}

void effectPingPong() {
  if (!ballsReady) initBalls();

  // ── Whirlwind: accumulate twist → animated hue wave ──────────────────
  float twistAbs = fabsf(twistRate);
  float whirlStr = constrain((twistAbs - 20.0f) / 150.0f, 0.0f, 1.0f);
  whirlPhase += twistRate * 0.016f * 0.15f;
  if (whirlPhase >  TWO_PI) whirlPhase -= TWO_PI;
  if (whirlPhase < -TWO_PI) whirlPhase += TWO_PI;

  // ── Background: constant red and blue animation ───────────────────────
  uint32_t ms = millis();
  for (int i = 0; i < BLADE_LENGTH; i++) {
    float wave = (sinf((float)i * 0.15f + (float)ms * 0.003f) + 1.0f) * 0.5f;
    // wave = 0 -> Red, wave = 1 -> Blue. Peak brightness 150.
    uint8_t r = (uint8_t)((1.0f - wave) * 150.0f);
    uint8_t b = (uint8_t)(wave * 150.0f);
    bladeSet(i, CRGB(r, 0, b));
  }

  // ── Physics forces ────────────────────────────────────────────────────
  // Gravity: tip-up (accelZ>0) → balls fall toward hilt (vel decreases)
  // Scaled up for 110-LED blade so balls drop faster
  float gravity   = -accelZ * 1.2f * (boostMode ? 1.5f : 1.0f);

  // Swing impulse: sustained swing above 30 dps rushes balls toward tip
  static float lastSwingBall = 0.0f;
  float swingKick = fmaxf(0.0f, swingMag - 30.0f) * 0.025f * (boostMode ? 1.8f : 1.0f);
  lastSwingBall   = swingMag * 0.95f;

  // Stab: sharp thrust rushes all balls hard toward tip
  float stabKick  = stabImpulse * 20.0f * (boostMode ? 1.5f : 1.0f);
  // Pull: sharp pullback rushes all balls hard toward hilt
  float pullKick  = pullImpulse * 20.0f * (boostMode ? 1.5f : 1.0f);

  // ── Update each ball ──────────────────────────────────────────────────
  // Increased max velocity to traverse the longer 110-LED strip snappily
  float vmax = boostMode ? 16.0f : 10.0f;
  for (int b = 0; b < NUM_BALLS; b++) {
    balls[b].vel += gravity + swingKick + stabKick - pullKick;
    balls[b].vel  = constrain(balls[b].vel, -vmax, vmax);
    balls[b].pos += balls[b].vel;

    // Wall bounce at tip (much less bouncy at rest)
    if (balls[b].pos >= (float)(BLADE_LENGTH - 1)) {
      balls[b].pos = (float)(BLADE_LENGTH - 1);
      balls[b].vel = -fabsf(balls[b].vel) * 0.35f; // Was 0.70f - half the bounciness
      if (boostMode) { balls[b].flash = fmaxf(balls[b].flash, 0.5f); balls[b].sat = 180; }
    }
    // Wall bounce at hilt (much less bouncy at rest)
    if (balls[b].pos <= 0.0f) {
      balls[b].pos = 0.0f;
      balls[b].vel =  fabsf(balls[b].vel) * 0.35f; // Was 0.70f - half the bounciness
      if (boostMode) { balls[b].flash = fmaxf(balls[b].flash, 0.5f); balls[b].sat = 180; }
    }

    // Impulses cause a bright flash
    if (stabImpulse > 0.3f || pullImpulse > 0.3f) {
      balls[b].flash = fmaxf(balls[b].flash, fmaxf(stabImpulse, pullImpulse));
      balls[b].sat   = (uint8_t)fmaxf(0.0f, (float)balls[b].sat - 100.0f);
    }

    // Decay flash, saturation, and width back toward white/normal
    balls[b].flash *= 0.88f;
    balls[b].sat    = (uint8_t)((float)balls[b].sat * 0.96f);
    // Scale width up slightly for longer blade
    balls[b].width  = (uint8_t)constrain(3 + (int)(balls[b].flash * 5.0f), 3, 8);
  }

  // ── Ball–ball elastic collisions ─────────────────────────────────────
  for (int a = 0; a < NUM_BALLS - 1; a++) {
    for (int b = a + 1; b < NUM_BALLS; b++) {
      float dist = fabsf(balls[a].pos - balls[b].pos);
      if (dist < 4.0f) {
        float va      = balls[a].vel;
        float vb      = balls[b].vel;
        float closing = (balls[a].pos < balls[b].pos) ? (va - vb) : (vb - va);
        if (closing > 0.2f) {
          float bfac    = boostMode ? 0.9f : 0.6f; // Collisions lose energy instead of retaining 100%
          balls[a].vel  = vb * bfac;
          balls[b].vel  = va * bfac;
          // Physically separate so they don't stick
          float sep = (4.0f - dist) * 0.5f;
          if (balls[a].pos < balls[b].pos) { balls[a].pos -= sep; balls[b].pos += sep; }
          else                             { balls[a].pos += sep; balls[b].pos -= sep; }
          spawnExplosion(balls[a], balls[b]);
        }
      }
    }
  }

  // ── Render balls (Gaussian glow, additive) ───────────────────────────
  for (int b = 0; b < NUM_BALLS; b++) {
    float   bpos   = constrain(balls[b].pos, 0.0f, (float)(BLADE_LENGTH - 1));
    int     centre = (int)bpos;
    float   frac   = bpos - centre;
    int     radius = balls[b].width + 2;
    float   sigma  = (float)balls[b].width * 0.7f + 0.1f;

    // Whirlwind tints the balls
    float   waveH  = sinf(bpos * 0.3f + whirlPhase);
    uint8_t bHue   = (uint8_t)((float)balls[b].hue + waveH * whirlStr * 60.0f);
    uint8_t bSat   = (uint8_t)fminf((float)balls[b].sat + whirlStr * (255.0f - balls[b].sat) * 0.7f, 255.0f);
    uint8_t baseBri= (uint8_t)(180.0f + balls[b].flash * 75.0f);

    for (int d = -radius; d <= radius; d++) {
      int pos = centre + d;
      if (pos < 0 || pos >= BLADE_LENGTH) continue;
      float   dist    = fabsf((float)d + frac);
      float   falloff = expf(-0.5f * (dist / sigma) * (dist / sigma));
      uint8_t bri     = (uint8_t)((float)baseBri * falloff);
      if (bri < 4) continue;
      bladeSet(pos, bladeGet(pos) + CHSV(bHue, bSat, bri));
    }

    // Explosion ring expands outward from collision point
    if (balls[b].flash > 0.15f) {
      int ring = (int)(balls[b].flash * 7.0f);
      for (int side = -1; side <= 1; side += 2) {
        int pos = centre + side * ring;
        if (pos >= 0 && pos < BLADE_LENGTH) {
          uint8_t rbri = (uint8_t)(balls[b].flash * 200.0f);
          bladeSet(pos, bladeGet(pos) + CHSV(balls[b].hue + 128, 200, rbri));
        }
      }
    }
  }

  // Boost mode: scatter sparks
  if (boostMode) {
    int sc = constrain(2 + (int)(swingMag * 0.04f), 2, 7);
    for (int s = 0; s < sc; s++) {
      int pos = random(BLADE_LENGTH);
      bladeSet(pos, bladeGet(pos) + CHSV(random8(), random8(80, 220), random8(100, 230)));
    }
  }
}

// =====================================================================
// EFFECT 3 — FIRE STORM
// =====================================================================
void effectFireStorm() {
  const float dt = 0.016f;
  static byte heat[BLADE_LENGTH];
  static uint32_t lu = 0;
  static float fb = 0, fbv = 0;
  static float rPos[5] = {0}; 
  static unsigned long rT[5] = {0}; 
  static bool rA[5] = {false};
  
  uint32_t now = millis();
  if (now - lu < 50) return; 
  lu = now;

  if (imuOk) {
    float ty = constrain(accelZ * 0.5f, -1.0f, 1.0f);
    fbv = fbv * 0.88f + ty * dt * 8.0f; 
    fb += fbv * dt * 2.0f;
    if (fb < 0.1f) { 
      fb = 0.1f; fbv = -fbv * 0.7f; 
      for (int i = 0; i < 5; i++) if (!rA[i]) { rA[i] = true; rPos[i] = 0.0f; rT[i] = now; break; }
    }
    if (fb > 0.9f) { 
      fb = 0.9f; fbv = -fbv * 0.7f; 
      for (int i = 0; i < 5; i++) if (!rA[i]) { rA[i] = true; rPos[i] = 1.0f; rT[i] = now; break; }
    }
  } else { 
    static float sp = 0; sp += dt * 0.5f; fb = 0.5f + sinf(sp) * 0.3f; fbv = cosf(sp) * 0.2f; 
  }
  
  if (random(100) < 3)
    for (int i = 0; i < 5; i++) 
      if (!rA[i]) { rA[i] = true; rPos[i] = 0.0f; rT[i] = now; break; }

  // Cool down every cell a little
  for (int i = 0; i < BLADE_LENGTH; i++) { 
    int cd = random(10, 25); 
    heat[i] = (heat[i] > cd) ? heat[i] - cd : 0; 
  }

  // Heat propagation (drifting upwards from hilt to tip)
  for(int k = BLADE_LENGTH - 1; k >= 4; k--) {
    heat[k] = (byte)((heat[k-1] * 0.5f + heat[k-2] * 0.3f + heat[k] * 0.2f));
  }

  // Active Fire Base (LED 4 to ~BLADE_LENGTH/4)
  // Sparking and intense heat at the bottom
  if (random8() < 160) {
    int sparkPos = random(4, BLADE_LENGTH / 4);
    heat[sparkPos] = constrain(heat[sparkPos] + random(100, 200), 0, 255);
  }
  
  // Keep the very base continuously hot to act as the source
  heat[4] = constrain(heat[4] + random(50, 150), 0, 255);
  heat[5] = constrain(heat[5] + random(50, 150), 0, 255);

  int bi = (int)(fb * BLADE_LENGTH); 
  int bri = 8 + (int)(fabsf(fbv) * 15.0f);
  for(int i = -bri; i <= bri; i++) {
    int idx = bi + i;
    if (idx >= 0 && idx < BLADE_LENGTH) {
      float d = fabsf((float)i) / (float)bri;
      int nh = heat[idx] + (int)((1.0f - d) * 200.0f);
      heat[idx] = (nh > 255) ? 255 : (byte)nh;
    }
  }

  for (int i = 0; i < BLADE_LENGTH; i++) {
    byte t = heat[i]; 
    uint8_t r, g, b;
    if(t > 200) { r = 255; g = 255; b = random(30, 80); }
    else if(t > 150) { r = 255; g = random(80, 120); b = 0; }
    else if(t > 100) { r = 255; g = random(30, 80);  b = 0; }
    else if(t > 50)  { r = 200 + random(55); g = random(20, 40); b = 0; }
    else { r = 100 + random(50); g = 0; b = 0; }
    bladeSet(i, CRGB(r, g, b));
  }

  for (int r = 0; r < 5; r++) {
    if(!rA[r]) continue;
    float rr = (float)(now - rT[r]) / 800.0f;
    if (rr > 1.2f) { rA[r] = false; continue; }
    for (int i = 0; i < BLADE_LENGTH; i++) {
      float pos = (float)i / (float)BLADE_LENGTH;
      float dr2 = fabsf(pos - rr);
      if(dr2 < 0.1f) {
        float ints = (1.0f - dr2 / 0.1f) * (1.0f - rr * 0.8f);
        if (ints > 0) bladeSet(i, bladeGet(i) + CRGB((uint8_t)(ints * 255.0f), (uint8_t)(ints * 100.0f), 0));
      }
    }
  }
}

// =====================================================================
// EFFECT 4 — OCEAN WAVES
// =====================================================================
void effectOceanWaves() {
  const float dt = 0.016f;
  static float wp = 0, ob = 0.5f, obv = 0;
  static float wgP[3] = {0.2f, 0.5f, 0.8f};
  static float wgS[3] = {0.15f, 0.2f, 0.18f};
  static float wgI[3] = {0.8f, 1.0f, 0.9f};
  static unsigned long wgT[3] = {0};
  
  uint32_t now = millis();
  if (imuOk) {
    float ty = constrain(accelZ * 0.4f, -1.0f, 1.0f);
    obv = obv * 0.9f + ty * dt * 6.0f; 
    ob += obv * dt * 2.0f;
    if (ob < 0.1f) { 
      ob = 0.1f; obv = -obv * 0.8f; 
      wgP[0] = 0; wgS[0] = 0.1f; wgI[0] = 1.0f; wgT[0] = now; 
    }
    if (ob > 0.9f) { 
      ob = 0.9f; obv = -obv * 0.8f; 
      wgP[2] = 1; wgS[2] = 0.1f; wgI[2] = 1.0f; wgT[2] = now; 
    }
  } else { 
    static float sp = 0; sp += dt * 0.4f; 
    ob = 0.5f + sinf(sp) * 0.25f; obv = cosf(sp) * 0.15f; 
  }
  
  wp += dt * (2.0f + fabsf(obv) * 3.0f); 
  if (wp > 6.283f) wp -= 6.283f;

  for (int w = 0; w < 3; w++) {
    if(wgI[w] > 0.1f) {
      wgS[w] += dt * 0.3f;
      wgP[w] += dt * 0.4f;
      wgI[w] *= 0.97f;
      if(wgP[w] > 1.2f) wgI[w] = 0;
    }
  }

  if (random(100) < 2) {
    for (int w = 0; w < 3; w++) {
      if (wgI[w] < 0.2f) {
        wgP[w] = -0.2f - (float)random(20) / 100.0f;
        wgS[w] = 0.08f + (float)random(10) / 100.0f;
        wgI[w] = 0.6f + (float)random(40) / 100.0f;
        wgT[w] = now;
        break;
      }
    }
  }

  for (int i = 0; i < BLADE_LENGTH; i++) {
    float pos = (float)i / (float)BLADE_LENGTH;
    uint8_t bh = 140 + (uint8_t)(pos * 30.0f);
    uint8_t bs = 255;
    uint8_t bv = 60 + (uint8_t)(pos * 80.0f);
    
    float wave = sinf(pos * 20.0f + wp) * 0.3f + 0.7f; 
    bv = (uint8_t)((float)bv * wave);
    
    for (int w = 0; w < 3; w++) {
      float dw = fabsf(pos - wgP[w]);
      if(dw < wgS[w] && wgI[w] > 0.1f) {
        float wi = (wgS[w] - dw) / wgS[w]; 
        wi *= wi; wi *= wgI[w];
        bv = (uint8_t)constrain((int)bv + (int)(wi * 180.0f), 0, 255); 
        bs = (uint8_t)constrain((int)bs - (int)(wi * 120.0f), 100, 255); 
        bh = 150 + (uint8_t)(wi * 20.0f);
        if(wi > 0.7f) { bs = 100; bv = 255; }
      }
    }
    
    float db = fabsf(pos - ob);
    if(db < 0.15f) {
      float bi = (0.15f - db) / 0.15f; 
      bi *= bi;
      bv = (uint8_t)constrain((int)bv + (int)(bi * 150.0f), 0, 255);
      bs = (uint8_t)constrain((int)bs - (int)(bi * 100.0f), 150, 255);
      if(fabsf(obv) > 0.3f && bi > 0.7f) { bv = 255; bs = 150; }
    }
    
    bladeSet(i, CHSV(bh, bs, bv));
  }
  
  if (fabsf(obv) > 0.2f && random8() < 50) {
    int rndPos = random(BLADE_LENGTH);
    bladeSet(rndPos, CHSV(180, 200, 220));
  }
}

// =====================================================================
// EFFECT 5 — PLASMA STORM
// =====================================================================
void effectPlasmaStorm() {
  const float dt = 0.016f;
  static float p1 = 0, p2 = 0, p3 = 0;
  
  p1 += dt * 2.5f; if(p1 > 6.283f) p1 -= 6.283f;
  p2 += dt * 1.8f; if(p2 > 6.283f) p2 -= 6.283f;
  p3 += dt * 3.2f; if(p3 > 6.283f) p3 -= 6.283f;
  
  for (int i = 0; i < BLADE_LENGTH; i++) {
    float pos = (float)i / (float)BLADE_LENGTH;
    float c = (sinf(pos * 10.0f + p1) + sinf(pos * 15.0f - p2) + sinf(pos * 8.0f + p3)) / 3.0f;
    bladeSet(i, CHSV(200 + (uint8_t)(c * 40.0f), 255, 120 + (uint8_t)(c * 135.0f)));
  }
  
  if (random8() < 30) {
    int ap = random(BLADE_LENGTH - 5);
    for (int j = 0; j < 3; j++) {
      bladeSet(ap + j, CHSV(180, 200, 255));
    }
  }
}

// ─── ROLLING SPARKLE OVERLAY (Replaces gBlob)
void updateAndRenderRollOverlay() {
  // If the sword is being twisted significantly along its axis (Z)...
  float gyroZ_abs = fabsf(twistRate);
  
  if (gyroZ_abs > 30.0f) {
    rollPhase += (twistRate * 0.005f);
    
    // Create traveling spiraling sparks along the whole blade based on twist
    int numSparks = constrain((int)(gyroZ_abs / 20.0f), 1, 8);
    for (int i = 0; i < numSparks; i++) {
        int target = random(0, BLADE_LENGTH);
        float shift = sinf(rollPhase + (target * 0.1f)) + 1.0f;
        CRGB sparkColor = CHSV(baseHue + (twistRate > 0 ? 30 : -30), 120, (uint8_t)(shift * 127.0f));
        bladeSet(target, bladeGet(target) + sparkColor); 
    }
  }
}


// =====================================================================
// BOOST MODE — spark overlay (called after main effect + gBlob)
// =====================================================================
void renderBoostSparks() {
  int count = constrain(3 + (int)(swingMag * 0.06f), 3, 10);
  for (int s = 0; s < count; s++) {
    int     pos = random(BLADE_LENGTH);
    uint8_t h   = baseHue + (int8_t)(random8() / 4 - 32);  // slight hue scatter
    uint8_t sat = random8(60, 200);                          // white→coloured range
    uint8_t bri = random8(160, 255);
    bladeSet(pos, bladeGet(pos) + CHSV(h, sat, bri));
  }
}


// =====================================================================
// MUSIC MODES — shared helpers
// =====================================================================

// Returns true when a mic packet arrived within the last 3 seconds.
static inline bool musicHasSignal() {
  return fswLastPkt > 0 && (millis() - fswLastPkt) < 3000;
}

// Slow purple/blue breathing wave — shown across the whole blade when
// no ESP-NOW audio signal has been received yet (mic ESP not connected).
static void renderMusicNoSignal() {
  float t = (float)millis() * 0.0015f;
  for (int i = 0; i < BLADE_LENGTH; i++) {
    float pos = (float)i / (BLADE_LENGTH - 1);
    float br  = (sinf(t - pos * 2.5f) + 1.0f) * 0.5f;
    bladeSet(i, bladeGet(i) + CHSV(baseHue + 15, 210, (uint8_t)(br * 55 + 10)));
  }
}

// Shared debug ticker — prints audio values once per 3 s in any music mode.
static void musicDebugPrint(const char* modeName) {
  static uint32_t lastDbg = 0;
  if (millis() - lastDbg < 3000) return;
  lastDbg = millis();
  Serial.printf("[%s] signal:%s bass:%.2f mid:%.2f treb:%.2f vol:%.2f beat:%s\n",
    modeName, musicHasSignal() ? "YES" : "NO",
    fswBass, fswMid, fswTreb, fswVol, fswBeat ? "YES" : "no");
}

// =====================================================================
// MUSIC MODE 5 — PURPLE PULSE
// =====================================================================
void effectMusicPulse() {
  const float dt = 0.016f;
  static float  wR[8];           
  static float  wB[8];           
  static bool   wA[8];           
  static float  breathPhase = 0.0f;
  static float  highSmooth  = 0.0f;
  static uint32_t lastWave  = 0;

  musicDebugPrint("PurplePulse");

  uint32_t now = millis();
  float ty  = imuOk ? constrain(accelZ, -1.0f, 1.0f) : 0.0f;
  static float lastAccelY1 = 0.0f;
  float jerk = imuOk ? constrain((ty - lastAccelY1) * 5.0f, -3.0f, 3.0f) : 0.0f;
  lastAccelY1 = ty;
  int ci = BLADE_LENGTH / 2 + (int)(ty * (BLADE_LENGTH / 2.5f));
  ci = constrain(ci, 4, BLADE_LENGTH - 5);

  float breathSpeed = 3.0f + fswBass * 6.0f + fswBeatI * 8.0f;
  breathPhase += dt * breathSpeed;
  if (breathPhase > 6.283f) breathPhase -= 6.283f;

  static float beatSwell = 0.0f;
  if (fswBeat && fswBeatI > 0.15f) { beatSwell = fswBeatI; }
  beatSwell *= 0.88f;  

  float pulse = (sinf(breathPhase) * 0.5f + 0.5f) * 0.55f + beatSwell * 0.45f;
  pulse = constrain(pulse, 0.0f, 1.0f);

  if (!musicHasSignal()) { renderMusicNoSignal(); return; }

  float coreRadius = (float)ci * (0.30f + pulse * 0.40f);  
  for (int i = 0; i < BLADE_LENGTH; i++) {
    float d = fabsf((float)i - ci);
    float falloff = constrain(1.0f - (d / coreRadius), 0.0f, 1.0f);
    falloff = falloff * falloff;  
    uint8_t hue = (uint8_t)(192 + pulse * 18.0f + fswMid * 12.0f);
    uint8_t sat = 240;
    uint8_t val = max((uint8_t)100, (uint8_t)(falloff * (pulse * 180.0f + 40.0f)));
    bladeSet(i, CHSV(hue, sat, val));
  }

  float highEdge = fswTreb - highSmooth;
  highSmooth = highSmooth * 0.65f + fswTreb * 0.35f;

  uint32_t waveInterval = (uint32_t)(350 - (uint32_t)(fswTreb * 250));
  if ((highEdge > 0.08f && fswTreb > 0.18f) && (now - lastWave) > waveInterval) {
    lastWave = now;
    for (int k = 0; k < 8; k++) {
      if (!wA[k]) { wR[k] = 0.0f; wB[k] = 0.25f + fswTreb * 0.75f; wA[k] = true; break; }
    }
  }
  if (fswBeat && fswBeatI > 0.20f) {
    for (int k = 0; k < 8; k++) {
      if (!wA[k]) { wR[k] = 0.0f; wB[k] = 0.5f + fswBeatI * 0.5f; wA[k] = true; break; }
    }
  }

  float waveSpeed = (55.0f + fswTreb * 55.0f);
  for (int k = 0; k < 8; k++) {
    if (!wA[k]) continue;
    wR[k] += waveSpeed * dt;
    wB[k] -= dt * 0.8f;
    if (wB[k] <= 0.0f || wR[k] >= (float)ci) { wA[k] = false; continue; }

    for (int i = 0; i < ci; i++) {
      float rd = fabsf((float)i - wR[k]);
      if (rd < 5.0f) {
        float ri = (1.0f - rd / 5.0f) * wB[k];
        uint8_t v   = (uint8_t)(ri * 220.0f);
        uint8_t hue = (uint8_t)(195 + wR[k] / (float)ci * 25.0f);  
        if (ci + i < BLADE_LENGTH) bladeSet(ci + i, bladeGet(ci + i) + CHSV(hue, 250, v));
        if (ci - i >= 0)           bladeSet(ci - i, bladeGet(ci - i) + CHSV(hue, 250, v));
      }
    }
  }
}

// =====================================================================
// MUSIC MODE 6 — FREQ COMETS
// =====================================================================
void effectMusicComets() {
  const float dt = 0.016f;
  static float   cp[16], cv[16], cb[16];
  static uint8_t ch[16], cs[16];
  static bool    ca[16];
  static uint32_t lastBassC = 0, lastMidC = 0, lastHighC = 0;
  static float midSmooth = 0.0f, highSmooth = 0.0f;

  musicDebugPrint("FreqComets");

  if (!musicHasSignal()) { renderMusicNoSignal(); return; }

  uint32_t now = millis();

  // No center 'ci'. Base is always 0 (hilt).
  float midEdge  = fswMid  - midSmooth;
  float highEdge = fswTreb - highSmooth;
  midSmooth  = midSmooth  * 0.75f + fswMid  * 0.25f;
  highSmooth = highSmooth * 0.60f + fswTreb * 0.40f;

  // Bass comets on beat (base to tip)
  if ((fswBeat && fswBeatI > 0.20f) && (now - lastBassC) > 120) {
    lastBassC = now;
    for (int k = 0; k < 16; k++) {
      if (!ca[k]) {
        cp[k] = 0.0f; cv[k] = (60.0f + fswBeatI * 110.0f); ch[k] = (uint8_t)random(0, 22);
        cs[k] = (uint8_t)(8 + (int)(fswBeatI * 12)); cb[k] = 0.80f + fswBeatI * 0.20f; ca[k] = true; break;
      }
    }
  }

  // Mid comets
  if (midEdge > 0.14f && fswMid > 0.28f && (now - lastMidC) > 180) {
    lastMidC = now;
    for (int k = 0; k < 16; k++) {
      if (!ca[k]) {
        cp[k] = 0.0f; cv[k] = (80.0f + fswMid * 100.0f); ch[k] = (uint8_t)random(88, 140);
        cs[k] = 3; cb[k] = 0.70f + fswMid * 0.25f; ca[k] = true; break;
      }
    }
  }

  // Treble comets
  if (highEdge > 0.11f && fswTreb > 0.22f && (now - lastHighC) > 100) {
    lastHighC = now;
    for (int k = 0; k < 16; k++) {
      if (!ca[k]) {
        cp[k] = 0.0f; cv[k] = (120.0f + fswTreb * 130.0f); ch[k] = (uint8_t)random(155, 215);
        cs[k] = 1; cb[k] = 0.55f + fswTreb * 0.35f; ca[k] = true; break;
      }
    }
  }

  // Fade background
  for (int i = 0; i < BLADE_LENGTH; i++) {
    CRGB current = bladeGet(i);
    current.nscale8(210);
    // Keep a dim floor using gradient
    if (current.getLuma() < 100) current = blend(current, CHSV((160 + i)%255, 230, 100), 20);
    bladeSet(i, current);
  }

  // Plasma reach from hilt
  float plasmaReach = (fswVol * 0.5f + fswBeatI * 0.7f) * BLADE_LENGTH;
  int pr = (int)plasmaReach;
  for (int i = 0; i < pr; i++) {
    if (i < BLADE_LENGTH) {
      uint8_t pVal = (uint8_t)((1.0f - (float)i / (float)(pr + 1)) * 200);
      bladeSet(i, bladeGet(i) + CHSV((140 + i*3 + now/20)%255, 240, pVal));
    }
  }

  // Bass glow at hilt
  int gr = (int)(6 + fswVol * 12 + fswBeatI * 25);
  for (int i = 0; i <= gr; i++) {
    if (i < BLADE_LENGTH) {
      float gd = 1.0f - (float)i / (float)(gr + 1);
      bladeSet(i, bladeGet(i) + CHSV((uint8_t)(10 + fswBeatI*20), (uint8_t)max(0, (int)(210 - fswBeatI*50)), (uint8_t)(gd * 190 + fswBass * 65)));
    }
  }

  // Draw comets
  for (int k = 0; k < 16; k++) {
    if (!ca[k]) continue;
    cp[k] += cv[k] * dt;
    cb[k] -= dt * 0.95f;
    if (cb[k] <= 0.0f || cp[k] >= (float)BLADE_LENGTH) { ca[k] = false; continue; }

    int head = (int)cp[k];
    for (int t = 0; t <= cs[k] + 4; t++) {
      int idx = head - t;  // trail stretches behind head
      if (idx >= 0 && idx < BLADE_LENGTH) {
        float tf = (1.0f - (float)t / (float)(cs[k] + 5)) * cb[k];
        bladeSet(idx, bladeGet(idx) + CHSV(ch[k], 235, (uint8_t)(tf * 255)));
      }
    }
  }
}

// =====================================================================
// MUSIC MODE 7 — RIPPLE ORCHESTRA
// =====================================================================
void effectMusicRipples() {
  const float dt = 0.016f;
  static float   rr[18], rv[18], rb[18];
  static uint8_t rh[18], rw[18];
  static bool    ra[18];
  static float   expPos[2], expLife[2];
  static uint8_t expHue[2];
  static bool    expActive[2];
  static int     expIdx = 0;

  static uint32_t lastBassR = 0, lastMidR = 0, lastHighR = 0;
  static float midSmooth = 0.0f, highSmooth = 0.0f;

  musicDebugPrint("RippleOrch");

  if (!musicHasSignal()) { renderMusicNoSignal(); return; }

  uint32_t now = millis();

  float midEdge  = fswMid  - midSmooth;
  float highEdge = fswTreb - highSmooth;
  midSmooth  = midSmooth  * 0.80f + fswMid  * 0.20f;
  highSmooth = highSmooth * 0.65f + fswTreb * 0.35f;

  uint32_t bassInterval = (uint32_t)(1200 - (uint32_t)(fswBass * 900));
  if (fswBass > 0.22f && (now - lastBassR) > bassInterval) {
    lastBassR = now;
    for (int k = 0; k < 6; k++) {
      if (!ra[k]) { rr[k]=0; rv[k]=(22.0f+fswBass*50.0f); rb[k]=0.15f+fswBass*0.85f; rh[k]=random(0,28); rw[k]=5+(int)(fswBass*5); ra[k]=true; break; }
    }
  }

  if (midEdge > 0.10f && fswMid > 0.22f && (now - lastMidR) > 240) {
    lastMidR = now;
    for (int k = 6; k < 12; k++) {
      if (!ra[k]) { rr[k]=0; rv[k]=(42.0f+fswMid*58.0f); rb[k]=0.20f+fswMid*0.75f; rh[k]=random(85,148); rw[k]=3+(int)(fswMid*4); ra[k]=true; break; }
    }
  }

  if (highEdge > 0.08f && fswTreb > 0.18f && (now - lastHighR) > 120) {
    lastHighR = now;
    for (int k = 12; k < 18; k++) {
      if (!ra[k]) { rr[k]=0; rv[k]=(72.0f+fswTreb*90.0f); rb[k]=0.15f+fswTreb*0.70f; rh[k]=random(155,225); rw[k]=2; ra[k]=true; break; }
    }
  }

  if (fswBeat && fswBeatI > 0.28f) {
    for(int d=0; d<2; d++) {
      float best_r = 15.0f; int best_k = -1;
      for (int k = 0; k < 18; k++) { if (ra[k] && rr[k] > best_r) { best_r = rr[k]; best_k = k; } }
      if (best_k != -1) {
        expPos[expIdx] = rr[best_k]; expHue[expIdx] = rh[best_k]; expLife[expIdx] = 1.0f;
        expActive[expIdx] = true; expIdx = (expIdx + 1) % 2; ra[best_k] = false;
      }
    }
    for (int k = 0;  k < 6;  k++) { if (!ra[k]) { rr[k]=0; rv[k]=30.0f+fswBeatI*55; rb[k]=fswBeatI;       rh[k]=8;   rw[k]=7; ra[k]=true; break; } }
    for (int k = 6;  k < 12; k++) { if (!ra[k]) { rr[k]=0; rv[k]=52.0f+fswBeatI*60; rb[k]=fswBeatI*0.80f; rh[k]=112; rw[k]=5; ra[k]=true; break; } }
    for (int k = 12; k < 18; k++) { if (!ra[k]) { rr[k]=0; rv[k]=88.0f+fswBeatI*75; rb[k]=fswBeatI*0.60f; rh[k]=188; rw[k]=3; ra[k]=true; break; } }
  }

  for (int i = 0; i < BLADE_LENGTH; i++) {
    bladeSet(i, CHSV((uint8_t)(8 + i/4 + now/100), 200, max(100, (int)(fswBass * 180))));
  }

  for (int k = 0; k < 18; k++) {
    if (!ra[k]) continue;
    rr[k] += rv[k] * dt; rb[k] -= dt * 0.72f;
    if (rb[k] <= 0.0f || rr[k] >= (float)BLADE_LENGTH) { ra[k] = false; continue; }

    for (int i = 0; i < BLADE_LENGTH; i++) {
      float rd = fabsf((float)i - rr[k]);
      if (rd < (float)rw[k]) {
        float ri = (1.0f - rd / (float)rw[k]) * rb[k];
        uint8_t v = (uint8_t)(ri * 255);
        bladeSet(i, bladeGet(i) + CHSV(rh[k], 245, v));
      }
    }
  }

  for (int e = 0; e < 2; e++) {
    if (!expActive[e]) continue;
    expLife[e] -= dt * 1.5f;
    if (expLife[e] <= 0.0f) { expActive[e] = false; continue; }
    float fizzR = expPos[e] + (1.0f - expLife[e]) * 15.0f;
    float fizzWidth = 3.0f + (1.0f - expLife[e]) * 10.0f;
    for (int i = 0; i < BLADE_LENGTH; i++) {
      float rd = fabsf((float)i - fizzR);
      if (rd < fizzWidth) {
        if (random(255) < (int)(expLife[e] * 200.0f + 55)) { 
          float ri = (1.0f - rd / fizzWidth) * expLife[e];
          uint8_t v = (uint8_t)(ri * 255);
          bladeSet(i, bladeGet(i) + CHSV(expHue[e] + random(-25, 25), random(120, 240), v));
        }
      }
    }
  }
}

// =====================================================================
// MUSIC MODE 8 — COLOUR CHASER
// =====================================================================
void effectMusicChaser() {
  const float dt = 0.016f;
  static float ledHue[BLADE_LENGTH];
  static float   wPos[4], wHue[4], wSpd[4];
  static int8_t  wDir[4];
  static bool    wOn[4];
  static float   bgHue  = 160.0f, nextHue = 0.0f;
  static bool    outward  = true;

  musicDebugPrint("ColourChaser");

  if (!musicHasSignal()) { renderMusicNoSignal(); return; }

  bgHue += dt * (6.0f + fswMid * 18.0f);
  if (bgHue > 255.0f) bgHue -= 255.0f;

  for (int i = 0; i < BLADE_LENGTH; i++) {
    float diff = bgHue - ledHue[i];
    if (diff >  128.0f) diff -= 255.0f;
    if (diff < -128.0f) diff += 255.0f;
    ledHue[i] += diff * dt * 1.2f;
    if (ledHue[i] > 255.0f) ledHue[i] -= 255.0f;
    if (ledHue[i] < 0.0f)   ledHue[i] += 255.0f;
  }

  if (fswBeat && fswBeatI > 0.15f) {
    for (int k = 0; k < 4; k++) {
      if (!wOn[k]) {
        wHue[k] = nextHue; nextHue += 55.0f + (float)(int)random(0, 25);
        if (nextHue > 255.0f) nextHue -= 255.0f;
        wSpd[k]  = (45.0f + fswBeatI * 70.0f); wOn[k] = true;
        wPos[k] = 0.0f; wDir[k] = 1; break;
      }
    }
  }

  for (int k = 0; k < 4; k++) {
    if (!wOn[k]) continue;
    wPos[k] += (float)wDir[k] * wSpd[k] * dt;
    if (wPos[k] >= (float)BLADE_LENGTH) { wOn[k] = false; continue; }

    int wp = (int)wPos[k];
    for (int t = -5; t <= 5; t++) {
      int idxA = wp + t;
      if (idxA >= 0 && idxA < BLADE_LENGTH) ledHue[idxA] = wHue[k];
    }
  }

  for (int i = 0; i < BLADE_LENGTH; i++) {
    float d = fabsf((float)i) / (float)BLADE_LENGTH;
    d = constrain(d, 0.0f, 1.0f);
    float centreShape = 1.0f - d * d;  
    centreShape = constrain(centreShape, 0.0f, 1.0f);
    float baseBright = 25.0f + centreShape * 40.0f + fswVol * 30.0f;
    float waveBrightBoost = 0.0f;
    for (int k = 0; k < 4; k++) {
      if (!wOn[k]) continue;
      float closest = fabsf((float)(int)wPos[k] - (float)i);
      if (closest < 8.0f) waveBrightBoost += (1.0f - closest / 8.0f) * (80.0f + fswBeatI * 100.0f);
    }
    float totalBright = constrain(baseBright + waveBrightBoost, 0.0f, 255.0f);
    bladeSet(i, CHSV((uint8_t)ledHue[i], (uint8_t)(210 + centreShape * 45), (uint8_t)totalBright));
  }
}

// =====================================================================
// MUSIC MODE 9 — AUDIO FIRE
// =====================================================================
void effectMusicFire() {
  const float dt = 0.016f;
  static byte heat[BLADE_LENGTH];
  static uint32_t lu = 0;
  musicDebugPrint("AudioFire");

  if (!musicHasSignal()) { renderMusicNoSignal(); return; }

  uint32_t now = millis();

  if (now - lu < 40) return;
  lu = now;

  float coolBase = constrain(9.0f + fswTreb * 11.0f - fswBass * 5.5f, 3.0f, 24.0f);
  for (int i = 0; i < BLADE_LENGTH; i++) {
    int cd = random((int)coolBase, (int)(coolBase + 10));
    heat[i] = (heat[i] > cd) ? heat[i] - cd : 0;
  }

  for (int k = BLADE_LENGTH - 1; k > 1; k--) heat[k] = (byte)((heat[k - 1] + heat[k - 2]) / 2);

  int bassRadius = (int)(3 + fswBass * 15);
  if (random(255) < (int)(200 * (0.05f + fswBass * 0.95f))) {
    int off = random(0, bassRadius + 1);
    if (off < BLADE_LENGTH) heat[off] = (byte)constrain((int)heat[off] + random(150, 242), 0, 255);
  }
  for (int i = 0; i < BLADE_LENGTH; i++) {
    float d = (float)i / (float)BLADE_LENGTH;
    byte minH = max((byte)100, (byte)(fswBass * (120 - d * 80) + 100));
    if (heat[i] < minH) heat[i] = minH;
  }

  static float chargePos = BLADE_LENGTH / 2.0f;
  static float chargeDir = 1.0f;
  float chargeSpeed = 30.0f + fswMid * 100.0f;
  if (fswVol > 0.5f) chargeSpeed += fswVol * 150.0f;
  chargePos += chargeDir * chargeSpeed * dt;
  if(chargePos >= BLADE_LENGTH - 2) { chargePos = BLADE_LENGTH - 2; chargeDir = -1.0f; }
  if(chargePos <= 2) { chargePos = 2; chargeDir = 1.0f; }

  if (fswMid > 0.15f && random(255) < (int)(fswMid * 200 + fswVol * 50)) {
    int bw = (int)(3 + fswMid * 14 + fswVol * 10);
    for(int i = -bw; i <= bw; i++) {
      int pos = (int)chargePos + i;
      if (pos >= 0 && pos < BLADE_LENGTH) {
        float falloff = 1.0f - fabsf((float)i) / (bw + 1.0f);
        heat[pos] = (byte)constrain((int)heat[pos] + (int)(random(50, 150) * falloff), 0, 255);
      }
    }
  }

  if (fswMid > 0.25f && random(255) < (int)(fswMid * 180)) {
    int burstCenter = random(0, BLADE_LENGTH);
    int burstWidth = random(3, 10);
    for(int i = -burstWidth; i <= burstWidth; i++) {
        int pos = burstCenter + i;
        if(pos >= 0 && pos < BLADE_LENGTH) heat[pos] = (byte)constrain((int)heat[pos] + random(40, 130), 0, 255);
    }
  }

  if (fswTreb > 0.18f && random(255) < (int)(fswTreb * 145)) {
    int outerRange = 15;
    int pos = random(outerRange * 2) + 2;
    if (pos >= 0 && pos < BLADE_LENGTH) heat[pos] = (byte)constrain((int)heat[pos] + random(45, 115), 0, 255);
  }

  if (fswBeat && fswBeatI > 0.18f) {
    int burstR = (int)(8 + fswBeatI * 13);
    for (int i = 0; i <= burstR; i++) {
        if (i < BLADE_LENGTH) {
            float fd  = (float)i / (float)(burstR > 0 ? burstR : 1);
            int   add = (int)(fswBeatI * (225.0f - fd * 110.0f));
            heat[i] = (byte)constrain((int)heat[i] + add, 0, 255);
        }
    }
    int ns = (int)(8 + fswBeatI * 18);
    for (int s = 0; s < ns; s++) {
      int pos = random(0, BLADE_LENGTH / 3);
      if (pos >= 0 && pos < BLADE_LENGTH) heat[pos] = (byte)constrain((int)heat[pos] + (int)random(150, 255), 0, 255);
    }
  }

  float blueShift = constrain(fswMid * 1.8f, 0.0f, 1.0f);

  for (int i = 0; i < BLADE_LENGTH; i++) {
    byte t = heat[i]; uint8_t r, g, b;
    if (t > 230) { r = 255; g = 255; b = random(40, 120); }
    else if (t > 170) { r = 255; g = random(150, 210); b = (uint8_t)(blueShift * 70); }
    else if (t > 110) { r = (uint8_t)(255 - blueShift * 100); g = random(60, 140); b = (uint8_t)(blueShift * 225); }
    else if (t > 50)  { r = (uint8_t)max(0, (int)(230 - blueShift * 190)); g = random(15, 45); b = (uint8_t)constrain((int)(blueShift * 255) + (int)t, 0, 255); }
    else { r = (uint8_t)(t * 2.5f); g = 0; b = (uint8_t)constrain((int)(blueShift * t * 2.5f), 0, 255); }

    CRGB color = CRGB(r, g, b);
    bladeSet(i, color);
  }
}

// =====================================================================
// ESP-NOW SYNC — receive callback, send helper, setup
// =====================================================================

// Called from ESP-NOW task context — only set flags / copy data, no heavy work.
void onSyncReceive(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len == (int)sizeof(SyncPacket)) {
    // ── Sword-to-sword sync packet ──
    SyncPacket pkt;
    memcpy(&pkt, data, sizeof(SyncPacket));
    syncInType    = pkt.type;
    syncInMode    = pkt.mode;
    syncGotPacket = true;
  } else if (len >= (int)sizeof(MicPacket)) {
    // ── Mic ESP audio packet ──
    MicPacket msg;
    memcpy(&msg, data, sizeof(MicPacket));
    // Self-normalising peak: rises instantly, drifts back to 1.0 during silence
    static float enMax = 1.0f;
    if (msg.totalVolume > enMax) enMax = msg.totalVolume;
    enMax = enMax * 0.9997f + 0.0003f;
    if (enMax < 0.01f) enMax = 0.01f;
    fswVol  = constrain(msg.totalVolume / enMax,        0.0f, 1.0f);
    fswBass = constrain(msg.bass        / enMax * 2.5f, 0.0f, 1.0f);
    fswMid  = constrain(msg.mid         / enMax * 2.5f, 0.0f, 1.0f);
    fswTreb = constrain(msg.treble      / enMax * 2.0f, 0.0f, 1.0f);
    // Beat: fast upward bass transient = kick
    static float lastBass = 0.0f;
    float delta = fswBass - lastBass;
    lastBass = fswBass * 0.65f;
    if (delta > 0.18f && fswBass > 0.30f) {
      fswBeat  = true;
      fswBeatI = constrain(delta * 3.5f, 0.3f, 1.0f);
    }
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
  peer.channel = 0;  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) Serial.println("ESP-NOW peer failed");
  else Serial.println("ESP-NOW ready");
}

// 2-second cyan sweep hilt→tip (sync-arm animation)
void renderSyncAnim(float progress) {
  bladeClear();
  int lit = constrain((int)(progress * BLADE_LENGTH), 0, BLADE_LENGTH - 1);
  for (int i = 0; i <= lit; i++) bladeSet(i, CRGB(0, 50, 80));
  bladeSet(lit, CRGB(255, 255, 255));     // bright leading-edge pixel
}

// Searching visual: alternating 10-LED red/purple bands that scroll slowly
void renderSyncSearching() {
  static float offset = 0.0f;
  offset += 0.3f;
  if (offset >= 20.0f) offset -= 20.0f;
  float   pulse = 0.55f + 0.45f * sinf((float)millis() * 0.003f);
  uint8_t val   = (uint8_t)(200.0f * pulse);
  int     off   = (int)offset;
  for (int i = 0; i < BLADE_LENGTH; i++) {
    uint8_t hue = (((i + off) / 10) % 2) ? 192 : 0;   // 192=purple  0=red
    bladeSet(i, CHSV(hue, 255, val));
  }
}

// Subtle cyan pulse at the hilt end — "sync active" indicator overlay
void renderSyncIndicator() {
  float   pulse = (sinf((float)millis() * 0.004f) + 1.0f) * 0.5f;
  uint8_t bri   = (uint8_t)(pulse * 70.0f + 15.0f);
  leds[0]            += CRGB(0, bri, bri);   // hilt side A
  leds[NUM_LEDS - 1] += CRGB(0, bri, bri);   // hilt side B
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
  for (int i = 0; i < NUM_LEDS; i += 5)
    if (blinkState) leds[i] = CRGB(0, 0, 150);
  int filled = (otaProgress * BLADE_LENGTH) / 100;
  for (int i = 0; i < filled; i++) {
    bladeSet(i, CRGB(0, 100, 255));
  }
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
    server.on("/mode", handleMode);
    server.begin();
  }
  ArduinoOTA.setHostname(hostname);
  ArduinoOTA.setPassword(otaPassword);
  ArduinoOTA.onStart([]()                         { otaActive = true; otaProgress = 0; Serial.println("OTA Start"); });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int tot) { otaProgress = (p*100)/tot; });
  ArduinoOTA.onEnd([]()                           { Serial.println("OTA End"); });
  ArduinoOTA.onError([](ota_error_t e)            { Serial.printf("OTA Error[%u]\n", e); });
  ArduinoOTA.begin();
  setupESPNow();
}

// =====================================================================
// WEB SERVER HANDLERS
// =====================================================================
void handleRoot() {
  String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{background:#111;color:#eee;font-family:sans-serif;text-align:center;}";
  html += "button{display:block;width:90%;margin:15px auto;padding:20px;font-size:20px;background:#334;color:#fff;border:none;border-radius:10px;cursor:pointer;}";
  html += "button:active{background:#557;}</style></head><body>";
  html += "<h1>Fullsword Control</h1>";
  html += "<h3>Active Mode: " + String(effectMode) + "</h3>";
  
  html += "<form action='/mode' method='GET'><button name='m' value='0'>Test IMU</button></form>";
  html += "<form action='/mode' method='GET'><button name='m' value='1'>Painter</button></form>";
  html += "<form action='/mode' method='GET'><button name='m' value='2'>Ping Pong</button></form>";
  html += "<form action='/mode' method='GET'><button name='m' value='3'>Fire Storm</button></form>";
  html += "<form action='/mode' method='GET'><button name='m' value='4'>Ocean Waves</button></form>";
  html += "<form action='/mode' method='GET'><button name='m' value='5'>Plasma Storm</button></form>";
  html += "<h3 style='color:#4af'>&#127925; Music Modes</h3>";
  html += "<form action='/mode' method='GET'><button name='m' value='6'>Purple Pulse</button></form>";
  html += "<form action='/mode' method='GET'><button name='m' value='7'>Freq Comets</button></form>";
  html += "<form action='/mode' method='GET'><button name='m' value='8'>Ripple Orch</button></form>";
  html += "<form action='/mode' method='GET'><button name='m' value='9'>Colour Chaser</button></form>";
  html += "<form action='/mode' method='GET'><button name='m' value='10'>Audio Fire</button></form>";

  
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleMode() {
  if (server.hasArg("m")) {
    int m = server.arg("m").toInt();
    if (m >= 0 && m <= 10) {
      effectMode = m;
      pnt.ready = false;
      ballsReady = false;
      bladeClear();
      if (syncEnabled) sendSyncPacket(SYNC_MSG_MODE, effectMode);
    }
  }
  // Redirect back to root
  server.sendHeader("Location", "/");
  server.send(303);
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

  // Startup sweep — travels from hilt to tip
  for (int i = 0; i < BLADE_LENGTH; i++) {
    bladeSet(i, CRGB(100, 0, 150));
    FastLED.show();
    delay(4);
  }
  delay(200);
  bladeClear();
  FastLED.show();

  setupWiFiOTA();
  Serial.println("Ready.  DblClick=mode  TripleHold=sync  Hold=boost");
}


// =====================================================================
// MAIN LOOP
// =====================================================================
void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  if (otaActive) { renderOTAMode(); return; }

  // ── WiFi reconnect watchdog — restores OTA visibility if link drops ───
  static uint32_t wifiCheckMs = 0;
  if (millis() - wifiCheckMs > 30000) {
    wifiCheckMs = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi lost — reconnecting...");
      WiFi.reconnect();
    }
  }

  updateIMU();

  // ── Handle incoming ESP-NOW sync packet (flag set by callback) ────────
  if (syncGotPacket) {
    syncGotPacket = false;
    if (syncInType == SYNC_MSG_MODE && syncEnabled) {
      // Paired peer changed mode — apply it here
      effectMode     = syncInMode % 11;
      pnt.ready      = false;
      pntCompressPos = 0.0f;
      pntCompressVel = 0.0f;
      ballsReady     = false;
      bladeClear();
      const char* names[] = { "TestIMU", "Painter", "PingPong", "FireStorm", "OceanWaves", "PlasmaStorm", 
                              "PurplePulse", "FreqComets", "RippleOrch", "ColourChaser", "AudioFire" };
      Serial.printf("Sync recv: %s\n", names[effectMode]);
    } else if (syncInType == SYNC_MSG_PING) {
      if (syncSearching) {
        // Found a peer! Pair up and confirm back so they pair too.
        syncSearching = false;
        syncEnabled   = true;
        sendSyncPacket(SYNC_MSG_PING, effectMode);
        Serial.println("Sync: PAIRED");
        CRGB fc = CRGB(0, 100, 100);
        for (int f = 0; f < 3; f++) {
          for (int i = 0; i < BLADE_LENGTH; i++) bladeSet(i, fc);
          FastLED.show(); delay(150);
          bladeClear(); FastLED.show(); delay(100);
        }
      } else if (syncEnabled) {
        // Already paired — respond so a newly-searching sword can find us
        sendSyncPacket(SYNC_MSG_PING, effectMode);
      }
    }
  }

  // ── Button: HOLD=boost  DOUBLE-CLICK=mode  TRIPLE-CLICK+HOLD=sync arm ─
  bool btn = digitalRead(BUTTON_PIN);
  if (btn == LOW && lastButton == HIGH) {
    uint32_t now = millis();
    if (now - lastClickMs > 40) {   // debounce
      clickCount++;
      lastClickMs  = now;
      buttonDownMs = now;
    }
  }
  // Triple-click + hold → start 2-second sync-arm animation
  if (!syncAnimating && clickCount >= 3 && btn == LOW && millis() - buttonDownMs > 80) {
    syncAnimating  = true;
    syncAnimStart  = millis();
    syncSearching  = false;   // cancel any ongoing search
    clickCount     = 0;
    boostMode      = false;
  }
  // Boost: hold from a fresh press (no pending multi-click, not syncing)
  if (btn == LOW && !boostMode && !syncAnimating && clickCount < 3 &&
      millis() - buttonDownMs > BOOST_HOLD_MS) {
    boostMode  = true;
    clickCount = 0;
  }
  // Release ends boost
  if (btn == HIGH && lastButton == LOW && boostMode) {
    boostMode = false;
  }
  lastButton = btn;

  // Dispatch after click window (not held into boost/sync)
  if (!boostMode && !syncAnimating && clickCount > 0 && millis() - lastClickMs > DCLICK_MS) {
    if (clickCount == 2) {   // exactly double-click → cycle mode
      effectMode     = (effectMode + 1) % 11;
      pnt.ready      = false;
      pntCompressPos = 0.0f;
      pntCompressVel = 0.0f;
      ballsReady     = false;
      fswBeat        = false;
      bladeClear();
      const char* names[] = { "TestIMU", "Painter", "PingPong", "FireStorm", "OceanWaves", "PlasmaStorm",
                              "PurplePulse", "FreqComets", "RippleOrch", "ColourChaser", "AudioFire" };
      Serial.printf("Mode: %s\n", names[effectMode]);
      if (syncEnabled) sendSyncPacket(SYNC_MSG_MODE, effectMode);  // broadcast to peers
    }
    // Single-click and 3+ quick clicks: discard silently
    clickCount = 0;
  }

  // ── Sync-arm animation (triple-click + hold for 2 seconds) ───────────
  if (syncAnimating) {
    float progress = (float)(millis() - syncAnimStart) / (float)SYNC_ANIM_MS;
    if (btn == HIGH) {
      // Released before complete — cancel silently
      syncAnimating = false;
    } else if (progress >= 1.0f) {
      syncAnimating = false;
      if (syncEnabled) {
        // Already paired — turn sync off
        syncEnabled = false;
        Serial.println("Sync: OFF");
        CRGB fc = CRGB(100, 40, 0);
        for (int f = 0; f < 3; f++) {
          for (int i = 0; i < BLADE_LENGTH; i++) bladeSet(i, fc);
          FastLED.show(); delay(150);
          bladeClear(); FastLED.show(); delay(100);
        }
      } else {
        // Start 60-second discovery window
        syncSearching  = true;
        syncSearchStart = millis();
        lastPingMs     = 0;
        Serial.printf("Sync: searching 60s  MAC: %s\n", WiFi.macAddress().c_str());
        CRGB fc = CRGB(0, 80, 100);
        for (int f = 0; f < 3; f++) {
          for (int i = 0; i < BLADE_LENGTH; i++) bladeSet(i, fc);
          FastLED.show(); delay(150);
          bladeClear(); FastLED.show(); delay(100);
        }
      }
    } else {
      // Still animating — render sweep and return early
      renderSyncAnim(progress);
      FastLED.setBrightness(BRIGHTNESS);
      FastLED.show();
      delay(16);
      return;
    }
  }

  // ── Sync search: broadcast pings every 500 ms, timeout after 60 s ────
  if (syncSearching) {
    if (millis() - syncSearchStart >= SYNC_SEARCH_MS) {
      syncSearching = false;
      Serial.println("Sync: search timed out — no peer found");
      CRGB fc = CRGB(100, 40, 0);
      for (int f = 0; f < 3; f++) {
        for (int i = 0; i < BLADE_LENGTH; i++) bladeSet(i, fc);
        FastLED.show(); delay(150);
        bladeClear(); FastLED.show(); delay(100);
      }
    } else if (millis() - lastPingMs > 500) {
      sendSyncPacket(SYNC_MSG_PING, effectMode);
      lastPingMs = millis();
    }
  }

  // ── Effects (or sync-searching override) ──────────────────────────────
  if (syncSearching) {
    renderSyncSearching();
  } else {
    // Primary Modes Dispatched Here
    switch (effectMode) {
      case 0: effectTestIMU(); break;
      case 1: effectPainter(); break;
      case 2: effectPingPong(); break;
      case 3: effectFireStorm(); break;
      case 4: effectOceanWaves(); break;
      case 5: effectPlasmaStorm(); break;
      case 6: effectMusicPulse(); break;
      case 7: effectMusicComets(); break;
      case 8: effectMusicRipples(); break;
      case 9: effectMusicChaser(); break;
      case 10: effectMusicFire(); break;
    }
  }

  // Roll Overlay — skip during sync search (blade is already taken)
  if (!syncSearching) {
    updateAndRenderRollOverlay();
  }

  if (boostMode) {
    renderBoostSparks();
    // Rapid brightness pulse at ~8 Hz — "rushed energy" feel
    float pulse = 0.72f + 0.28f * sinf((float)millis() * 0.050f);
    FastLED.setBrightness((uint8_t)(BRIGHTNESS * pulse));
  } else {
    FastLED.setBrightness(BRIGHTNESS);
  }

  // Sync indicator: subtle cyan pulse at hilt while paired
  if (syncEnabled) renderSyncIndicator();

  // Impact waves layer over everything
  if (!syncSearching) {
    updateAndRenderImpacts();
  }
  
  // Render accent LEDs base animation last
  renderAccents();

  FastLED.show();
  delay(16);   // ~60 fps
}
