// rabbit_editor web remote server (M2).
//
// Keeps one vkdt graph + its GPU buffers warm and serves a tiny web client over
// HTTP/WebSocket (civetweb). The browser sends a parameter value, the server
// re-renders on the GPU and pushes the resulting JPEG back as a binary WS frame.
// This is the end-to-end loop whose latency we want to measure over Tailscale.
//
// usage: vkdt-server [docroot] [port] [graph.cfg]
//
// NOTE M2: readback currently goes through the o-jpg module to a file which the
// server reads back; WebP-in-memory (skip the disk hop) is the next refinement.
// No TLS here on purpose -- Tailscale Serve terminates HTTPS/WSS in front.

#include "qvk/qvk.h"
#include "pipe/graph.h"
#include "pipe/graph-io.h"
#include "pipe/graph-export.h"
#include "pipe/global.h"
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
static int             g_eqmod  = -1;   // module whose param the slider drives
static int             g_outmod = -1;   // o-jpg sink
static pthread_mutex_t g_lock   = PTHREAD_MUTEX_INITIALIZER; // serialise graph access
static const char     *g_jpgbase = "preview";
static char            g_jpgpath[512];
static volatile int    g_stop = 0;

static inline double now_ms()
{
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

// run the warm graph and slurp the freshly written jpeg into a malloc'd buffer.
// caller frees. returns NULL on failure. call with g_lock held.
static unsigned char *render_to_jpeg(size_t *out_len, double *render_ms)
{
  const double t0 = now_ms();
  g_graph.runflags = s_graph_run_record_cmd_buf | s_graph_run_download_sink | s_graph_run_wait_done;
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

static void send_frame(struct mg_connection *c)
{
  pthread_mutex_lock(&g_lock);
  size_t n = 0; double ms = 0;
  unsigned char *b = render_to_jpeg(&n, &ms);
  pthread_mutex_unlock(&g_lock);
  if(!b) return;
  mg_websocket_write(c, MG_WEBSOCKET_OPCODE_BINARY, (const char *)b, n);
  free(b);
}

static int ws_connect(const struct mg_connection *c, void *u)
{ (void)c; (void)u; return 0; } // 0 = accept

static void ws_ready(struct mg_connection *c, void *u)
{ (void)u; send_frame(c); } // push an initial frame on connect

static int ws_data(struct mg_connection *c, int bits, char *data, size_t len, void *u)
{
  (void)u;
  if((bits & 0xf) == MG_WEBSOCKET_OPCODE_TEXT)
  {
    // protocol (M2, minimal): "edges=<float>"
    char tmp[64]; size_t k = len < sizeof(tmp)-1 ? len : sizeof(tmp)-1;
    memcpy(tmp, data, k); tmp[k] = 0;
    const char *eq = strchr(tmp, '=');
    if(eq && g_eqmod >= 0)
    {
      const float v = atof(eq + 1);
      pthread_mutex_lock(&g_lock);
      dt_module_set_param_float(g_graph.module + g_eqmod, dt_token("edges"), v);
      size_t n = 0; double ms = 0;
      unsigned char *b = render_to_jpeg(&n, &ms);
      pthread_mutex_unlock(&g_lock);
      if(b)
      {
        mg_websocket_write(c, MG_WEBSOCKET_OPCODE_BINARY, (const char *)b, n);
        free(b);
        fprintf(stderr, "[srv] edges=%.3f  render %.1f ms  %zu B\n", v, ms, n);
      }
    }
  }
  return 1; // keep connection open
}

static void ws_close(const struct mg_connection *c, void *u) { (void)c; (void)u; }

static void on_sigint(int s) { (void)s; g_stop = 1; }

int main(int argc, char *argv[])
{
  const char *docroot = argc > 1 ? argv[1] : "../web";
  const char *port    = argc > 2 ? argv[2] : "8090";
  const char *cfg     = argc > 3 ? argv[3] : "examples/m1.cfg";

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

  g_eqmod  = dt_module_get(&g_graph, dt_token("eq"),    dt_token("01"));
  g_outmod = dt_module_get(&g_graph, dt_token("o-jpg"), dt_token("main"));
  snprintf(g_jpgpath, sizeof(g_jpgpath), "%s.jpg", g_jpgbase);
  fprintf(stderr, "[srv] graph warm. eq id=%d  o-jpg id=%d  jpg=%s\n", g_eqmod, g_outmod, g_jpgpath);

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
