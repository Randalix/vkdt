// rabbit_editor web remote server (M3.2).
//
// Keeps one vkdt graph + its GPU buffers warm and serves a tiny web client over
// HTTP/WebSocket (civetweb). On connect it sends a `menu` message generated
// DYNAMICALLY from the live graph: every module that has editable (slider, float,
// scalar) parameters becomes a group, with each parameter's min/max/default/current
// -- exactly the data a touch radial/chord menu needs, no fixed favourites list.
// The client sends `P <modid> <parid> <value>`; the server writes the parameter,
// re-renders on the GPU and pushes the resulting JPEG back as a binary WS frame.
//
// usage: vkdt-server [docroot] [port] [graph.cfg] [image.jpg]
//
// The image is edited as a downscaled preview proxy (full-res only for export).
// No TLS here -- Tailscale Serve terminates HTTPS/WSS in front.

#include "qvk/qvk.h"
#include "pipe/graph.h"
#include "pipe/graph-io.h"
#include "pipe/graph-export.h"
#include "pipe/global.h"
#include "pipe/params.h"
#include "pipe/modules/api.h"
#include "core/log.h"
#include "core/threads.h"
#include "civetweb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>

static dt_graph_t      g_graph;
static pthread_mutex_t g_lock   = PTHREAD_MUTEX_INITIALIZER;
static const char     *g_jpgbase = "preview";
static char            g_jpgpath[512];
static volatile int    g_stop = 0;
static char            g_menu_json[65536];

static inline double now_ms()
{
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static inline int is_radial_slider(const dt_ui_param_t *p)
{
  return p->widget.type == dt_token("slider") && p->type == dt_token("float") && p->cnt == 1;
}
static inline int is_wheel(const dt_ui_param_t *p)
{ // colour wheel: rgb chroma + master, float[4]
  return p->widget.type == dt_token("colwheel") && p->type == dt_token("float") && p->cnt == 4;
}
static inline int is_editable(const dt_ui_param_t *p){ return is_radial_slider(p) || is_wheel(p); }

static inline float param_get(int modid, int parid)
{
  const dt_ui_param_t *p = g_graph.module[modid].so->param[parid];
  return *(float *)((uint8_t *)g_graph.module[modid].param + p->offset);
}

// run the warm graph (plus any extra runflags) and slurp the fresh jpeg.
// caller frees. NULL on failure. call with g_lock held.
static unsigned char *render_to_jpeg(size_t *out_len, double *render_ms, dt_graph_run_t extra)
{
  const double t0 = now_ms();
  g_graph.runflags = s_graph_run_record_cmd_buf | s_graph_run_download_sink | s_graph_run_wait_done | extra;
  if(dt_graph_run(&g_graph, g_graph.runflags) != VK_SUCCESS) return NULL;
  if(render_ms) *render_ms = now_ms() - t0;
  FILE *f = fopen(g_jpgpath, "rb");
  if(!f) return NULL;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  if(n <= 0) { fclose(f); return NULL; }
  unsigned char *buf = malloc(n);
  size_t rd = fread(buf, 1, n, f);
  fclose(f);
  if(rd != (size_t)n) { free(buf); return NULL; }
  *out_len = n;
  return buf;
}

static void push_frame(struct mg_connection *c, dt_graph_run_t extra)
{
  pthread_mutex_lock(&g_lock);
  size_t n = 0; double ms = 0;
  unsigned char *b = render_to_jpeg(&n, &ms, extra);
  pthread_mutex_unlock(&g_lock);
  if(!b) return;
  mg_websocket_write(c, MG_WEBSOCKET_OPCODE_BINARY, (const char *)b, n);
  free(b);
}

static char *json_label(char *o, char *e, const dt_token_t modname, const dt_ui_param_t *p)
{
  if(p->long_name && p->long_name[0])
    return o + snprintf(o, e-o, "%s", p->long_name);
  return o + snprintf(o, e-o, "%"PRItkn, dt_token_str(p->name));
}

// dynamically enumerate the graph: every module with >=1 radial-slider param
// becomes a menu group. presets come from darkroom.ui (display only for now).
static void build_menu_json(void)
{
  char *o = g_menu_json, *e = g_menu_json + sizeof(g_menu_json);
  o += snprintf(o, e-o, "{\"type\":\"menu\",\"groups\":[");
  int firstgrp = 1;
  for(int m = 0; m < g_graph.num_modules; m++)
  {
    dt_module_so_t *so = g_graph.module[m].so;
    if(!so) continue;
    const char *nm = dt_token_str(g_graph.module[m].name);
    if(nm[0] && nm[1]=='-' && (nm[0]=='i' || nm[0]=='o')) continue; // skip input/output modules
    int has = 0;
    for(int pi = 0; pi < so->num_params; pi++) if(is_editable(so->param[pi])) { has = 1; break; }
    if(!has) continue;
    if(!firstgrp) o += snprintf(o, e-o, ",");
    firstgrp = 0;
    o += snprintf(o, e-o, "{\"label\":\"%"PRItkn"\",\"modid\":%d,\"items\":[",
        dt_token_str(g_graph.module[m].name), m);
    int firstit = 1;
    for(int pi = 0; pi < so->num_params; pi++)
    {
      const dt_ui_param_t *p = so->param[pi];
      if(!is_editable(p)) continue;
      if(!firstit) o += snprintf(o, e-o, ",");
      firstit = 0;
      o += snprintf(o, e-o, "{\"label\":\"");
      o = json_label(o, e, g_graph.module[m].name, p);
      if(is_wheel(p))
      {
        const float *c = (const float *)((uint8_t *)g_graph.module[m].param + p->offset);
        const float *d = (const float *)p->val;
        o += snprintf(o, e-o, "\",\"kind\":\"wheel\",\"parid\":%d,\"min\":%g,\"max\":%g,"
            "\"def\":[%g,%g,%g,%g],\"cur\":[%g,%g,%g,%g]}",
            pi, p->widget.min, p->widget.max, d[0],d[1],d[2],d[3], c[0],c[1],c[2],c[3]);
      }
      else o += snprintf(o, e-o, "\",\"kind\":\"slider\",\"parid\":%d,\"min\":%g,\"max\":%g,\"def\":%g,\"cur\":%g}",
          pi, p->widget.min, p->widget.max, p->val[0], param_get(m, pi));
    }
    o += snprintf(o, e-o, "]}");
  }
  o += snprintf(o, e-o, "],\"presets\":[");
  // presets from darkroom.ui (cwd = bin)
  FILE *f = fopen("darkroom.ui", "r");
  if(f)
  {
    char line[256]; int firstp = 1;
    while(fgets(line, sizeof(line), f))
    {
      line[strcspn(line, "\r\n")] = 0;
      if(strncmp(line, "preset:", 7)) continue;
      char *desc = strtok(line + 7, ":");
      char *name = strtok(NULL, "");
      if(!desc || !name) continue;
      if(!firstp) o += snprintf(o, e-o, ",");
      firstp = 0;
      o += snprintf(o, e-o, "{\"label\":\"%s\",\"preset\":\"%s\"}", desc, name);
    }
    fclose(f);
  }
  o += snprintf(o, e-o, "]}");
}

static int  ws_connect(const struct mg_connection *c, void *u) { (void)c; (void)u; return 0; }
static void ws_close  (const struct mg_connection *c, void *u) { (void)c; (void)u; }

static void ws_ready(struct mg_connection *c, void *u)
{
  (void)u;
  mg_websocket_write(c, MG_WEBSOCKET_OPCODE_TEXT, g_menu_json, strlen(g_menu_json));
  push_frame(c, s_graph_run_none);
}

static int ws_data(struct mg_connection *c, int bits, char *data, size_t len, void *u)
{
  (void)u;
  if((bits & 0xf) != MG_WEBSOCKET_OPCODE_TEXT) return 1;
  char tmp[128]; size_t k = len < sizeof(tmp)-1 ? len : sizeof(tmp)-1;
  memcpy(tmp, data, k); tmp[k] = 0;

  int modid, parid; float v;
  if(sscanf(tmp, "P %d %d %f", &modid, &parid, &v) == 3 &&
     modid >= 0 && modid < g_graph.num_modules)
  {
    pthread_mutex_lock(&g_lock);
    dt_module_t *m = g_graph.module + modid;
    const dt_ui_param_t *p = m->so->param[parid];
    float *val = (float *)((uint8_t *)m->param + p->offset);
    float old = *val; *val = v;
    dt_graph_run_t fl = s_graph_run_none;
    if(m->so->check_params) fl = m->so->check_params(m, parid, 0, &old);
    g_graph.active_module = modid;
    size_t n = 0; double ms = 0;
    unsigned char *b = render_to_jpeg(&n, &ms, fl);
    pthread_mutex_unlock(&g_lock);
    if(b)
    {
      mg_websocket_write(c, MG_WEBSOCKET_OPCODE_BINARY, (const char *)b, n);
      free(b);
      fprintf(stderr, "[srv] set %d:%d = %g  render %.1f ms  %zu B\n", modid, parid, v, ms, n);
    }
    return 1;
  }

  // vec4 set (colour wheels): "V <modid> <parid> <v0> <v1> <v2> <v3>"
  float w[4];
  if(sscanf(tmp, "V %d %d %f %f %f %f", &modid, &parid, w+0, w+1, w+2, w+3) == 6 &&
     modid >= 0 && modid < g_graph.num_modules)
  {
    pthread_mutex_lock(&g_lock);
    dt_module_t *m = g_graph.module + modid;
    const dt_ui_param_t *p = m->so->param[parid];
    float *val = (float *)((uint8_t *)m->param + p->offset);
    float old0 = val[0];
    for(int i=0;i<4;i++) val[i] = w[i];
    dt_graph_run_t fl = s_graph_run_none;
    if(m->so->check_params) fl = m->so->check_params(m, parid, 0, &old0);
    g_graph.active_module = modid;
    size_t n = 0; double ms = 0;
    unsigned char *b = render_to_jpeg(&n, &ms, fl);
    pthread_mutex_unlock(&g_lock);
    if(b)
    {
      mg_websocket_write(c, MG_WEBSOCKET_OPCODE_BINARY, (const char *)b, n);
      free(b);
      fprintf(stderr, "[srv] setvec %d:%d  render %.1f ms  %zu B\n", modid, parid, ms, n);
    }
  }
  return 1;
}

static void on_sigint(int s) { (void)s; g_stop = 1; }

int main(int argc, char *argv[])
{
  const char *docroot = argc > 1 ? argv[1] : "../web";
  const char *port    = argc > 2 ? argv[2] : "8090";
  const char *cfg     = argc > 3 ? argv[3] : "examples/m3.cfg";
  const char *image   = argc > 4 ? argv[4] : NULL;

  dt_log_init(s_log_cli);
  dt_pipe_global_init();
  threads_global_init();
  if(qvk_init(0, -1, 0, 0, 0)) { fprintf(stderr, "[srv] qvk_init failed\n"); return 1; }

  dt_graph_init(&g_graph, s_queue_compute);
  snprintf(g_graph.searchpath, sizeof(g_graph.searchpath), ".");

  dt_graph_export_t param = {0};
  param.p_cfgfile  = cfg;
  param.output_cnt = 1;
  param.output[0].inst             = dt_token("main");
  param.output[0].mod              = dt_token("o-jpg");
  param.output[0].p_filename       = g_jpgbase;
  param.output[0].quality          = 90;
  param.output[0].colour_primaries = s_colour_primaries_srgb;
  param.output[0].colour_trc       = s_colour_trc_srgb;
  param.output[0].max_width        = 1280;
  param.output[0].max_height       = 1280;

  // preview proxy: full-res source would be re-decoded each frame (~400ms/12MP).
  // edit a downscaled proxy; full-res is only for export (M5). TODO keep source resident.
  const char *use_image = image;
  char proxypath[1024];
  if(image)
  {
    snprintf(proxypath, sizeof(proxypath), "rabbit_proxy.jpg");
    char cmd[2400];
    snprintf(cmd, sizeof(cmd),
        "python3 -c 'from PIL import Image; im=Image.open(\"%s\"); "
        "im.thumbnail((1600,1600)); im.save(\"%s\",quality=92)' 2>/dev/null", image, proxypath);
    if(system(cmd) == 0 && access(proxypath, R_OK) == 0)
    { use_image = proxypath; fprintf(stderr, "[srv] preview proxy %s <- %s\n", proxypath, image); }
    else fprintf(stderr, "[srv] proxy failed, full-res %s (slow)\n", image);
  }
  char imgline[1024]; char *extra[1];
  if(use_image)
  {
    snprintf(imgline, sizeof(imgline), "param:i-jpg:main:filename:%s", use_image);
    extra[0] = imgline;
    param.extra_param_cnt = 1;
    param.p_extra_param   = extra;
  }
  if(dt_graph_export(&g_graph, &param) != VK_SUCCESS)
  { fprintf(stderr, "[srv] graph setup failed for '%s'\n", cfg); return 1; }

  snprintf(g_jpgpath, sizeof(g_jpgpath), "%s.jpg", g_jpgbase);
  build_menu_json();
  fprintf(stderr, "[srv] graph warm. jpg=%s  menu=%zu B\n", g_jpgpath, strlen(g_menu_json));

  signal(SIGINT, on_sigint);
  mg_init_library(0);
  const char *opts[] = {
    "document_root",   docroot,
    "listening_ports", port,
    "num_threads",     "4",
    "enable_directory_listing", "no",
    "tcp_nodelay",     "1",
    "static_file_max_age", "0",
    "additional_header", "Cache-Control: no-store",  // never cache the client during dev
    NULL
  };
  struct mg_callbacks cb; memset(&cb, 0, sizeof(cb));
  struct mg_context *ctx = mg_start(&cb, NULL, opts);
  if(!ctx) { fprintf(stderr, "[srv] mg_start failed (port %s)\n", port); return 1; }
  mg_set_websocket_handler(ctx, "/ws", ws_connect, ws_ready, ws_data, ws_close, NULL);

  fprintf(stderr, "[srv] listening on :%s  docroot=%s  (Ctrl-C to stop)\n", port, docroot);
  while(!g_stop) sleep(1);

  fprintf(stderr, "[srv] shutting down\n");
  mg_stop(ctx);
  mg_exit_library();
  dt_graph_cleanup(&g_graph);
  threads_global_cleanup();
  qvk_cleanup();
  return 0;
}
