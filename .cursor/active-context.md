> **BrainSync Context Pumper** 🧠
> Dynamically loaded for active file: `purplesword\Config.h` (Domain: **Generic Logic**)

### 📐 Generic Logic Conventions & Fixes
- **[what-changed] Updated configuration CONFIGURATION**: - #define ACCENT_PIN    D2
+ #define BUTTON_PIN    D3
- #define BUTTON_PIN    D3
+ #define SDA_PIN       D4
- #define SDA_PIN       D4
+ #define SCL_PIN       D5
- #define SCL_PIN       D5
+ 
- 
+ // ─── LED CONFIGURATION ────────────────────────────────────────────────
- // ─── LED CONFIGURATION ────────────────────────────────────────────────
+ #define NUM_LEDS      86
- #define NUM_LEDS      86
+ #define BLADE_HALF    43      // physical LEDs per side (strip folded at tip)
- #define BLADE_HALF    43      // logical positions: 0=hilt, 42=tip
+ #define BRIGHTNESS    180
- #define BRIGHTNESS    180
+ #define OTA_BRIGHTNESS 60
- #define OTA_BRIGHTNESS 60
+ 
- 
+ // Virtual blade length — effects run in 0..179 space, mapped to 43 physical
- // Virtual configuration for existing effects
+ #define BLADE_LENGTH  180
- #define BLADE_LENGTH  180
+ 
- #define HILT_LEDS     15
+ // ─── IMU ──────────────────────────────────────────────────────────────
- #define BLADE_START   HILT_LEDS
+ #define IMU_ADDR      0x68
- #define BLADE_END     BLADE_LENGTH
+ 
- #define BLADE_PIXELS  (BLADE_END - BLADE_START)   // = 165
+ // ─── BUTTON / SYNC TIMING ─────────────────────────────────────────────
- #define NUM_ACCENTS   4
+ #define DCLICK_MS       350
- 
+ #define BOOST_HOLD_MS   500
- // ─── IMU ──────────────────────────────────────────────────────────────
+ #define SYNC_ANIM_MS    2000
- #define IMU_ADDR      0x68
+ #define SYNC_SEARCH_MS  60000
- 
+ #define SYNC_MSG_MODE   0x01
- // ─── BUTTON / SYNC TIMING ─────────────────────────────────────────────
+ #define SYNC_MSG_PING   0x02
- #define DCLICK_MS       350
+ 
- #define BOOST_HOLD_MS   500
+ // ─── MODE ENUM ────────────────────────────────────────────────────────
- #define SYNC_ANIM_MS    2000
+ enum EffectModeEnum : uint8_t {
- #define SYNC_SEARCH_MS  60000
+   MODE_FIRE      = 0,
- #define SYNC_MSG_MODE   0x01
+   MODE_RAINBOW   = 1,
- #define SYNC_MSG_PING   0x02
+   MODE_LIGHTNING = 2,
- 
+   MODE_COUNT     = 3
- // ─── 
… [diff truncated]
- **[what-changed] Updated schema PHYSICAL — externalizes configuration for environment flexibility**: - // PHYSICAL LED LAYOUT — Two-Sided Folded Strip (86 total):
+ // PHYSICAL LED LAYOUT — Single Folded Strip (86 total, blade only):
- //   The single strip is folded back on itself to create two blade faces.
+ //   One strip folded at the tip creates two blade faces.
- //   Side B: leds[43] (tip)  → leds[85] (hilt)  — strip runs tip→hilt (folded back)
+ //   Side B: leds[43] (tip)  → leds[85] (hilt)  — strip runs tip→hilt
- //   bladeSet(pos, color) writes to BOTH sides simultaneously:
+ //   bladeSet(pos, color) maps virtual pos (0–179) to physical (0–42)
- //     phys = (pos * 43) / 180
+ //   and mirrors to both sides simultaneously.
- //     leds[phys]        = color   ← side A (hilt→tip)
+ //
- //     leds[85 - phys]   = color   ← side B (tip→hilt, mirrored by fold)
+ // Button (D3): Click = next mode | Hold > 0.5s = Boost | Triple+Hold = Sync
- //
+ // OTA hostname: PurpleSword  |  OTA [REDACTED]
- //   Accent LEDs (4) are on a separate strip on ACCENT_PIN.
+ // =====================================================================
- //
+ 
- // Button (D3): Click = next mode | Hold > 0.5s = Boost | Triple+Hold = Sync
+ #include "Config.h"
- // OTA hostname: PurpleSword  |  OTA [REDACTED]
+ 
- // =====================================================================
+ // ─── GLOBAL VARIABLE DEFINITIONS ──────────────────────────────────────
- 
+ const char* ssid        = "CGN3-4400";
- #include "Config.h"
+ const char* [REDACTED]
- 
+ const char* hostname    = "PurpleSword";
- // ─── GLOBAL VARIABLE DEFINITIONS ──────────────────────────────────────
+ const char* otaHash   = "f3b462d93b24cb0538f5d864546bc3e0"; // MD5 hash of "sword"
- const char* ssid        = "CGN3-4400";
+ WebServer   server(80);
- const char* [REDACTED]
+ 
- const char* hostname    = "PurpleSword";
+ bool     otaActive   = false;
- const char* otaHash   = "f3b462d93b24cb0538f5d864546bc3e0"; // MD5 hash of "sword"
+ uint16_t otaProgress = 0;
- We
… [diff truncated]
- **[what-changed] Updated schema HIGH — ensures atomic multi-step database operations**: - CRGB accentLeds[NUM_ACCENTS];
+ 
- 
+ bool   imuOk     = false;
- bool   imuOk     = false;
+ float  swingMag  = 0.0f;
- float  swingMag  = 0.0f;
+ float  currSwing = 0.0f;
- float  currSwing = 0.0f;
+ float  twistRate = 0.0f;
- float  twistRate = 0.0f;
+ float  accelX    = 0.0f;
- float  accelX    = 0.0f;
+ float  accelY    = 0.0f;
- float  accelY    = 0.0f;
+ float  accelZ    = 0.0f;
- float  accelZ    = 0.0f;
+ float  gx = 0.0f, gy = 0.0f, gz = 0.0f;
- float  gx = 0.0f, gy = 0.0f, gz = 0.0f;
+ int8_t tiltDir   =  1;
- int8_t tiltDir   =  1;
+ 
- 
+ uint8_t  effectMode   = 0;
- uint8_t  effectMode   = 0;
+ uint8_t  baseHue      = 190;
- uint8_t  baseHue      = 190;
+ 
- 
+ uint8_t  clickCount   = 0;
- uint8_t  clickCount   = 0;
+ uint32_t lastClickMs  = 0;
- uint32_t lastClickMs  = 0;
+ bool     lastButton   = HIGH;
- bool     lastButton   = HIGH;
+ 
- 
+ bool     boostMode    = false;
- bool     boostMode    = false;
+ uint32_t buttonDownMs = 0;
- uint32_t buttonDownMs = 0;
+ 
- 
+ float rollPhase = 0.0f;
- float rollPhase = 0.0f;
+ float lastGyroZ = 0.0f;
- float lastGyroZ = 0.0f;
+ 
- 
+ bool fireActive = false;
- bool fireActive = false;
+ 
- 
+ bool      syncEnabled    = false;
- bool      syncEnabled    = false;
+ bool      syncSearching  = false;
- bool      syncSearching  = false;
+ uint32_t  syncSearchStart = 0;
- uint32_t  syncSearchStart = 0;
+ uint32_t  lastPingMs     = 0;
- uint32_t  lastPingMs     = 0;
+ bool      syncAnimating  = false;
- bool      syncAnimating  = false;
+ uint32_t  syncAnimStart  = 0;
- uint32_t  syncAnimStart  = 0;
+ volatile bool    syncGotPacket = false;
- volatile bool    syncGotPacket = false;
+ volatile uint8_t syncInType    = 0;
- volatile uint8_t syncInType    = 0;
+ volatile uint8_t syncInMode    = 0;
- volatile uint8_t syncInMode    = 0;
+ 
- 
+ struct SyncPacket { uint8_t type; uint8_t mode; };
- struct SyncPacket { uint8_t type; uint8_t mode; };
+ uint8_t broadcastAddr[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
- uin
… [diff truncated]
- **[what-changed] Updated schema Multi — externalizes configuration for environment flexibility**: - // Fullsword.ino — Multi-Mode Sword Controller
+ // purplesword.ino — Multi-Mode Purple Sword Controller
- // LED layout (184 total):
+ // PHYSICAL LED LAYOUT — Two-Sided Folded Strip (86 total):
- //   leds[0..3]   = Accent LEDs (base of strip)
+ //
- //   leds[4..183] = Blade (hilt→tip in bladeSet/bladeGet coordinates)
+ //   The single strip is folded back on itself to create two blade faces.
- //
+ //   Side A: leds[0]  (hilt) → leds[42] (tip)   — strip runs hilt→tip
- // Button (D3): Click = next mode | Hold > 0.5s = Boost | Triple+Hold = Sync
+ //   Side B: leds[43] (tip)  → leds[85] (hilt)  — strip runs tip→hilt (folded back)
- // OTA hostname: Fullsword  |  OTA [REDACTED]
+ //
- // =====================================================================
+ //   bladeSet(pos, color) writes to BOTH sides simultaneously:
- 
+ //     phys = (pos * 43) / 180
- #include "Config.h"
+ //     leds[phys]        = color   ← side A (hilt→tip)
- 
+ //     leds[85 - phys]   = color   ← side B (tip→hilt, mirrored by fold)
- // ─── GLOBAL VARIABLE DEFINITIONS ──────────────────────────────────────
+ //
- const char* ssid        = "CGN3-4400";
+ //   Accent LEDs (4) are on a separate strip on ACCENT_PIN.
- const char* [REDACTED]
+ //
- const char* hostname    = "PurpleSword";
+ // Button (D3): Click = next mode | Hold > 0.5s = Boost | Triple+Hold = Sync
- const char* otaHash   = "f3b462d93b24cb0538f5d864546bc3e0"; // MD5 hash of "sword"
+ // OTA hostname: PurpleSword  |  OTA [REDACTED]
- WebServer   server(80);
+ // =====================================================================
- bool     otaActive   = false;
+ #include "Config.h"
- uint16_t otaProgress = 0;
+ 
- 
+ // ─── GLOBAL VARIABLE DEFINITIONS ──────────────────────────────────────
- CRGB leds[NUM_LEDS];
+ const char* ssid        = "CGN3-4400";
- CRGB accentLeds[NUM_ACCENTS];
+ const char* [REDACTED]
- 
+ const char* hostname    = "PurpleSword";
- bool   imuOk     =
… [diff truncated]
- **[what-changed] what-changed in EffectFire.cpp**: -       for (int j = 0; j < 30; j++) {
+       for (int j = 0; j < 40; j++) {
-         CRGB color;
+         
-         if (j < 10) {
+         if (j > 8) {
-           float coreFrac = (float)j / 10.0f;
+           float gapChance = (float)(j - 8) / 32.0f;
-           float twinkle  = random8(200, 255) / 255.0f;
+           if (random8() < (uint8_t)(gapChance * 200)) {
-           uint8_t g = (uint8_t)((1.0f - coreFrac * 0.6f) * 220.0f * twinkle * fbStrobe);
+             continue;
-           uint8_t b = (uint8_t)((1.0f - coreFrac) * 80.0f * fbStrobe);
+           }
-           color = CRGB(255, g, b);
+         }
-         } else if (j < 20) {
+ 
-           float t = (float)(j - 10) / 10.0f;
+         uint8_t rHue = random8(35);
-           float twinkle = random8(180, 255) / 255.0f;
+         uint8_t hue = (rHue < 20) ? rHue : (256 - (rHue - 19));
-           uint8_t g = (uint8_t)((1.0f - t * 0.75f) * 130.0f * twinkle * fbPulse);
+         float brightness = 1.0f - ((float)j / 40.0f); 
-           color = CRGB(255, g, 0);
+         CRGB color;
-         } else {
+ 
-           float t = (float)(j - 20) / 10.0f;
+         if (j < 6) {
-           uint8_t r = (uint8_t)((1.0f - t) * 200.0f);
+           float twinkle = random8(180, 255) / 255.0f;
-           color = CRGB(r, 0, 0);
+           float intensity = brightness * twinkle * fbStrobe;
-         }
+           color = CHSV(hue, random8(120, 200), (uint8_t)(intensity * 255));
-         if (color) { bladeSet(idx, color); fireTrail[idx] = 1.0f; trailHue[idx] = (j < 15) ? 32 : 5; }
+         } else {
-       }
+           float twinkle = random8(100, 255) / 255.0f;
-     }
+           float intensity = brightness * twinkle * fbPulse;
-   }
+           color = CHSV(hue, random8(240, 255), (uint8_t)(intensity * 255));
- 
+         }
-   // ── COMPRESSION PHASE ─────────────────────────────────────────────────
+ 
-   if (fireCompressing) {
+         if (color) { 
-     float compressT = (float)(millis() - compress
… [diff truncated]
- **[what-changed] what-changed in EffectFire.cpp**: File updated (external): purplesword/EffectFire.cpp

Content summary (359 lines):
#include "Config.h"

// =====================================================================
// EFFECT: FIRE — Melting Lava + Swing Fireball
// (Moved verbatim from the original effectTestIMU)
// =====================================================================

void effectFire() {
  const float dt = 0.016f;
  static float fizzle = 0.0f;
  static float lastSwing = 0.0f;
  static float hiltFlash = 0.0f;

  static uint32_t fireStartMs  = 0;
  static float    fireSpeed    = 0.0f;
  static bool
- **[what-changed] what-changed in purplesword.ino**: File updated (external): purplesword/purplesword.ino

Content summary (617 lines):

// =====================================================================
// Fullsword.ino — Multi-Mode Sword Controller
// =====================================================================
//
// Modes (single-click cycles):
//   0 = Fire        — Melting lava + swing fireball
//   1 = Rainbow     — Paint splashes that mush & mix hilt→tip
//   2 = Lightning   — Blue storm rush + lightning strike on stop
//
// LED layout (184 total):
//   leds[0..3]   = Accent LEDs (base of strip)
//   leds[4
