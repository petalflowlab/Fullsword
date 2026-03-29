#pragma once
// =====================================================================
// Config.h — Constants, externs, inline blade helpers, mode enum
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
#define BLADE_LENGTH  180
#define HILT_LEDS     15
#define BLADE_START   HILT_LEDS
#define BLADE_END     BLADE_LENGTH
#define BLADE_PIXELS  (BLADE_END - BLADE_START)   // = 165
#define BRIGHTNESS    180
#define OTA_BRIGHTNESS 60

// ─── IMU ──────────────────────────────────────────────────────────────
#define IMU_ADDR      0x68

// ─── BUTTON / SYNC TIMING ─────────────────────────────────────────────
#define DCLICK_MS       350
#define BOOST_HOLD_MS   500
#define SYNC_ANIM_MS    2000
#define SYNC_SEARCH_MS  60000
#define SYNC_MSG_MODE   0x01
#define SYNC_MSG_PING   0x02

// ─── MODE ENUM ────────────────────────────────────────────────────────
enum EffectModeEnum : uint8_t {
  MODE_FIRE      = 0,
  MODE_RAINBOW   = 1,
  MODE_LIGHTNING = 2,
  MODE_COUNT     = 3
};

// ─── GLOBALS (defined in Fullsword.ino) ───────────────────────────────
extern CRGB     leds[NUM_LEDS];
extern bool     imuOk;
extern float    swingMag, currSwing, twistRate;
extern float    accelX, accelY, accelZ;
extern float    gx, gy, gz;
extern int8_t   tiltDir;
extern uint8_t  effectMode;
extern uint8_t  baseHue;
extern bool     fireActive;
extern uint8_t  currentMode;
extern bool     autoCycleMode;

// ─── INLINE BLADE HELPERS ─────────────────────────────────────────────
inline void bladeSet(int pos, CRGB color) {
  if (pos < 0 || pos >= BLADE_LENGTH) return;
  leds[4 + pos] = color;
}

inline CRGB bladeGet(int pos) {
  if (pos < 0 || pos >= BLADE_LENGTH) return CRGB::Black;
  return leds[4 + pos];
}

inline void bladeClear() {
  fill_solid(leds + 4, NUM_LEDS - 4, CRGB::Black);
}

// ─── EFFECT FUNCTION DECLARATIONS ─────────────────────────────────────
void effectFire();
void effectRainbow();
void effectLightning();

// ─── POV OVERLAY DECLARATIONS ─────────────────────────────────────────
// updatePOV()          — integrate gyro angle; call once per loop() before effects.
// renderPOVOverlay(m)  — world-locked pattern on top of effect; call after effect,
//                        before FastLED.show().
void updatePOV();
void renderPOVOverlay(uint8_t mode);
