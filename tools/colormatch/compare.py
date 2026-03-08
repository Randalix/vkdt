#!/usr/bin/env python3
"""
Compare vkdt-rendered JPGs against Canon embedded JPEGs.

Usage:
  python3 compare.py <vkdt.jpg> <embedded.jpg> --samples-file samples.json [--json]
  python3 compare.py <vkdt.jpg> <embedded.jpg> [--grid NxM] [--json]

Outputs per-sample and per-hue-sector analysis in terms that map directly
to vkdt module parameters:
  - exposure difference in stops  -> colour:01:exposure
  - oklab hue position (0..1)     -> curves:01 vtx2/vtx6/vtx7 x-positions
  - oklab Y/C/h deltas per sector -> curves:01 vtx6 (Y/h), vtx7 (C/h), vtx2 (h/h)
  - saturation ratio              -> colour:01:sat
"""
import sys, json, argparse, math
from pathlib import Path
import numpy as np
from PIL import Image

# ---- Color math ----

def srgb_to_linear(c):
    c = c / 255.0
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)

def linear_to_xyz(rgb):
    M = np.array([
        [0.4124564, 0.3575761, 0.1804375],
        [0.2126729, 0.7151522, 0.0721750],
        [0.0193339, 0.1191920, 0.9503041],
    ])
    return rgb @ M.T

def xyz_to_lab(xyz):
    white = np.array([0.95047, 1.0, 1.08883])
    xyz = xyz / white
    f = np.where(xyz > 0.008856, np.cbrt(xyz), 7.787 * xyz + 16.0/116.0)
    L = 116.0 * f[..., 1] - 16.0
    a = 500.0 * (f[..., 0] - f[..., 1])
    b = 200.0 * (f[..., 1] - f[..., 2])
    return np.stack([L, a, b], axis=-1)

def srgb_to_lab(rgb_uint8):
    lin = srgb_to_linear(rgb_uint8.astype(np.float64))
    return xyz_to_lab(linear_to_xyz(lin))

def linear_to_oklab(rgb_lin):
    """Linear sRGB -> oklab (L, a, b)."""
    # sRGB to LMS via oklab matrix
    M1 = np.array([
        [0.4122214708, 0.5363325363, 0.0514459929],
        [0.2119034982, 0.6806995451, 0.1073969566],
        [0.0883024619, 0.2817188376, 0.6299787005],
    ])
    lms = rgb_lin @ M1.T
    lms_ = np.sign(lms) * np.abs(lms) ** (1.0/3.0)
    M2 = np.array([
        [0.2104542553, 0.7936177850, -0.0040720468],
        [1.9779984951, -2.4285922050, 0.4505937099],
        [0.0259040371, 0.7827717662, -0.8086757660],
    ])
    return lms_ @ M2.T

def srgb_to_oklab(rgb_uint8):
    lin = srgb_to_linear(rgb_uint8.astype(np.float64))
    return linear_to_oklab(lin)

def oklab_to_oklch(lab):
    """oklab (L, a, b) -> oklch (L, C, h) with h in 0..1 range (vkdt convention)."""
    L = lab[..., 0]
    a = lab[..., 1]
    b = lab[..., 2]
    C = np.sqrt(a**2 + b**2)
    h = np.arctan2(b, a) / (2 * np.pi) % 1.0  # 0..1
    return np.stack([L, C, h], axis=-1)

def deltaE_2000(lab1, lab2):
    L1, a1, b1 = lab1[...,0], lab1[...,1], lab1[...,2]
    L2, a2, b2 = lab2[...,0], lab2[...,1], lab2[...,2]
    C1 = np.sqrt(a1**2 + b1**2)
    C2 = np.sqrt(a2**2 + b2**2)
    Cm = (C1 + C2) / 2.0
    G = 0.5 * (1 - np.sqrt(Cm**7 / (Cm**7 + 25**7)))
    a1p, a2p = a1 * (1 + G), a2 * (1 + G)
    C1p = np.sqrt(a1p**2 + b1**2)
    C2p = np.sqrt(a2p**2 + b2**2)
    h1p = np.degrees(np.arctan2(b1, a1p)) % 360
    h2p = np.degrees(np.arctan2(b2, a2p)) % 360
    dLp, dCp = L2 - L1, C2p - C1p
    dhp = h2p - h1p
    dhp = np.where(np.abs(dhp) > 180, dhp - np.sign(dhp) * 360, dhp)
    dHp = 2 * np.sqrt(C1p * C2p) * np.sin(np.radians(dhp / 2))
    Lm = (L1 + L2) / 2
    Cpm = (C1p + C2p) / 2
    hpm = (h1p + h2p) / 2
    hpm = np.where(np.abs(h1p - h2p) > 180, hpm + 180, hpm)
    T = (1 - 0.17*np.cos(np.radians(hpm-30)) + 0.24*np.cos(np.radians(2*hpm))
         + 0.32*np.cos(np.radians(3*hpm+6)) - 0.20*np.cos(np.radians(4*hpm-63)))
    SL = 1 + 0.015*(Lm-50)**2 / np.sqrt(20+(Lm-50)**2)
    SC = 1 + 0.045*Cpm
    SH = 1 + 0.015*Cpm*T
    RT_theta = 30*np.exp(-((hpm-275)/25)**2)
    RC = 2*np.sqrt(Cpm**7/(Cpm**7+25**7))
    RT = -np.sin(np.radians(2*RT_theta))*RC
    return np.sqrt((dLp/SL)**2 + (dCp/SC)**2 + (dHp/SH)**2 + RT*(dCp/SC)*(dHp/SH))


# ---- Helpers ----

HUE_NAMES = [
    (0.083, "red"),
    (0.167, "orange"),
    (0.250, "yellow"),
    (0.333, "chartreuse"),
    (0.417, "green"),
    (0.500, "teal"),
    (0.583, "cyan"),
    (0.667, "azure"),
    (0.750, "blue"),
    (0.833, "violet"),
    (0.917, "magenta"),
    (1.000, "rose"),
]

def hue_name(h01):
    """Map oklab hue 0..1 to nearest named hue."""
    for center, name in HUE_NAMES:
        if h01 < center + 0.042:
            return name
    return "red"

def hue_sector(h01, n_sectors=6):
    """Quantize hue to sector index and center."""
    idx = int(h01 * n_sectors) % n_sectors
    center = (idx + 0.5) / n_sectors
    return idx, center

def exposure_stops(v_lin_Y, e_lin_Y):
    """Exposure difference in stops from linear luminance."""
    if v_lin_Y <= 0 or e_lin_Y <= 0:
        return 0.0
    return math.log2(e_lin_Y / v_lin_Y)

def sat_ratio(v_C, e_C):
    """Chroma ratio (embedded / vkdt). >1 means Canon is more saturated."""
    if v_C < 0.005:
        return float('nan')
    return e_C / v_C


def sample_patch(img_arr, x, y, patch=5):
    h, w = img_arr.shape[:2]
    r = patch // 2
    x0, x1 = max(0, x-r), min(w, x+r+1)
    y0, y1 = max(0, y-r), min(h, y+r+1)
    return img_arr[y0:y1, x0:x1].mean(axis=(0,1))

def grid_samples(w, h, nx, ny, margin=0.05):
    xs = np.linspace(int(w * margin), int(w * (1 - margin)), nx, dtype=int)
    ys = np.linspace(int(h * margin), int(h * (1 - margin)), ny, dtype=int)
    return [{"x": int(x), "y": int(y), "label": f"grid_{j*nx+i}", "desc": ""}
            for j, y in enumerate(ys) for i, x in enumerate(xs)]

def load_samples(path):
    with open(path) as f:
        return json.load(f)["samples"]

def align_vkdt_to_embedded(vkdt_img, emb_img):
    ew, eh = emb_img.size
    if vkdt_img.size == emb_img.size:
        return vkdt_img
    vw, vh = vkdt_img.size
    if abs(vw/vh - eh/ew) < abs(vw/vh - ew/eh):
        emb_small = emb_img.resize((160, 108), Image.LANCZOS)
        ea = np.array(emb_small, dtype=np.float32)
        best_mse, best_rot = float('inf'), 90
        for rot in [90, 270]:
            va = np.array(vkdt_img.rotate(rot, expand=True).resize((160, 108), Image.LANCZOS), dtype=np.float32)
            mse = np.mean((va - ea)**2)
            if mse < best_mse:
                best_mse, best_rot = mse, rot
        vkdt_img = vkdt_img.rotate(best_rot, expand=True)
    return vkdt_img.resize((ew, eh), Image.LANCZOS)

def categorize(label):
    for prefix in ["skin", "orange", "pink", "green", "sky", "flower",
                    "yellow", "white", "shadow", "neutral", "warm", "floral",
                    "terrace", "foliage", "bg"]:
        if label.startswith(prefix):
            return prefix
    return "other"


# ---- Main ----

def compare(vkdt_path, embedded_path, samples_file=None, grid=None, output_json=False):
    vkdt_img = Image.open(vkdt_path).convert("RGB")
    emb_img = Image.open(embedded_path).convert("RGB")
    ew, eh = emb_img.size

    vkdt_img = align_vkdt_to_embedded(vkdt_img, emb_img)
    vkdt_arr = np.array(vkdt_img)
    emb_arr = np.array(emb_img)

    if samples_file:
        samples = load_samples(samples_file)
    elif grid:
        nx, ny = map(int, grid.split("x"))
        samples = grid_samples(ew, eh, nx, ny)
    else:
        print("Error: provide --samples-file or --grid", file=sys.stderr)
        sys.exit(1)

    results = []
    for i, s in enumerate(samples):
        x, y = s["x"], s["y"]
        label = s.get("label", f"pt_{i}")
        desc = s.get("desc", "")

        v_rgb = sample_patch(vkdt_arr, x, y)
        e_rgb = sample_patch(emb_arr, x, y)

        # CIELAB for deltaE
        v_lab = srgb_to_lab(v_rgb.reshape(1,3)).reshape(3)
        e_lab = srgb_to_lab(e_rgb.reshape(1,3)).reshape(3)
        dE = float(deltaE_2000(v_lab.reshape(1,3), e_lab.reshape(1,3)).item())

        # Oklab LCh — maps to vkdt curves module
        v_oklab = srgb_to_oklab(v_rgb.reshape(1,3)).reshape(3)
        e_oklab = srgb_to_oklab(e_rgb.reshape(1,3)).reshape(3)
        v_oklch = oklab_to_oklch(v_oklab.reshape(1,3)).reshape(3)
        e_oklch = oklab_to_oklch(e_oklab.reshape(1,3)).reshape(3)

        # Linear luminance for exposure stops
        v_lin = srgb_to_linear(v_rgb.astype(np.float64))
        e_lin = srgb_to_linear(e_rgb.astype(np.float64))
        v_Y = 0.2126*v_lin[0] + 0.7152*v_lin[1] + 0.0722*v_lin[2]
        e_Y = 0.2126*e_lin[0] + 0.7152*e_lin[1] + 0.0722*e_lin[2]

        ev_diff = exposure_stops(v_Y, e_Y)
        s_ratio = sat_ratio(v_oklch[1], e_oklch[1])

        # Hue delta in 0..1 space (wrapped)
        dh = e_oklch[2] - v_oklch[2]
        if dh > 0.5: dh -= 1.0
        if dh < -0.5: dh += 1.0

        results.append({
            "label": label,
            "desc": desc,
            "category": categorize(label),
            "x": x, "y": y,
            "vkdt_rgb": [int(round(c)) for c in v_rgb],
            "emb_rgb": [int(round(c)) for c in e_rgb],
            "dE2000": round(dE, 2),
            # oklab LCh (vkdt curves space)
            "vkdt_oklch": [round(v_oklch[0], 3), round(v_oklch[1], 4), round(v_oklch[2], 3)],
            "emb_oklch": [round(e_oklch[0], 3), round(e_oklch[1], 4), round(e_oklch[2], 3)],
            "hue_name": hue_name(e_oklch[2]) if e_oklch[1] > 0.01 else "neutral",
            # actionable deltas
            "ev_stops": round(ev_diff, 2),      # -> colour:01:exposure
            "sat_ratio": round(s_ratio, 2) if not math.isnan(s_ratio) else None,  # -> colour:01:sat
            "dY_oklab": round(float(e_oklch[0] - v_oklch[0]), 3),  # -> vtx6 (Y/h)
            "dC_oklab": round(float(e_oklch[1] - v_oklch[1]), 4),  # -> vtx7 (C/h)
            "dh_oklab": round(float(dh), 4),                        # -> vtx2 (h/h)
            # keep CIELAB for reference
            "dL": round(float(e_lab[0] - v_lab[0]), 1),
            "da": round(float(e_lab[1] - v_lab[1]), 1),
            "db": round(float(e_lab[2] - v_lab[2]), 1),
        })

    results.sort(key=lambda r: r["dE2000"], reverse=True)

    if output_json:
        out = {
            "vkdt": str(vkdt_path),
            "embedded": str(embedded_path),
            "dimensions": f"{ew}x{eh}",
            "n_samples": len(results),
            "mean_dE": round(np.mean([r["dE2000"] for r in results]), 2),
            "max_dE": round(max(r["dE2000"] for r in results), 2),
            "mean_ev_stops": round(np.mean([r["ev_stops"] for r in results]), 2),
            "samples": results,
        }
        print(json.dumps(out, indent=2))
        return

    mean_dE = np.mean([r["dE2000"] for r in results])
    max_dE = max(r["dE2000"] for r in results)
    skin_dE = [r["dE2000"] for r in results if r["category"] == "skin"]
    ev_stops = [r["ev_stops"] for r in results]
    sat_ratios = [r["sat_ratio"] for r in results if r["sat_ratio"] is not None]

    print(f"\n  vkdt: {vkdt_path}")
    print(f"  emb:  {embedded_path}")
    print(f"  mean dE: {mean_dE:.1f}  |  max dE: {max_dE:.1f}  |  skin dE: {np.mean(skin_dE):.1f}" if skin_dE else "")
    print(f"  mean EV: {np.mean(ev_stops):+.2f} stops  |  mean sat ratio: {np.mean(sat_ratios):.2f}x")

    # ---- Per-sample table ----
    print(f"\n  {'label':<22} {'hue':<8} {'vkdt_RGB':>14} {'emb_RGB':>14}  {'dE':>5} {'EV':>6} {'sat':>5} {'dY':>6} {'dC':>7} {'dh':>7}")
    print("  " + "-" * 104)
    for r in results:
        vrgb = f"({r['vkdt_rgb'][0]:3},{r['vkdt_rgb'][1]:3},{r['vkdt_rgb'][2]:3})"
        ergb = f"({r['emb_rgb'][0]:3},{r['emb_rgb'][1]:3},{r['emb_rgb'][2]:3})"
        sr = f"{r['sat_ratio']:.2f}" if r['sat_ratio'] is not None else "  n/a"
        print(f"  {r['label']:<22} {r['hue_name']:<8} {vrgb:>14} {ergb:>14}  {r['dE2000']:5.1f} {r['ev_stops']:+5.2f} {sr:>5} {r['dY_oklab']:+6.3f} {r['dC_oklab']:+7.4f} {r['dh_oklab']:+7.4f}")

    # ---- Per-category summary ----
    cats = {}
    for r in results:
        cats.setdefault(r["category"], []).append(r)
    print(f"\n  Category summary:")
    print(f"    {'category':<12} {'dE':>5} {'EV':>6} {'sat':>5} {'dY':>6} {'dC':>7} {'dh':>7}  n")
    print("    " + "-" * 60)
    for c in sorted(cats, key=lambda c: -np.mean([r["dE2000"] for r in cats[c]])):
        rs = cats[c]
        srs = [r["sat_ratio"] for r in rs if r["sat_ratio"] is not None]
        print(f"    {c:<12} {np.mean([r['dE2000'] for r in rs]):5.1f} "
              f"{np.mean([r['ev_stops'] for r in rs]):+5.2f} "
              f"{np.mean(srs):5.2f} " if srs else f"{'n/a':>5} ",
              end="")
        print(f"{np.mean([r['dY_oklab'] for r in rs]):+6.3f} "
              f"{np.mean([r['dC_oklab'] for r in rs]):+7.4f} "
              f"{np.mean([r['dh_oklab'] for r in rs]):+7.4f}  {len(rs)}")

    # ---- Per-hue-sector summary (maps to vtx curve positions) ----
    N_SECTORS = 6
    sector_names = ["red", "yellow", "green", "cyan", "blue", "magenta"]
    sectors = {i: [] for i in range(N_SECTORS)}
    for r in results:
        # Use embedded hue for sector assignment (skip near-neutral)
        eC = r["emb_oklch"][1]
        if eC < 0.015:
            continue
        eh = r["emb_oklch"][2]
        idx, _ = hue_sector(eh, N_SECTORS)
        sectors[idx].append(r)

    print(f"\n  Hue sector analysis (vtx curve mapping):")
    print(f"    oklab hue positions: {', '.join(f'{(i+0.5)/N_SECTORS:.3f}={sector_names[i]}' for i in range(N_SECTORS))}")
    print(f"    {'sector':<10} {'h_center':>8} {'n':>3}  {'mean_dY':>7} {'mean_dC':>8} {'mean_dh':>8} {'mean_EV':>8} {'mean_sat':>8}")
    print("    " + "-" * 68)
    for i in range(N_SECTORS):
        rs = sectors[i]
        hc = (i + 0.5) / N_SECTORS
        if not rs:
            print(f"    {sector_names[i]:<10} {hc:8.3f} {0:3}       —        —        —        —        —")
            continue
        srs = [r["sat_ratio"] for r in rs if r["sat_ratio"] is not None]
        print(f"    {sector_names[i]:<10} {hc:8.3f} {len(rs):3}  "
              f"{np.mean([r['dY_oklab'] for r in rs]):+7.3f} "
              f"{np.mean([r['dC_oklab'] for r in rs]):+8.4f} "
              f"{np.mean([r['dh_oklab'] for r in rs]):+8.4f} "
              f"{np.mean([r['ev_stops'] for r in rs]):+8.2f} "
              f"{np.mean(srs):8.2f}" if srs else "     n/a")

    # ---- Global tuning hints ----
    print(f"\n  Tuning hints:")
    med_ev = np.median(ev_stops)
    print(f"    colour:01:exposure  {med_ev:+.2f}  (median across all samples)")
    if sat_ratios:
        med_sat = np.median(sat_ratios)
        print(f"    colour:01:sat       {med_sat:.2f}   (median sat ratio, 1.0=neutral)")
    print()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Compare vkdt vs embedded JPEG")
    parser.add_argument("vkdt", help="vkdt-rendered JPG")
    parser.add_argument("embedded", help="Embedded JPEG from raw")
    parser.add_argument("--samples-file", "-s", help="JSON file with hand-picked sample coordinates")
    parser.add_argument("--grid", help="Use grid sampling NxM (e.g. 6x4)")
    parser.add_argument("--json", action="store_true", help="Output JSON instead of table")
    args = parser.parse_args()
    compare(args.vkdt, args.embedded, samples_file=args.samples_file, grid=args.grid, output_json=args.json)
