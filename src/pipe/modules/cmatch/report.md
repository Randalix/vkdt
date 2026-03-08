# cmatch module — color matching via embedded JPEG

## Goal

Match the raw pipeline output colors to the camera's embedded JPEG by computing
a color transform (LUT) from corresponding pixel pairs.

## Current approach (working): oklab chroma-only 3D LUT

### Concept
A 17x17x17 3D lookup table indexed by input sRGB display RGB. Each cell stores
oklab chroma deltas (a,b only — NOT luminance L). This means the correction only
affects color/hue, not brightness, so upstream parameter changes (exposure, contrast,
etc.) pass through without the image "falling apart."

### How it works
1. **build.comp**: For each LUT cell, scan 128x128 UV grid. Convert both source
   (filmcurv output) and target (embedded JPEG) to oklab via `srgb_eotf → linear_srgb_to_oklab`.
   Accumulate weighted `(tgt.ab - src.ab)` chroma deltas. Store `vec4(0, delta_a, delta_b, 1)`.
2. **apply.comp**: Convert input pixel to sRGB display space for LUT indexing.
   Trilinear lookup returns oklab a,b deltas. Convert input to oklab, apply deltas
   (scaled by strength param) to a,b channels only, convert back to linear sRGB,
   then to rec2020 pipeline space.

### Why chroma-only
Full RGB deltas (previous version) worked well for static exports but broke when
the user adjusted exposure or other parameters in the GUI — the LUT fought the
user's changes. Oklab chroma-only deltas preserve the pipeline's luminance while
applying the JPEG's color character (hue shifts, saturation differences).

### LUT format
- `r` = 0 (unused)
- `g` = oklab a delta (green-red axis)
- `b` = oklab b delta (blue-yellow axis)
- Empty cells = zero delta = identity (no correction)

### Layout
The 3D LUT is stored as a 2D image: `(17*17) x 17` = `289 x 17` pixels in rgba:f16.
- x = R_index + B_index * 17
- y = G_index
- Trilinear lookup: bilinear via texture sampler on R,G axes, manual interpolation on B axis.

### Sampling
Each LUT cell scans a 128x128 grid of UV samples from both images. Samples within
`cell_radius = 1.5 / (LUT_N - 1)` contribute with Gaussian weight `exp(-3 * d^2)`.

### Display-space indexing
Both build and apply convert pipeline values to sRGB display space for LUT indexing:
`srgb_oetf(clamp(rec2020_to_rec709 * pipeline_rgb, 0, 1))`. The JPEG is already in
sRGB. This ensures the LUT is indexed consistently regardless of pipeline internals.

## Key findings

### Matrix multiplication convention
vkdt uses `M * rgb` (column-vector) for matrix-vector multiplication in GLSL, confirmed
by `colenc/main.comp:28`. The `makemat` macro in `matrices.h` transposes from row-major
(C convention) to column-major (GLSL convention). Note: `i-vid/conv.comp` uses
`rgb *= M` (row-vector), which applies the transposed matrix — this appears to be a
bug in that module but is not our concern.

### Why absolute target values failed
The filmcurv output (Weibull CDF tone curve) and the inverse-colenc target (sRGB EOTF
linearized) are in fundamentally different transfer functions. For 18% gray:
- filmcurv output: ~0.4-0.5 (gamma-like, display-referred)
- inverse colenc of JPEG: ~0.17 (linear)

Both are correct — colenc will apply sRGB OETF to the linear value, producing the right
display output. But empty LUT cells (identity = cell_center in filmcurv space) are much
brighter numerically than populated cells (linear space). Trilinear interpolation between
them produces incorrect values at boundaries.

## Files

- `build.comp` — LUT construction: oklab chroma delta accumulation per cell
- `apply.comp` — LUT application: trilinear lookup of chroma deltas + oklab round-trip
- `main.c` — graph wiring: build → apply (two compute nodes)
- `scatter.comp` — STALE, unused (failed atomic scatter approach)

## Previous approaches (failed)

### 1. Atomic scatter
Dispatch at image resolution, atomicAdd into LUT cells. CAS-loop on M1/MoltenVK
is unreliable — R,G,B,weight accumulations get out of sync.

### 2. Absolute target values (multiple variants)
Direct LUT fill without atomics, one thread per cell. Every variant of target
conversion (sRGB EOTF, bt.709 EOTF, perceptual rec2020, raw JPEG) produced
sepia/desaturated output due to the linear/nonlinear domain mismatch between
populated and empty cells.

### 3. Spatial delta approach (worked but superseded)
Downsample + per-pixel oklab delta + blur + apply. Works well but is a different
architecture. Files were `down.comp` + blur + old `apply.comp`.

### 4. Display-space RGB delta LUT (worked for static, broke in GUI)
Same 3D LUT architecture but storing full RGB deltas `(tgt_display - src_display)`
in sRGB display space. Produced good CLI exports but when the user adjusted exposure
or other parameters in the GUI, the LUT fought the changes — the image "fell apart."
Superseded by the oklab chroma-only approach.
