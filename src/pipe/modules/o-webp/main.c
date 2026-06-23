#include "modules/api.h"
#include "core/fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <webp/encode.h>

// called after the pipeline finished up to here. the input buffer comes in memory
// mapped as 8-bit rgba (srgb). we lossy-webp-encode the rgb channels and write
// `<filename>.webp`. used for the rabbit_editor web preview stream (smaller frames
// over WiFi than jpeg). no icc/exif here — the preview is plain srgb.
void write_sink(
    dt_module_t            *module,
    void                   *buf,
    dt_write_sink_params_t *p)
{
  const char *basename = dt_module_param_string(module, 0);
  const int width  = module->connector[0].roi.wd;
  const int height = module->connector[0].roi.ht;
  const uint8_t *in = buf;
  if(width <= 0 || height <= 0) return;

  char dir[512];
  snprintf(dir, sizeof(dir), "%s", basename);
  if(fs_dirname(dir)) fs_mkdir_p(dir, 0755);

  char filename[512];
  snprintf(filename, sizeof(filename), "%s.webp", basename);

  float quality = dt_module_param_float(module, 1)[0];
  if(quality < 0.0f)   quality = 0.0f;
  if(quality > 100.0f) quality = 100.0f;

  // pack rgba -> rgb (the preview has no meaningful alpha; rgb avoids an alpha plane)
  uint8_t *rgb = malloc((size_t)3 * width * height);
  if(!rgb) return;
  const size_t px = (size_t)width * height;
  for(size_t i = 0; i < px; i++)
  {
    rgb[3*i+0] = in[4*i+0];
    rgb[3*i+1] = in[4*i+1];
    rgb[3*i+2] = in[4*i+2];
  }

  uint8_t *out = 0;
  const size_t n = WebPEncodeRGB(rgb, width, height, width * 3, quality, &out);
  free(rgb);
  if(!n || !out) { if(out) WebPFree(out); return; }

  FILE *f = fopen(filename, "wb");
  if(f) { fwrite(out, 1, n, f); fclose(f); }
  WebPFree(out);
}
