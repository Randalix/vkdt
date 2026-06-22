// rabbit_editor web remote server (M3.1).
//
// Keeps one vkdt graph + its GPU buffers warm and serves a tiny web client over
// HTTP/WebSocket (civetweb). On connect it sends a `state` message describing the
// favourite parameters (read from darkroom.ui, resolved against the live graph,
// with min/max/default/current metadata) -- the data a touch radial menu needs.
// The client sends `P <modid> <parid> <value>`; the server writes the parameter,
// re-renders on the GPU and pushes the resulting JPEG back as a binary WS frame.
//
// usage: vkdt-server [docroot] [port] [graph.cfg]
//
// No TLS here on purpose -- Tailscale Serve terminates HTTPS/WSS in front.
// readback still via o-jpg file; WebP-in-memory is a later refinement.

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

typedef struct fav_t
{
  int  is_preset;
  int  modid, parid;     // for params
  char preset[64];       // for presets
  char label[80];
} fav_t;

static dt_graph_t      g_graph;
static int             g_outmod = -1;
static pthread_mutex_t g_lock   = PTHREAD_MUTEX_INITIALIZER;
static const char     *g_jpgbase = "preview";
static char            g_jpgpath[512];
static volatile int    g_stop = 0;

static fav_t           g_fav[32];
static int             g_favcnt = 0;
static char            g_state_json[16384];

static inline double now_ms()
{
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static inline float param_get(int modid, int parid)
{
  const dt_ui_param_t *p = g_graph.module[modid].so->param[parid];
  return *(float *)((uint8_t *)g_graph.module[modid].param + p->offset);
}

// run the warm graph (with any extra runflags) and slurp the fresh jpeg.
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

// read darkroom.ui, resolve favourites against the live graph, keep slider floats
// and presets. cwd is the bin/ dir, so darkroom.ui sits right here.
static void load_favs(void)
{
  FILE *f = fopen("darkroom.ui", "r");
  if(!f) { fprintf(stderr, "[srv] no darkroom.ui, no favourites\n"); return; }
  char line[256];
  while(fgets(line, sizeof(line), f) && g_favcnt < (int)(sizeof(g_fav)/sizeof(g_fav[0])))
  {
    line[strcspn(line, "\r\n")] = 0;
    if(!line[0] || line[0] == '#') continue;
    char *a = strtok(line, ":");
    char *b = strtok(NULL, ":");
    char *c = strtok(NULL, "");   // rest of line (preset name has no further colon)
    if(!a || !b) continue;
    fav_t *fv = g_fav + g_favcnt;
    if(!strcmp(a, "preset"))
    {
      if(!c) continue;
      fv->is_preset = 1;
      snprintf(fv->preset, sizeof(fv->preset), "%s", c);
      snprintf(fv->label,  sizeof(fv->label),  "%s", b);  // human description
      g_favcnt++;
      continue;
    }
    if(!c) continue;
    int modid = dt_module_get(&g_graph, dt_token(a), dt_token(b));
    if(modid < 0) continue;
    int parid = dt_module_get_param(g_graph.module[modid].so, dt_token(c));
    if(parid < 0) continue;
    const dt_ui_param_t *p = g_graph.module[modid].so->param[parid];
    if(p->widget.type != dt_token("slider")) continue; // radial value widget supports sliders
    if(p->type != dt_token("float") || p->cnt != 1)  continue; // scalar float only, for now
    fv->is_preset = 0; fv->modid = modid; fv->parid = parid;
    if(p->long_name && p->long_name[0]) snprintf(fv->label, sizeof(fv->label), "%s", p->long_name);
    else snprintf(fv->label, sizeof(fv->label), "%"PRItkn" %"PRItkn,
        dt_token_str(g_graph.module[modid].name), dt_token_str(p->name));
    g_favcnt++;
  }
  fclose(f);
  fprintf(stderr, "[srv] %d favourites resolved from darkroom.ui\n", g_favcnt);
}

// build the (static) state json once. current values are refreshed lazily below.
static void build_state_json(void)
{
  char *o = g_state_json; char *e = g_state_json + sizeof(g_state_json);
  o += snprintf(o, e-o, "{\"type\":\"state\",\"favs\":[");
  for(int i = 0; i < g_favcnt; i++)
  {
    fav_t *fv = g_fav + i;
    if(i) o += snprintf(o, e-o, ",");
    if(fv->is_preset)
    {
      o += snprintf(o, e-o, "{\"i\":%d,\"kind\":\"preset\",\"label\":\"%s\",\"preset\":\"%s\"}",
          i, fv->label, fv->preset);
    }
    else
    {
      const dt_ui_param_t *p = g_graph.module[fv->modid].so->param[fv->parid];
      o += snprintf(o, e-o,
          "{\"i\":%d,\"kind\":\"param\",\"label\":\"%s\",\"modid\":%d,\"parid\":%d,"
          "\"min\":%g,\"max\":%g,\"def\":%g,\"cur\":%g}",
          i, fv->label, fv->modid, fv->parid,
          p->widget.min, p->widget.max, p->val[0], param_get(fv->modid, fv->parid));
    }
  }
  o += snprintf(o, e-o, "]}");
}

static int  ws_connect(const struct mg_connection *c, void *u) { (void)c; (void)u; return 0; }
static void ws_close  (const struct mg_connection *c, void *u) { (void)c; (void)u; }

static void ws_ready(struct mg_connection *c, void *u)
{
  (void)u;
  mg_websocket_write(c, MG_WEBSOCKET_OPCODE_TEXT, g_state_json, strlen(g_state_json));
  push_frame(c, s_graph_run_none); // initial image
}

static int ws_data(struct mg_connection *c, int bits, char *data, size_t len, void *u)
{
  (void)u;
  if((bits & 0xf) != MG_WEBSOCKET_OPCODE_TEXT) return 1;
  char tmp[128]; size_t k = len < sizeof(tmp)-1 ? len : sizeof(tmp)-1;
  memcpy(tmp, data, k); tmp[k] = 0;

  // protocol: "P <modid> <parid> <value>"  -> set parameter and re-render
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
  }
  return 1;
}

static void on_sigint(int s) { (void)s; g_stop = 1; }

int main(int argc, char *argv[])
{
  const char *docroot = argc > 1 ? argv[1] : "../web";
  const char *port    = argc > 2 ? argv[2] : "8090";
  const char *cfg     = argc > 3 ? argv[3] : "examples/m3.cfg";

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
  if(dt_graph_export(&g_graph, &param) != VK_SUCCESS)
  { fprintf(stderr, "[srv] graph setup failed for '%s'\n", cfg); return 1; }

  g_outmod = dt_module_get(&g_graph, dt_token("o-jpg"), dt_token("main"));
  snprintf(g_jpgpath, sizeof(g_jpgpath), "%s.jpg", g_jpgbase);
  load_favs();
  build_state_json();
  fprintf(stderr, "[srv] graph warm. o-jpg id=%d  jpg=%s  state=%zu B\n",
      g_outmod, g_jpgpath, strlen(g_state_json));

  signal(SIGINT, on_sigint);
  mg_init_library(0);
  const char *opts[] = {
    "document_root",   docroot,
    "listening_ports", port,
    "num_threads",     "4",
    "enable_directory_listing", "no",
    "tcp_nodelay",     "1",   // disable Nagle: small WS frames must not stall ~40ms on delayed-ACK
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
