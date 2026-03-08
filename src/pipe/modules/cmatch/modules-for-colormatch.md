# vkdt Modules for Color Matching Raw → Embedded JPEG

## Problem Statement

Camera JPEGs apply hue-specific saturation boosts, tone curves, and color
rendering that raw processors don't replicate out of the box. This document
catalogs every built-in module that can help close that gap, organized by
how useful each is for the color matching task.

---

## Tier 1: Directly Useful for Color Matching

### curves (YCh mode) — Hue-selective chroma/hue control
**What it does:** Cubic spline curves in oklab-derived YCh space. The 3x3
matrix of curve combinations (Y/C/h × Y/C/h) gives 9 independent controls.

**Key curves for color matching:**
- **C/h** (curve 7): Boost chroma at specific hues (e.g. reds for lips/skin)
- **h/h** (curve 2): Rotate hues (e.g. shift yellow-greens toward blue-green)
- **C/Y** (curve 4): Luminance-dependent saturation (camera JPEGs often
  boost midtone saturation more than shadows/highlights)

**cfg usage:** Set `mode:2` for YCh, `ych 3x3:7` to select C/h curve.
vtx7 layout is `[x0..x5, y0..y5]` where x = hue position (0-1 periodic),
y = additive chroma offset. abcd coefficients are computed automatically.

**Example (boost reds, mild green boost):**
```
param:curves:01:mode:2
param:curves:01:ych 3x3:7
param:curves:01:vtx7:0.083:0.25:0.417:0.583:0.75:0.917:0.18:0.02:0.06:0:0:0.12
```

**Proven:** v17 test showed C/h curve is the single most effective tool for
matching JPEG lip/skin color without affecting the whole image.

### filmcurv — Tone curve with color reconstruction
**What it does:** Weibull CDF parametric tone curve with 5 color modes.

**Key finding:** `colour:1` (per-channel) preserves saturation MUCH better
than `colour:3` (HSV/DNG). Per-channel applies the curve to R,G,B
independently, which naturally preserves channel ratios = saturation.
HSV mode uses `adjust_colour_dng` which constrains the middle channel
between min/max, actively compressing chroma for vivid colors.

**Color modes ranked for JPEG matching:**
1. **Per-channel (1)** — best saturation preservation, slight hue shifts possible
2. **DT UCS (0)** — perceptually uniform, good colors, slight desaturation
3. **Munsell (2)** — data-driven hue constancy, similar to DT UCS
4. **HSV/DNG (3)** — default, robust near black, but desaturates vivid colors
5. **AgX (4)** — film-like rendering, tends to desaturate

**Contrast parameter** also matters: camera JPEGs typically have contrast
~1.1-1.2 vs vkdt default of 1.0.

### colour — Input transform + RBF color mapping
**What it does:** Camera RGB → rec2020 conversion, white balance, exposure,
saturation, and optional RBF (radial basis function) arbitrary color mapping.

**Key parameters for matching:**
- `sat`: Global saturation multiplier (1.2 gets ~80% there)
- `white`: White balance multipliers (affects overall color cast)
- `mode:1` + `rbmap`: Thin-plate spline RBF with up to 24 src→tgt pairs
  in rec2020 linear. TPS solve happens automatically in commit_params().

**RBF usage from cfg:**
```
param:colour:01:mode:1
param:colour:01:cnt:3
param:colour:01:rbmap:src_r:src_g:src_b:tgt_r:tgt_g:tgt_b:...
```
Values must be rec2020 linear. Best used with the pick module to sample
actual colors from both images.

### grade — Lift/Gamma/Gain in RGB
**What it does:** ASC CDL-style 3-point color grading. Lift affects shadows,
gamma affects midtones (power curve), gain affects highlights.

**For matching:** Subtle gain adjustments like `1.02:0.98:1.01` can warm the
overall palette to match camera JPEG white balance differences that the
colour module's WB doesn't fully capture.

**Limitation:** Operates in linear RGB, affects all hues equally. Can produce
out-of-gamut colors.

### mask — Parametric hue/saturation/luminance masking
**What it does:** Creates single-channel masks based on pixel properties.

**Modes:**
- 0: luminance (log)
- 1: **hue** — most useful for color matching
- 2: **saturation**
- 3: Bayer luminance

**For matching:** Create a hue mask selecting reds/skin tones, then feed it
to a blend module to selectively apply a correction (e.g. saturation boost)
only to warm colors. This replicates camera JPEG hue-specific processing.

**Pipeline example:**
```
colour → filmcurv → mask:skin (hue mode, select reds)
                  → grade:skin (boost saturation)
                  → blend:skin (back=filmcurv, input=grade:skin, mask=mask:skin)
```

### blend — Layer composition with mask support
**What it does:** Composites two images with opacity, blend mode, and
optional mask.

**Blend modes:**
- 0: over (alpha blend) — most useful
- 1: TAA (temporal)
- 2: focus stack
- 3: multiply

**For matching:** Use mode 0 (over) with a mask from the mask module to
apply selective corrections. `opacity` controls strength.

### loss — Image difference metric
**What it does:** Computes L2 loss between processed image and reference
target. GPU→CPU download pipeline for iterative optimization.

**For matching:** Connect your pipeline output and the embedded JPEG as
reference. The loss value tells you how close the match is. Could be used
to programmatically optimize parameters.

**Pipeline:**
```
grade:01:output → loss:01:input
i-jpg:01:output → loss:01:orig
```

---

## Tier 2: Supplementary / Situational

### llap — Local Laplacian pyramids (shadows/highlights/clarity)
**What it does:** Edge-aware shadow/highlight recovery and clarity control.

**For matching:** Camera JPEGs often have shadow lifting and highlight
compression. `shadows` < 1 darkens shadows (or > 1 lifts), `hilights`
compresses bright areas. `clarity` adds local contrast.

### zones — Ansel Adams zone system
**What it does:** Divides image into N luminance zones, applies per-zone
EV compensation.

**For matching:** Camera JPEGs apply tone curves that differ by luminance
range. Zones can approximate this: boost zone 3-4 (midtones) while
leaving shadows/highlights alone.

**Limitation:** Luminance only, no color control.

### eq — Wavelet local contrast equalizer
**What it does:** Per-frequency-band contrast control (6 bands from coarse
to fine).

**For matching:** Camera JPEGs have specific sharpening/clarity at different
spatial frequencies. The eq module can replicate this.

### dehaze — Dark channel prior dehazing
**What it does:** Removes atmospheric haze. The `haze` color parameter
specifies what color to remove.

**For matching:** Could help if the raw output looks "washed out" compared
to the JPEG's punchy contrast, especially in outdoor/landscape shots.

### OpenDRT — Open Display Rendering Transform
**What it does:** Complete cinematic DRT with 86+ parameters for tone
mapping, gamut mapping, hue shifts, purity control, brilliance.

**For matching:** Has hue shift controls (`hs_r`, `hs_g`, `hs_b`, etc.)
and purity (saturation) compression. Could potentially replace filmcurv
for more control, but it's complex and designed for cinematic rendering,
not JPEG matching.

**Key creative params:**
- `brl_*`: Brilliance per-channel (high-purity stimulus scaling)
- `hs_*`: Hue shift angles for primaries and secondaries
- `pt_lml/lmh`: Purity limits per hue

### filmsim — Spectral analog film simulation
**What it does:** Physically-based film emulation with DIR coupler
interactions that produce hue-specific saturation boosts — very similar
to what camera JPEGs do.

**For matching:** DIR couplers naturally boost saturation in reds/skin tones
(because the red-sensitive film layer inhibits neighboring green/blue layers
when strongly exposed). This is analogous to camera JPEG color rendering.

**Parameters:**
- `film`: 15 film stocks (Portra 400, Gold 200, Ultramax 400, etc.)
- `couplers`: 0-1, controls hue-specific saturation (0.5 is a good starting point)
- `paper`: 7 print paper stocks
- `filter c/m/y`: enlarger color filters for white balance
- `tune m/y`: fine color balance tweaks

**Note:** Requires i-lut modules for filmsim.lut and spectra-em.lut data.
Replaces filmcurv in the pipeline (see filmsim.pst preset).

---

## Tier 3: Diagnostic / Infrastructure

### pick — Color sampler (up to 24 spots)
Samples colors from image regions. Feeds into colour module's RBF import.
Shows CIE DE76 error vs reference values.

### ciediag — CIE chromaticity diagram
Plots pixel colors on xy chromaticity diagram. Shows gamut violations.
Useful for visualizing how pipeline colors differ from JPEG.

### ab — A/B split comparison
Side-by-side comparison of two pipeline branches. Good for visual
comparison of raw vs JPEG.

### hist — Histogram display
Standard RGB histogram.

### check — Gamut/exposure validation
Marks out-of-gamut or clipped pixels with false color overlay.

### const — Constant color generator
Generates solid color buffers. Used internally by pick module freeze.

### resize/format — Image scaling and pixel format conversion
Infrastructure modules, no color effect.

---

## Proven Best Recipe (from testing)

After testing 18 variants with CLI export and visual comparison:

```
filmcurv:colour    1 (per-channel)     # biggest single improvement
filmcurv:contrast  1.1                 # match JPEG punch
colour:sat         1.2                 # mild global boost
grade:gain         1.02:0.98:1.01      # subtle warm shift
curves:mode        2 (YCh)
curves:vtx7 (C/h)  +0.18 at reds, +0.12 at magentas, +0.06 at greens
```

### What each parameter does to close the gap:

| Change | Contribution | Mechanism |
|--------|-------------|-----------|
| per-channel filmcurv | ~40% | Preserves RGB ratios through tone curve |
| C/h curve boost | ~25% | Hue-selective chroma (lips, skin, greens) |
| sat 1.2 | ~15% | Global chroma lift |
| grade warm gain | ~10% | Overall color temperature shift |
| contrast 1.1 | ~10% | More midtone punch |

### Remaining gap (~15% from perfect match):
- Background green hue: still slightly too yellow (needs h/h curve shift)
- Highlight rendering: JPEG has smoother highlight rolloff
- Shadow color: JPEG shadows are slightly warmer

### To close the last 15%:
1. Use h/h curve (curve 2) to rotate greens toward cyan
2. Use C/Y curve (curve 4) for luminance-dependent saturation
3. Or use cmatch module's 3D LUT for automatic per-pixel matching

---

## Pipeline Combinations

### Basic (current best — no custom modules)
```
i-raw → denoise → hilite → demosaic → crop → colour → filmcurv → llap → curves → grade → display
```

### With selective mask correction
```
filmcurv → mask:skin (hue=reds) + grade:warm (sat boost) + blend:skin
         → llap → curves → grade → display
```

### With loss feedback (for optimization)
```
grade:01:output → loss:01:input
i-jpg:01:output → resize → loss:01:orig
loss:01:loss → (read back to CPU for optimization)
```

### With cmatch (automatic matching)
```
filmcurv → cmatch (input + JPEG target) → display
```
