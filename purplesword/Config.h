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
#define NUM_LEDS      86
#define BLADE_HALF    43      // physical LEDs per side (strip folded at tip)
#define BRIGHTNESS    180
#define OTA_BRIGHTNESS 60

// Virtual blade length — effects run in 0..179 space, mapped to 43 physical
#define BLADE_LENGTH  180
#define HILT_LEDS     0
#define BLADE_PIXELS  BLADE_LENGTH

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

// ─── GLOBALS (defined in purplesword.ino) ─────────────────────────────
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
  // Spatial downsampling mapping from virtual 180 to physical 43 LEDs
  int phys = (pos * BLADE_HALF) / BLADE_LENGTH;
  leds[phys] = color;
  leds[NUM_LEDS - 1 - phys] = color;
}

inline CRGB bladeGet(int pos) {
  if (pos < 0 || pos >= BLADE_LENGTH) return CRGB::Black;
  int phys = (pos * BLADE_HALF) / BLADE_LENGTH;
  return leds[phys];
}

inline void bladeClear() {
  fill_solid(leds, NUM_LEDS, CRGB::Black);
}

// ─── EFFECT FUNCTION DECLARATIONS ─────────────────────────────────────
void effectFire();
void effectRainbow();
void effectLightning();

// ─── POV OVERLAY DECLARATIONS ─────────────────────────────────────────
void updatePOV();
void renderPOVOverlay(uint8_t mode);
