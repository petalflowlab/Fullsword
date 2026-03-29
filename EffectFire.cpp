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
  static bool     fireLive     = false;
  static float    prevSwingMag = 0.0f;
  static bool     fireCharge   = false;
  static uint32_t chargeStart  = 0;
  static bool     fireCompressing = false;
  static uint32_t compressStartMs = 0;
  static float    lavaIntensity   = 0.0f;
  static uint32_t stopFlashMs     = 0;
  static uint32_t diagStopMs      = 0;
  static uint32_t diagFadeMs      = 0;
  static float    lastLavaInt     = 0.0f;

  static float   fireTrail[BLADE_LENGTH] = {0};
  static uint8_t trailHue[BLADE_LENGTH]   = {0};
  static float   trailMaxHalf = 0.0f;

  static float lava[BLADE_LENGTH] = {};
  static float lavaPhase    = 0.0f;
  static float hbPhase      = 0.0f;
  static float shimmerPhase = 0.0f;

  bool strikeBlocking = fireCharge || fireLive;
  bool fizzleActive   = fireCompressing || (trailMaxHalf > 0.01f);
  fireActive          = strikeBlocking || fizzleActive;

  if (strikeBlocking) {
    bladeClear();
  } else {
    for (int i = 0; i < HILT_LEDS; i++) bladeSet(i, CRGB::Black);
  }

  // ── Hilt flash ────────────────────────────────────────────────────────
  if (!fireActive) {
    if (swingMag > 200.0f || fabsf(twistRate) > 300.0f) hiltFlash = 1.0f;
    if (hiltFlash > 0.01f) {
      uint8_t bri = (uint8_t)(hiltFlash * 220.0f);
      CRGB hiltColor;
      if (fabsf(twistRate) > 300.0f) hiltColor = CRGB::White;
      else if (accelY < -0.4f)       hiltColor = CRGB(bri, bri/4, 0);
      else                            hiltColor = CRGB(bri, bri, bri/2);
      for (int i = 0; i < HILT_LEDS; i++) bladeSet(i, hiltColor);
      hiltFlash *= 0.80f;
    }
  } else {
    hiltFlash = 0.0f;
    if (lavaIntensity > 0.0f) lavaIntensity = 0.0f;
  }

  // ── DIAGNOSTIC HILT FLASHES ───────────────────────────────────────────
  if (diagStopMs && (millis() - diagStopMs < 300)) {
    float t = (float)(millis() - diagStopMs) / 300.0f;
    uint8_t bri = (uint8_t)((1.0f - t) * 255.0f);
    for (int i = 0; i < HILT_LEDS; i++) bladeSet(i, CRGB(0, 0, bri));
  } else if (diagFadeMs && (millis() - diagFadeMs < 300)) {
    float t = (float)(millis() - diagFadeMs) / 300.0f;
    uint8_t bri = (uint8_t)((1.0f - t) * 255.0f);
    for (int i = 0; i < HILT_LEDS; i++) bladeSet(i, CRGB(bri, 0, bri));
  }

  // ── 1. Orientation & Twist ────────────────────────────────────────────
  bool twisting = (!strikeBlocking) && (fabsf(twistRate) > 600.0f);

  if (!strikeBlocking && twisting) {
    if ((millis() / 30) % 2) fill_solid(leds + 4 + HILT_LEDS, BLADE_PIXELS, CRGB::White);
    else                      fill_solid(leds + 4 + HILT_LEDS, BLADE_PIXELS, CRGB::Black);
  } else if (!strikeBlocking) {
    // ── Melting Lava ────────────────────────────────────────────────────
    lavaIntensity = fminf(1.0f, lavaIntensity + 0.15f);
    if (lastLavaInt <= 0.0f && lavaIntensity > 0.0f) diagFadeMs = millis();
    lastLavaInt = lavaIntensity;

    float sway = (fabsf(gx) + fabsf(gy) + fabsf(gz)) * 0.0001f;
    lavaPhase    += 0.022f + sway * 2.0f;
    hbPhase      += 0.008f;
    shimmerPhase += 0.065f;
    float hb = 0.5f + 0.5f * sinf(hbPhase);

    float hiltHeat = (0.55f + 0.45f * hb) + fminf(sway * 5.0f, 0.2f);
    for (int i = HILT_LEDS; i < HILT_LEDS + 8; i++) {
      float fade = 1.0f - (float)(i - HILT_LEDS) / 8.0f;
      lava[i] = fmaxf(lava[i], hiltHeat * fade);
    }

    for (int i = BLADE_LENGTH - 1; i > HILT_LEDS; i--) {
      float pos  = (float)(i - HILT_LEDS) / BLADE_PIXELS;
      float coolRate  = 0.970f - pos * 0.030f + hb * 0.008f;
      float advect    = 0.18f + fminf(sway * 0.3f, 0.08f);
      float backdiff  = 0.015f;
      lava[i] = lava[i] * coolRate
              + lava[i - 1] * advect
              - lava[i + 1 < BLADE_LENGTH ? i + 1 : i] * backdiff;
      float wobbleAmp = 0.012f + sway * 0.3f;
      lava[i] += wobbleAmp * sinf(lavaPhase * 1.8f + (float)i * 0.22f);
      lava[i] = constrain(lava[i], 0.0f, 1.0f);
    }

    float pulseR = 0.88f + 0.12f * sinf(shimmerPhase * 0.70f);
    float pulseO = 0.86f + 0.14f * sinf(shimmerPhase * 1.20f + 1.0f);
    float pulseY = 0.84f + 0.16f * sinf(shimmerPhase * 1.80f + 2.0f);

    for (int i = HILT_LEDS; i < BLADE_LENGTH; i++) {
      float h = lava[i];
      float flicker = 0.88f + 0.12f * sinf(lavaPhase * 4.7f + (float)i * 0.31f);
      h *= flicker;
      h = constrain(h, 0.0f, 1.0f);

      CRGB color;
      if (h < 0.55f) {
        uint8_t r = (uint8_t)(fminf(1.0f, h / 0.55f + 0.2f) * 200 * pulseR);
        color = CRGB(r, 0, 0);
      } else if (h < 0.80f) {
        float t = (h - 0.55f) / 0.25f;
        float rnd = (float)random8(160, 255) / 255.0f;
        uint8_t r = (uint8_t)((180 + t * 65) * pulseO * rnd);
        uint8_t g = (uint8_t)((t * t * 60) * pulseO * rnd);
        color = CRGB(r, g, 0);
      } else if (h < 0.90f) {
        float t = (h - 0.80f) / 0.10f;
        float rnd = (float)random8(140, 255) / 255.0f;
        uint8_t r = (uint8_t)((245 + t * 10) * pulseO * rnd);
        uint8_t g = (uint8_t)((45 + t * 100) * pulseO * rnd);
        color = CRGB(r, g, 0);
      } else {
        float t = (h - 0.90f) / 0.10f;
        float rnd = (float)random8(120, 255) / 255.0f;
        uint8_t r = (uint8_t)(255 * pulseY * rnd);
        uint8_t g = (uint8_t)((130 + t * 125) * pulseY * rnd);
        uint8_t b = (uint8_t)((t * 50) * pulseY * rnd);
        color = CRGB(r, g, b);
      }

      if (lavaIntensity < 1.0f && random8() > (uint8_t)(lavaIntensity * 255.0f)) {
        bladeSet(i, CRGB::Black);
        continue;
      }
      bladeSet(i, color);
    }

    if (random8() < 12) {
      int pos = HILT_LEDS + random16(BLADE_PIXELS);
      bladeSet(pos, bladeGet(pos) + CRGB(random8(30, 90), random8(5, 30), 0));
    }
  }

  // ── 2. Swing Fireball ─────────────────────────────────────────────────
  #define CHARGE_MS 120
  static uint32_t lastTriggerMs = 0;
  float dSwing = swingMag - prevSwingMag;
  prevSwingMag = swingMag;

  // ── SUDDEN STOP DETECTION ─────────────────────────────────────────────
  static uint32_t lastStopDetectMs = 0;
  if (swingMag > 50.0f && dSwing < -15.0f && (currSwing < 40.0f) && (millis() - lastStopDetectMs > 500)) {
    stopFlashMs      = millis();
    diagStopMs       = millis();
    lastStopDetectMs = millis();
    lastTriggerMs    = millis() + 450;
    lavaIntensity    = 0.4f;
    lastLavaInt      = 0.0f;
    for (int i = HILT_LEDS; i < BLADE_LENGTH; i++) lava[i] = (float)random8(20, 60) / 100.0f;
    fireCharge = false; fireLive = false; fireCompressing = false;
    memset(fireTrail, 0, sizeof(fireTrail));
  }

  bool swingPredict = (swingMag > 90.0f && dSwing > 25.0f);
  bool swingStrong  = (swingMag > 260.0f);
  bool animateDone  = !fireCharge && !fireLive && !fireCompressing;

  if ((swingPredict && !fireCharge) || (swingStrong && animateDone && (millis() - lastTriggerMs > 350))) {
    lastTriggerMs = millis();
    fireCharge = true; chargeStart = millis();
    fireLive = false; fireCompressing = false;
    trailMaxHalf = 0.0f; lavaIntensity = 0.4f; lastLavaInt = 0.0f; hiltFlash = 1.0f;
    for (int i = HILT_LEDS; i < BLADE_LENGTH; i++) lava[i] = (float)random8(20, 60) / 100.0f;
    memset(fireTrail, 0, sizeof(fireTrail));
  }

  if (fireCharge && !fireLive && (millis() - chargeStart > CHARGE_MS)) {
    fireCharge = false; fireLive = true; fireStartMs = millis();
    fireSpeed = 0.009f + swingMag * 0.00002f;
    if (fireSpeed > 0.016f) fireSpeed = 0.016f;
  }

  // ── CHARGE PHASE ──────────────────────────────────────────────────────
  if (fireCharge) {
    float chargeT = (float)(millis() - chargeStart) / (float)CHARGE_MS;
    int chargeWidth = 12 + (int)(chargeT * 8);
    for (int j = 0; j < chargeWidth; j++) {
      int idx = HILT_LEDS + j;
      if (idx >= BLADE_LENGTH) break;
      float falloff = 1.0f - (float)j / chargeWidth;
      float twinkle = random8(160, 255) / 255.0f;
      if (j < 5) bladeSet(idx, CRGB(255, (uint8_t)(220 * falloff * twinkle), 0));
      else       bladeSet(idx, CRGB((uint8_t)(255 * falloff * twinkle), (uint8_t)(70 * falloff * falloff * twinkle), 0));
    }
  }

  // ── LIVE PHASE ────────────────────────────────────────────────────────
  if (fireLive) {
    float elapsed  = (float)(millis() - fireStartMs);
    float midPoint = (float)BLADE_PIXELS * 0.50f;
    float fastMs   = midPoint / (fireSpeed * 60.0f);
    float pos;
    if (elapsed <= fastMs) pos = elapsed * fireSpeed * 60.0f;
    else {
      float t2 = (elapsed - fastMs) / (fastMs * 4.0f);
      t2 = fminf(t2, 1.0f);
      pos = midPoint + (float)BLADE_PIXELS * 0.50f * sqrtf(t2);
    }
    pos += (float)HILT_LEDS;

    if (pos >= (float)(BLADE_LENGTH - 1)) {
      fireLive = false; fireCompressing = true; compressStartMs = millis();
    } else {
      int center = (int)pos;
      float fbPulse  = 0.70f + 0.30f * sinf((float)millis() * 0.0785f);
      float fbStrobe = 0.60f + 0.40f * sinf((float)millis() * 0.1257f);
      for (int j = 0; j < 40; j++) {
        int idx = center - j;
        if (idx < HILT_LEDS || idx >= BLADE_LENGTH) continue;
        
        if (j > 8) {
          float gapChance = (float)(j - 8) / 32.0f;
          if (random8() < (uint8_t)(gapChance * 200)) {
            continue;
          }
        }

        uint8_t rHue = random8(35);
        uint8_t hue = (rHue < 20) ? rHue : (256 - (rHue - 19));
        float brightness = 1.0f - ((float)j / 40.0f); 
        CRGB color;

        if (j < 6) {
          float twinkle = random8(180, 255) / 255.0f;
          float intensity = brightness * twinkle * fbStrobe;
          color = CHSV(hue, random8(120, 200), (uint8_t)(intensity * 255));
        } else {
          float twinkle = random8(100, 255) / 255.0f;
          float intensity = brightness * twinkle * fbPulse;
          color = CHSV(hue, random8(240, 255), (uint8_t)(intensity * 255));
        }

        if (color) { 
           bladeSet(idx, color); 
           fireTrail[idx] = 1.0f; 
           trailHue[idx] = hue;
        }
      }
    }
  }

  // ── COMPRESSION PHASE ─────────────────────────────────────────────────
  if (fireCompressing) {
    float compressT = (float)(millis() - compressStartMs) / 500.0f;
    if (millis() - compressStartMs > 500) { fireCompressing = false; }
    else {
      int center = BLADE_LENGTH - 1;
      int width  = (int)(30.0f * (1.0f - compressT));
      for (int j = 0; j < width; j++) {
        int idx = center - j;
        if (idx < HILT_LEDS) continue;
        float p = (float)j / (float)(width > 0 ? width : 1);
        CRGB color;
        if (p < 0.33f)      color = CRGB(255, (uint8_t)(220 * (1.0f - p*3)), 0);
        else if (p < 0.66f) { float t = (p - 0.33f) * 3.0f; color = CRGB(255, (uint8_t)(110 * (1.0f - t)), 0); }
        else                 { float t = (p - 0.66f) * 3.0f; color = CRGB((uint8_t)(200 * (1.0f - t)), 0, 0); }
        bladeSet(idx, bladeGet(idx) + color);
      }
    }
  }

  // ── TRAIL RENDERING ───────────────────────────────────────────────────
  if (fireActive && !strikeBlocking && (millis() - compressStartMs > 500)) memset(fireTrail, 0, sizeof(fireTrail));
  trailMaxHalf = 0.0f;
  if (fireActive) {
    for (int i = HILT_LEDS; i < BLADE_LENGTH; i++) {
      if (fireTrail[i] > 0.01f) {
        float posFrac = (float)(i - HILT_LEDS) / (float)BLADE_PIXELS;
        float decayRate = (0.84f + posFrac * 0.10f);
        if (swingMag < 60.0f || fireTrail[i] < 0.70f) decayRate *= 0.85f;
        fireTrail[i] *= decayRate;
        if (i < (HILT_LEDS + BLADE_PIXELS / 2)) { if (fireTrail[i] > trailMaxHalf) trailMaxHalf = fireTrail[i]; }
        float sparkle = 0.35f + (float)random8(165) / 255.0f;
        uint8_t bri = (uint8_t)(fireTrail[i] * 240.0f * sparkle);
        if (bri > 5) { 
           uint8_t sat = 210 + random8(45); 
           uint8_t rHue = random8(35);
           uint8_t hue = (rHue < 20) ? rHue : (256 - (rHue - 19));
           bladeSet(i, bladeGet(i) + CHSV(hue, sat, bri)); 
        }
      }
    }
  }

  // ── Thrust / Pull ─────────────────────────────────────────────────────
  static float prevAY = 0.0f;
  float dAY = accelY - prevAY; prevAY = accelY;
  if (fabsf(dAY) > 0.5f && swingMag < 80.0f && !fireLive && !fireCharge) {
    fireCharge = true; chargeStart = millis(); fireSpeed = 0.50f; hiltFlash = 1.0f;
  }

  // ── PERMANENT HILT/BASE FIRE ──────────────────────────────────────────
  {
    static float hiltPhase = 0.0f, hiltLenPhase = 0.0f;
    hiltPhase += 0.045f; hiltLenPhase += 0.018f;
    float lenOsc  = 7.0f * sinf(hiltLenPhase) + 3.5f * sinf(hiltLenPhase * 2.3f + 0.7f);
    int   fireLen = constrain((int)(22.0f + lenOsc), 14, 30);
    for (int j = 0; j < fireLen; j++) {
      int idx = HILT_LEDS + j; if (idx >= BLADE_LENGTH) break;
      float falloff = 1.0f - (float)j / (float)fireLen;
      float pulse   = 0.80f + 0.20f * sinf(hiltPhase + (float)j * 0.45f);
      float twinkle = random8(190, 255) / 255.0f;
      CRGB hiltColor;
      if (j < 5)       hiltColor = CRGB(255, (uint8_t)(200 * falloff * pulse * twinkle), 0);
      else if (j < 14) hiltColor = CRGB((uint8_t)(255 * falloff * pulse), (uint8_t)(70 * falloff * falloff * twinkle), 0);
      else { uint8_t r = (uint8_t)(200 * falloff * falloff * pulse * twinkle); hiltColor = CRGB(r, 0, 0); }
      bladeSet(idx, bladeGet(idx).lerp8(hiltColor, (uint8_t)(falloff * 230)));
    }
  }

  // ── SUDDEN STOP FLASH ─────────────────────────────────────────────────
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
        bladeSet(i, bladeGet(i) + CRGB(255,255,255).nscale8((uint8_t)(intensity * 255.0f)));
      }
    }
  }

  // ── Quick Stop & Fade ─────────────────────────────────────────────────
  if (lastSwing > 180.0f && swingMag < 80.0f) fizzle = 1.0f;
  if (fizzle > 0.01f) {
    for (int i = 0; i < BLADE_LENGTH; i++) {
      if (random8() < 50 * fizzle) { CRGB c = bladeGet(i); c.nscale8(180); bladeSet(i, c); }
    }
    fizzle *= 0.92f;
  }
  lastSwing = swingMag;

  static uint32_t lastPrint = 0;
  if (millis() - lastPrint > 500) {
    Serial.printf("FIRE | aX:%.2f aY:%.2f aZ:%.2f | swing:%.1f twist:%.1f\n", accelX, accelY, accelZ, swingMag, twistRate);
    lastPrint = millis();
  }
}
