# colormatch — compare vkdt renders to camera JPEGs

Compare vkdt-rendered images against embedded camera JPEGs to measure and
guide color matching. Output is in terms of vkdt module parameters so you
can translate the numbers directly into cfg edits.

## Setup

```sh
cd tools/colormatch
python3 -m venv .venv
.venv/bin/pip install numpy pillow
```

## Usage

### With hand-picked samples (recommended)

```sh
.venv/bin/python3 compare.py <vkdt.jpg> <embedded.jpg> -s samples.json
```

### With a grid

```sh
.venv/bin/python3 compare.py <vkdt.jpg> <embedded.jpg> --grid 8x6
```

### JSON output (for scripted iteration)

```sh
.venv/bin/python3 compare.py <vkdt.jpg> <embedded.jpg> -s samples.json --json
```

## Full workflow

### 1. Extract the embedded JPEG from a raw file

```sh
exiftool -b -PreviewImage _MG_1563.CR3 > embedded_1563.jpg
```

### 2. Create a vkdt export cfg

Start from the default darkroom pipeline and add `colenc` + `o-jpg` for
CLI export. The cfg must include `param:i-raw:main:filename:<raw>` pointing
at the original raw file. See `bin/default-darkroom.i-raw` for the base
pipeline and `LOOKS.md` for the variant naming convention.

### 3. Render with vkdt-cli

```sh
bin/vkdt-cli -g _MG_1563.CR3.cfg \
  --output main --filename output --format o-jpg --colour-trc sRGB
```

### 4. Run the comparison

```sh
.venv/bin/python3 tools/colormatch/compare.py \
  output.jpg embedded_1563.jpg -s tools/colormatch/samples_1563.json
```

### 5. Read the output and adjust

The tool reports three levels of detail:

**Per-sample table** — every sample point with its label, oklab hue name,
RGB values, deltaE, and actionable deltas:

| Column   | Meaning                          | Maps to                   |
|----------|----------------------------------|---------------------------|
| `EV`     | exposure difference in stops     | `colour:01:exposure`      |
| `sat`    | chroma ratio (embedded / vkdt)   | `colour:01:sat`           |
| `dY`     | oklab luminance delta            | `curves:01:vtx6` (Y/h)   |
| `dC`     | oklab chroma delta               | `curves:01:vtx7` (C/h)   |
| `dh`     | oklab hue delta (0..1 wrapped)   | `curves:01:vtx2` (h/h)   |

**Category summary** — averaged by semantic group (skin, orange, green, …).

**Hue sector analysis** — averaged by oklab hue sector (red, yellow, green,
cyan, blue, magenta). The `h_center` values are the x-positions for vtx
curve control points. Use `mean_dY` / `mean_dC` / `mean_dh` as the offset
values for vtx6 / vtx7 / vtx2 respectively.

**Tuning hints** — global median exposure and saturation correction.

### 6. Iterate

Edit the cfg, re-render, re-compare. The `--json` flag is useful for
scripting this loop. See `LOOKS.md` for the cfg variant naming convention
(`_MG_1563.CR3_r1_description.cfg`).

## Sample files

Each JSON sample file defines named pixel coordinates in the embedded
JPEG's coordinate space (which may be rotated relative to the scene).
The compare tool auto-detects rotation and resizes the vkdt render to match.

```json
{
  "image": "_MG_1563.CR3",
  "samples": [
    {"x": 240, "y": 720, "label": "skin_bright", "desc": "sunlit skin"},
    {"x": 1350, "y": 60, "label": "sky_blue", "desc": "clear blue sky"}
  ]
}
```

Labels should start with a category prefix for automatic grouping:
`skin_`, `orange_`, `green_`, `pink_`, `sky_`, `flower_`, `yellow_`,
`white_`, `shadow_`, `neutral_`, `warm_`, `foliage_`, `bg_`.

### Included sample files

- `samples_1563.json` — woman in orange wrap + child on terrace (24 points:
  7 skin, 4 orange, 2 sky, 2 yellow, 2 white, 2 neutral, 3 shadow, 1 floral, 1 terrace)
- `samples_4159.json` — baby in pink sweater with green foliage and flowers (24 points:
  6 skin, 6 green, 4 pink, 3 flower, 1 shadow, 1 white, 1 foliage, 1 warm, 1 bg)

## Oklab hue reference

```
red ≈ 0.083   orange ≈ 0.125   yellow ≈ 0.250
chartreuse ≈ 0.333   green ≈ 0.417   teal ≈ 0.500
cyan ≈ 0.583   azure ≈ 0.667   blue ≈ 0.750
violet ≈ 0.833   magenta ≈ 0.917   rose ≈ 0.958
```
