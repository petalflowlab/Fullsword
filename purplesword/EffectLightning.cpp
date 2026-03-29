#include "Config.h"

// =====================================================================
// EFFECT: LIGHTNING STORM — Blue storm rush + lightning strike on stop
// =====================================================================

#define MAX_STORM_PARTICLES 24

struct StormParticle {
  float pos, vel, intensity;
  uint8_t hue;
  bool active;
};

void effectLightning() {
  static StormParticle particles[MAX_STORM_PARTICLES];
  static int particleNext = 0;
  static bool firstRun = true;
  static float prevSwingMag = 0.0f;
  static uint32_t lastSpawnMs = 0;

  static uint32_t strikeMs = 0;
  static uint8_t boltPath[BLADE_LENGTH];
  static uint32_t lastStopDetectMs = 0;

  static float cracklePhase = 0.0f;
  static float ambientPhase = 0.0f;
  static float stormGlow[BLADE_LENGTH];

  if (firstRun) {
    firstRun = false;
    memset(particles, 0, sizeof(particles));
    memset(stormGlow, 0, sizeof(stormGlow));
    memset(boltPath, 0, sizeof(boltPath));
  }

  fireActive = false;
  cracklePhase += 0.08f;
  ambientPhase += 0.015f;

  for (int i = 0; i < HILT_LEDS; i++) bladeSet(i, CRGB::Black);

  // ── 1. AMBIENT: Dark blue-purple crackling ────────────────────────────
  for (int i = HILT_LEDS; i < BLADE_LENGTH; i++) {
    stormGlow[i] *= 0.92f;
    float ambient = 0.15f + 0.10f * sinf(ambientPhase * 2.0f + (float)i * 0.08f);
    if (random8() < 8) ambient += (float)random8(20, 80) / 255.0f;
    float totalGlow = constrain(ambient + stormGlow[i], 0.0f, 1.0f);
    uint8_t hue = 160 + (uint8_t)(10.0f * sinf(cracklePhase + (float)i * 0.05f));
    bladeSet(i, CHSV(hue, 200 + random8(55), (uint8_t)(totalGlow * 80.0f)));
  }

  // ── 2. SWING → Storm particles rushing hilt→tip ───────────────────────
  float dSwing = swingMag - prevSwingMag;
  prevSwingMag = swingMag;

  if (swingMag > 60.0f && (millis() - lastSpawnMs > 30)) {
    lastSpawnMs = millis();
    int spawnCount = constrain(1 + (int)(swingMag / 120.0f), 1, 4);
    for (int s = 0; s < spawnCount; s++) {
      StormParticle& p = particles[particleNext];
      p.active    = true;
      p.pos       = (float)HILT_LEDS + (float)random8(15);
      p.vel       = fminf(2.0f + swingMag * 0.015f + (float)random8(20) * 0.1f, 7.0f);
      p.intensity = fminf(0.6f + (swingMag / 500.0f) * 0.4f, 1.0f);
      p.hue       = 140 + random8(40);
      particleNext = (particleNext + 1) % MAX_STORM_PARTICLES;
    }
  }

  // ── Update & render storm particles ───────────────────────────────────
  for (int s = 0; s < MAX_STORM_PARTICLES; s++) {
    if (!particles[s].active) continue;
    StormParticle& p = particles[s];
    p.pos += p.vel + (float)(random8() - 128) * 0.02f;
    p.intensity *= 0.97f;
    if (p.pos >= BLADE_LENGTH + 5 || p.intensity < 0.05f) { p.active = false; continue; }

    int center = (int)p.pos;
    for (int j = -3; j <= 3; j++) {
      int idx = center + j;
      if (idx < HILT_LEDS || idx >= BLADE_LENGTH) continue;
      float falloff = 1.0f - fabsf((float)j) / 3.0f;
      if (falloff <= 0.0f) continue;
      uint8_t bri = (uint8_t)(p.intensity * falloff * 255.0f);
      CRGB pc;
      if (j == 0) pc = CRGB((uint8_t)(bri*0.7f), (uint8_t)(bri*0.85f), bri);
      else        pc = CHSV(p.hue, 200, bri);
      bladeSet(idx, bladeGet(idx) + pc);
      stormGlow[idx] = fmaxf(stormGlow[idx], p.intensity * falloff * 0.5f);
    }
  }

  // ── 3. SUDDEN STOP → LIGHTNING STRIKE ─────────────────────────────────
  if (swingMag > 50.0f && dSwing < -15.0f && (currSwing < 40.0f) && (millis() - lastStopDetectMs > 500)) {
    strikeMs = millis();
    lastStopDetectMs = millis();
    int offset = 0;
    for (int i = 0; i < BLADE_LENGTH; i++) {
      offset += random8(3) - 1;
      offset = constrain(offset, -3, 3);
      boltPath[i] = (uint8_t)(abs(offset) + 1);
      if (random8() < 30) boltPath[i] = 4 + random8(3);
    }
  }

  uint32_t strikeAge = millis() - strikeMs;
  if (strikeAge < 400) {
    if (strikeAge < 60) {
      // Full-blade white flash
      float flashT = (float)strikeAge / 60.0f;
      uint8_t flashBri = (uint8_t)((1.0f - flashT * 0.3f) * 255.0f);
      for (int i = HILT_LEDS; i < BLADE_LENGTH; i++)
        if (random8() < 240) bladeSet(i, CRGB(flashBri, flashBri, flashBri));
    } else if (strikeAge < 200) {
      // Jagged bolt
      float boltT = (float)(strikeAge - 60) / 140.0f;
      float boltInt = 1.0f - boltT * 0.5f;
      bool flicker = ((millis() / 25) % 2) == 0;
      for (int i = HILT_LEDS; i < BLADE_LENGTH; i++) {
        int w = boltPath[i];
        if (w >= 3 || flicker) {
          uint8_t bb = (uint8_t)(boltInt * 255.0f);
          bladeSet(i, bladeGet(i) + CRGB((uint8_t)(bb*0.9f), (uint8_t)(bb*0.95f), bb));
        }
        if (w > 2 && random8() < 180)
          bladeSet(i, bladeGet(i) + CHSV(155, 180, (uint8_t)(boltInt * 120.0f)));
      }
    } else {
      // Blue-purple afterglow
      float fadeInt = 1.0f - (float)(strikeAge - 200) / 200.0f;
      for (int i = HILT_LEDS; i < BLADE_LENGTH; i++) {
        if (boltPath[i] > 1)
          bladeSet(i, bladeGet(i) + CHSV(180, 200, (uint8_t)(fadeInt * 80.0f * ((float)boltPath[i] / 7.0f))));
        stormGlow[i] = fmaxf(stormGlow[i], fadeInt * 0.4f);
      }
    }
  }

  // ── 4. ELECTRIC HILT GLOW ─────────────────────────────────────────────
  {
    static float hiltPhase = 0.0f;
    hiltPhase += 0.06f;
    for (int j = 0; j < 20; j++) {
      int idx = HILT_LEDS + j; if (idx >= BLADE_LENGTH) break;
      float falloff = 1.0f - (float)j / 20.0f;
      float pulse   = 0.70f + 0.30f * sinf(hiltPhase + (float)j * 0.6f);
      float twinkle = random8(180, 255) / 255.0f;
      uint8_t bri = (uint8_t)(falloff * pulse * twinkle * 180.0f);
      CRGB hc;
      if (j < 6) hc = CRGB((uint8_t)(bri*0.4f), (uint8_t)(bri*0.7f), bri);
      else       hc = CHSV(155, 220, bri);
      bladeSet(idx, bladeGet(idx).lerp8(hc, (uint8_t)(falloff * 210)));
    }
    if (random8() < 25) {
      int sp = HILT_LEDS + random8(8);
      if (sp < BLADE_LENGTH) bladeSet(sp, bladeGet(sp) + CRGB(60, 80, 255));
    }
  }

  static uint32_t lastPrint = 0;
  if (millis() - lastPrint > 500) {
    Serial.printf("LIGHTNING | swing:%.1f dSwing:%.1f | strike:%s\n", swingMag, dSwing, (strikeAge < 400) ? "YES" : "no");
    lastPrint = millis();
  }
}
