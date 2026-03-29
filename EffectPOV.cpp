#include "Config.h"

// =====================================================================
// EFFECT: PERSISTENCE OF VISION OVERLAY
// =====================================================================
//
// PHYSICS MODEL
// ─────────────
// Pivot point  : ~0 cm = hilt base (where the IMU lives).
// Gyro output  : swingMag   = √(gx²+gy²) = transverse angular velocity (°/s)
//                twistRate  = gz           = axial roll rate (°/s)
//
// Angle integration
//   Every frame: povAngle  +=  swingMag * dt   (degrees swept since sweep began)
//
// World-space locking (the key to POV)
//   A feature hangs in the air when its on-condition is a pure function of
//   povAngle (world angle), NOT of time.  At each frame the blade occupies
//   exactly one angular slice; pixels lit only when that slice is "inside"
//   a pattern column will appear stationary across the arc.
//
// Per-pixel arc-length scaling
//   A pixel at bladePos i is at radius  r(i) = (i - HILT_LEDS) / BLADE_PIXELS
//   measured from the pivot.  The farther from the pivot, the more arc it
//   sweeps per degree.  We apply a mild radial scale so patterns have
//   consistent angular width regardless of where on the blade you look.
//
// =====================================================================

// ─── SHARED POV STATE ─────────────────────────────────────────────────
static float    povAngle  = 0.0f;   // integrated swing angle (degrees)
static float    povPeak   = 0.0f;   // peak swingMag this sweep
static bool     povActive = false;  // true during an active swing

// ─── CONSTANTS ────────────────────────────────────────────────────────
static const float DT          = 0.016f;  // 60 fps frame time (s)
static const float ENTER_SPEED = 55.0f;   // deg/s to enter POV mode
static const float EXIT_SPEED  = 35.0f;   // deg/s hysteresis exit level

// ─── HELPERS ──────────────────────────────────────────────────────────

// Smooth square wave, returns 0..1.  phase=0..1, duty=fraction that is "on",
// edge sharpness controlled by edge (0=sharp, 0.15=soft).
static inline float squareWave(float phase, float duty, float edge) {
    float lo  = duty - edge;
    float hi  = duty + edge;
    if (edge < 0.001f) return (phase < duty) ? 1.0f : 0.0f;
    if (phase < lo)   return 1.0f;
    if (phase > hi)   return 0.0f;
    return 1.0f - (phase - lo) / (hi - lo);
}

// Wrap angle (degrees) into [0, period).
static inline float wrapDeg(float angle, float period) {
    angle = fmodf(angle, period);
    if (angle < 0.0f) angle += period;
    return angle;
}

// Radial fraction for a blade pixel (0 = hilt boundary, 1 = tip).
static inline float radFrac(int bladePos) {
    return (float)(bladePos - HILT_LEDS) / (float)BLADE_PIXELS;
}

// =====================================================================
// updatePOV() — call once per loop(), before effect dispatch
// =====================================================================
void updatePOV() {
    if (swingMag > ENTER_SPEED) {
        // Active swing: integrate angle
        povAngle += swingMag * DT;
        povPeak   = fmaxf(povPeak, swingMag);
        povActive = true;
    } else if (povActive && swingMag > EXIT_SPEED) {
        // Hysteresis: keep integrating at reduced speed so patterns don't
        // snap off the moment the wrist decelerates slightly.
        povAngle += swingMag * DT;
        povPeak  *= 0.97f;
    } else {
        // Swing stopped — slowly drain the angle, reset on full stop.
        povAngle *= 0.80f;
        povPeak  *= 0.88f;
        if (povPeak < 20.0f) {
            povActive = false;
            povAngle  = 0.0f;
        }
    }
}

// =====================================================================
// renderPOVOverlay(mode) — call AFTER the base effect, BEFORE FastLED.show()
// =====================================================================
// The overlay blends on top of whatever the base effect painted.
// blend factor scales with intensity so at slow swings the base effect
// still shows through.

void renderPOVOverlay(uint8_t mode) {
    static uint32_t lastPovEndMs = 0;
    static bool     wasPovActive = false;
    static uint8_t  currentRainbowHue = 0;
    static uint32_t strikeStartMs = 0;

    // Track state transitions for per-swing effects
    if (povActive && !wasPovActive) {
        currentRainbowHue += random8(40, 90);  // Shift hue for requested single-color rainbow slash
        strikeStartMs = millis();
    } else if (!povActive && wasPovActive) {
        lastPovEndMs = millis();
    }
    wasPovActive = povActive;

    if (!povActive) {
        // ── Post-Strike Effects (when sword stops moving) ──
        if (mode == MODE_LIGHTNING) {
            uint32_t stoppedTime = millis() - lastPovEndMs;
            if (stoppedTime < 800) {  // Crackle for 800ms after swing stops
                if (random8() < (uint8_t)(30.0f * (1.0f - stoppedTime / 800.0f))) {
                    int start = HILT_LEDS + random16(BLADE_PIXELS - 15);
                    int len   = random8(4, 15);
                    for (int i = start; i < start + len; i++) {
                        bladeSet(i, CRGB(180, 220, 255));
                    }
                }
            }
        }
        return;
    }

    // Intensity: 0 at 55 deg/s, 1.0 at 250 deg/s.  Clamp both ends.
    float intensity = constrain((povPeak - ENTER_SPEED) / 200.0f, 0.0f, 1.0f);
    if (intensity < 0.05f) return;

    // ── Overlay alpha: how hard the POV pattern overrides the base effect ──
    // At low intensity it blends gently; at full intensity it dominates.
    uint8_t overrideAlpha = (uint8_t)(intensity * intensity * 220.0f);

    switch (mode) {

    // ================================================================
    // MODE 0 — FIRE POV (Slash Trails)
    // ================================================================
    // What you see in the air: a thick, sweeping crescent of fire.
    // As you sweep, the physical tip (which covers the most physical 
    // area) burns white-hot, while the base stays dark. The instantaneous 
    // swing speed defines the overall heat (so the start and end of the 
    // slash taper off naturally), and an angle-locked noise texture 
    // tears the trail into static "flame tongues" hanging in the air.
    // ================================================================
    case MODE_FIRE: {
        for (int i = HILT_LEDS; i < BLADE_LENGTH; i++) {
            float rf = radFrac(i); // 0 at hilt, 1 at tip

            // 1. Base heat is driven by how fast the sword is CURRENTLY moving.
            // This creates a natural taper at the start and end of the slash.
            float baseHeat = constrain((swingMag - 40.0f) / 200.0f, 0.0f, 1.0f);

            // 2. The tip gets much hotter and brighter as requested,
            // mapping strongly away from the base.
            float tipWeight = rf * rf; 
            float heat = baseHeat * (0.2f + 0.8f * tipWeight);

            // 3. Angle-locked flame texture.
            // We map a fast-changing sine wave combination across the world angle
            // so the trail appears textured and torn instead of perfectly smooth.
            float flameTex = sinf(povAngle * 0.4f + rf * 8.0f) * 0.5f + 
                             sinf(povAngle * 0.9f - rf * 16.0f) * 0.5f;
            
            heat += flameTex * 0.35f * baseHeat; 
            heat = constrain(heat, 0.0f, 1.0f);

            if (heat < 0.08f) {
                // Dark background: dim the base effect but don't kill it
                CRGB c = bladeGet(i);
                c.nscale8((uint8_t)((1.0f - intensity * 0.6f) * 255.0f));
                bladeSet(i, c);
            } else {
                // Map heat -> Fire colors (Red -> Orange -> Yellow -> White-core)
                uint8_t r = 255;
                uint8_t g = (uint8_t)(heat * heat * 210.0f);
                uint8_t b = (uint8_t)(fmaxf(0.0f, heat - 0.75f) * 140.0f);
                CRGB povColor(r, g, b);

                uint8_t alpha = (uint8_t)(constrain(heat * 1.5f, 0.0f, 1.0f) * overrideAlpha);
                bladeSet(i, bladeGet(i).lerp8(povColor, alpha));
            }
        }
        break;
    }

    // ================================================================
    // MODE 1 — RAINBOW POV (Single Color Slash)
    // ================================================================
    // Paints a solid, bright ribbon of light in the air that changes
    // to a completely different color on each new slash.
    // ================================================================
    case MODE_RAINBOW: {
        float activeTime = (float)(millis() - strikeStartMs) / 1000.0f;
        // Chaser heavily shoots from base to tip (loops rapidly)
        float chaserPos = fmodf(activeTime * 3.5f, 1.0f); 

        for (int i = HILT_LEDS; i < BLADE_LENGTH; i++) {
            float rf = radFrac(i); // 0 at hilt, 1 at tip

            float baseHeat = constrain((swingMag - 40.0f) / 200.0f, 0.0f, 1.0f);
            float tipWeight = rf * rf; 
            float heat = baseHeat * (0.2f + 0.8f * tipWeight);

            // Add smooth texture so it looks dynamic in the air
            float tex = sinf(povAngle * 0.3f + rf * 5.0f) * 0.5f + 
                        sinf(povAngle * 0.7f - rf * 10.0f) * 0.5f;
            heat += tex * 0.25f * baseHeat; 

            // ── The Energy Chaser ──
            float distToChaser = fabsf(rf - chaserPos);
            // Sharp brilliant core shooting up the physical blade
            float chaserHeat = fmaxf(0.0f, 1.0f - distToChaser * 7.0f); 
            heat += chaserHeat * 1.8f * baseHeat; 
            heat = constrain(heat, 0.0f, 1.0f);

            if (heat < 0.08f) {
                // Dim background
                CRGB c = bladeGet(i);
                c.nscale8((uint8_t)((1.0f - intensity * 0.6f) * 255.0f));
                bladeSet(i, c);
            } else {
                // ── Fully 3D cylindrical depth effect ──
                // Mapping a cross-section highlight over the entire blade
                float cylinderDepth = sinf(rf * 3.14159f); // 0 ends, 1 middle
                
                uint8_t bri = (uint8_t)(heat * heat * 255.0f);
                // Core of the chaser AND middle of the cylinder get the white highlight
                uint8_t sat = 255 - (uint8_t)(constrain(heat * 90.0f + cylinderDepth * 40.0f, 0.0f, 255.0f)); 
                
                uint8_t hue = currentRainbowHue + (uint8_t)(rf * 30.0f); // Wrap gradient

                // Edge shading for the 3D volume
                float volShade = 0.6f + 0.4f * cylinderDepth;
                bri = (uint8_t)(bri * volShade);

                CRGB povColor = CHSV(hue, sat, bri);

                uint8_t alpha = (uint8_t)(constrain(heat * 1.5f, 0.0f, 1.0f) * overrideAlpha);
                bladeSet(i, bladeGet(i).lerp8(povColor, alpha));
            }
        }
        break;
    }

    // ================================================================
    // MODE 2 — LIGHTNING POV (Jagged Electric Arc)
    // ================================================================
    // Creates a searing, highly jagged path that mimics a lightning 
    // bolt ripped through the air by the blade. Produces sharp spikes
    // of brightness and lingering crackles when stopped.
    // ================================================================
    case MODE_LIGHTNING: {
        for (int i = HILT_LEDS; i < BLADE_LENGTH; i++) {
            float rf = radFrac(i); 

            float baseHeat = constrain((swingMag - 40.0f) / 200.0f, 0.0f, 1.0f);
            float tipWeight = rf * rf; 
            float heat = baseHeat * (0.2f + 0.8f * tipWeight);

            // High frequency, sharp texture to mimic rapid lightning zags
            float tex = sinf(povAngle * 1.5f + rf * 20.0f) * 0.5f + 
                        sinf(povAngle * 2.8f - rf * 35.0f) * 0.5f;

            // Square the texture to make it sharp and spiky instead of smooth
            float sharpTex = powf(fmaxf(0.0f, tex + 0.5f), 3.0f);
            heat += sharpTex * 0.6f * baseHeat; 
            heat = constrain(heat, 0.0f, 1.0f);

            if (heat < 0.15f) {
                CRGB c = bladeGet(i);
                c.nscale8((uint8_t)((1.0f - intensity * 0.8f) * 255.0f));
                bladeSet(i, c);
            } else {
                // Deep blue halo -> Cyan sub-core -> Absolute White core
                uint8_t r = (uint8_t)(heat * heat * heat * 200.0f);
                uint8_t g = (uint8_t)(heat * heat * 230.0f);
                uint8_t b = 255;
                CRGB povColor(r, g, b);

                uint8_t alpha = (uint8_t)(constrain(heat * 2.0f, 0.0f, 1.0f) * overrideAlpha);
                bladeSet(i, bladeGet(i).lerp8(povColor, alpha));
            }
        }
        break;
    }

    default:
        break;
    }
}
