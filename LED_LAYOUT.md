# Fullsword — LED Layout Reference

## Physical Overview

The sword is a **single continuous WS2812B strip** running from the base of the
hilt all the way to the blade tip.  The IMU (MPU-9250) is mounted at the
**bottom** of the hilt, so the hilt/base end is always at physical index 0 of
the strip.

```
[IMU]
  │
  ▼  strip direction (data flows hilt → tip)
╔════╦══════════════════════════════════════════════════════════╗
║ A  ║  H  H  H  H  H  H  H  H  H  H  H  H  H  H  H | blade…║
╚════╩══════════════════════════════════════════════════════════╝
 0123  4  5  6  7  8  9 10 11 12 13 14 15 16 17 18    …    183
       └─────────── HILT_LEDS (15) ──────────────┘
```

Legend: **A** = Accent LEDs · **H** = Hilt LEDs · `|` = hilt/blade boundary

---

## `leds[]` Array Segments

| Segment | `leds[]` indices | Count | Purpose |
|---|---|---|---|
| **Accent / Base** | `leds[0..3]` | 4 | Decorative base accent; pulse with `baseHue`. Always written directly, never through `bladeSet`. |
| **Hilt** | `leds[4..18]` | 15 | Lowest 15 pixels of the blade coordinate space (`bladePos 0–14`). Physically wrapped around or near the hilt guard. Part of the blade strip — effect code treats them as the "hot base" of the fire. |
| **Blade** | `leds[19..183]` | 165 | Pure blade pixels (`bladePos 15–179`). |

> **Total:** 4 accent + 180 blade-space = **184 LEDs** (`NUM_LEDS = 184`)

---

## Coordinate System — `bladeSet` / `bladeGet`

All effect code uses the helper functions defined in `Config.h`.  They add a
**fixed offset of 4** to translate a *blade position* into a raw `leds[]`
index, hiding the accent LEDs entirely:

```cpp
// Config.h
inline void bladeSet(int pos, CRGB color) {
    if (pos < 0 || pos >= BLADE_LENGTH) return;
    leds[4 + pos] = color;          // ← +4 offset skips the 4 accent LEDs
}
```

| Symbolic constant | Value | Meaning |
|---|---|---|
| `NUM_LEDS` | 184 | Total LEDs in the FastLED array |
| `BLADE_LENGTH` | 180 | Total pixels addressable by `bladeSet` (0 → 179) |
| `HILT_LEDS` | 15 | Number of blade-space pixels that are physically in the hilt region |
| `BLADE_START` | 15 | First pure-blade pixel in blade-space (`= HILT_LEDS`) |
| `BLADE_END` | 180 | One-past-the-last blade pixel (`= BLADE_LENGTH`) |
| `BLADE_PIXELS` | 165 | Count of pure-blade pixels (`BLADE_END − BLADE_START`) |

### Translating between spaces

```
leds[] index  =  bladePos + 4
bladePos      =  leds[] index − 4

Hilt region   →  bladePos  0 .. 14   (leds[4]  .. leds[18])
Blade region  →  bladePos 15 .. 179  (leds[19] .. leds[183])
```

---

## Hilt / Blade Boundary

The hilt occupies `bladePos 0–14` (the first `HILT_LEDS = 15` pixels).  This
boundary is **part of the same strip** — there is no gap or separate data line.
Effects that want to start at the true blade use `HILT_LEDS` as their loop
start:

```cpp
for (int i = HILT_LEDS; i < BLADE_LENGTH; i++) { ... }
//              ↑ 15                    ↑ 180
```

Effects that want to light the hilt separately loop from `0`:

```cpp
for (int i = 0; i < HILT_LEDS; i++) bladeSet(i, CRGB::Black);
```

---

## `bladeClear()`

Clears the entire blade-space (all 180 pixels) without touching the 4 accent
LEDs:

```cpp
inline void bladeClear() {
    fill_solid(leds + 4, NUM_LEDS - 4, CRGB::Black);
    //                        ↑ 180 pixels cleared
}
```

---

## Accent LEDs (`leds[0..3]`)

- Written **directly** to the `leds[]` array (no bladeSet offset).
- Rendered every frame by `renderAccents()` as a slow HSV pulse keyed on
  `baseHue + 20`.
- Used by `renderSyncIndicator()` to show pairing status via a cyan pulse on
  `leds[0]` and `leds[NUM_LEDS - 1]` (= `leds[183]`).
- These are the only LEDs that sit *outside* the blade coordinate space.

---

## IMU Orientation Notes

The MPU-9250 is mounted at the **bottom** of the hilt with the following axis
convention (calibrated empirically):

| Physical orientation | Key reading |
|---|---|
| Sword pointing **UP** | `accelY < −0.4 g` |
| Sword pointing **DOWN** | `accelY > +0.4 g` |
| Sword **flat/horizontal** | `accelY ≈ 0` |
| Swing (transverse) | `swingMag = √(gx² + gy²)` in deg/s |
| Twist (axial roll) | `twistRate = gz` in deg/s |

`tiltDir` is `+1` when tip-up (`accelZ > 0.2`) and `−1` when tip-down.

---

## Quick-Reference Cheat Sheet

```
Physical strip (183 = tip end, 0 = hilt/IMU end):

  leds[0]  leds[1]  leds[2]  leds[3]  | leds[4] … leds[18] | leds[19] … leds[183]
  ─────────────────────────────────────────────────────────────────────────────────
  Accent0  Accent1  Accent2  Accent3   | Hilt 0 … Hilt 14   | Blade 0 … Blade 164
                                         bladePos 0 … 14       bladePos 15 … 179
                                         ↑ HILT_LEDS = 15      ↑ BLADE_PIXELS = 165
                                         └──────── BLADE_LENGTH = 180 ────────────┘
```
