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
#include "pipe/graph-history.h"
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
#include <sys/stat.h>
#include <pthread.h>
#include <signal.h>
#include <setjmp.h>
#include <stdarg.h>
#include <ctype.h>
#include <jpeglib.h>

static dt_graph_t      g_graph;
static pthread_mutex_t g_lock   = PTHREAD_MUTEX_INITIALIZER;
static const char     *g_jpgbase = "preview";
static char            g_jpgpath[512];
static const char     *g_histbase = "histogram";  // second sink: the waveform histogram image
static char            g_histpath[512];
static volatile int    g_stop = 0;
static char            g_menu_json[65536];
static char            g_recipe_path[1024];   // server-authoritative recipe (vkdt .cfg sidecar)
static char            g_cur_image[1024];      // current ORIGINAL image (not the proxy) — for full-res export
static volatile int    g_dirty = 0;           // edits pending an autosave
static uint32_t        g_history_base = 0;    // history items below this are the baseline snapshot
static FILE           *g_logf = NULL;         // optional logfile (config: logfile=)

// all server settings live here; loaded from a config file (key=value), CLI args override.
static struct {
  char port[16], docroot[512], cfg[512], cfg_raw[512], image[1024], libdir[512], logfile[512];
  int  proxy_max, preview_max, quality;
  int  preview_webp;   // 1 = stream webp preview frames (o-webp) instead of jpeg (o-jpg)
} g_conf = { "8090", "../web", "examples/m3.cfg", "examples/m3_raw.cfg", "", "uploads", "rabbit.log", 1600, 1280, 90, 0 };

// raw photo extensions -> use the i-raw pipeline (no jpeg proxy; vkdt decodes the raw)
static int is_raw_path(const char *p)
{
  const char *dot = strrchr(p, '.'); if(!dot) return 0;
  char e[8]; int i = 0; for(const char *s = dot+1; s[i] && i < 7; i++) e[i] = tolower((unsigned char)s[i]); e[i] = 0;
  static const char *raw[] = { "dng","cr2","cr3","nef","arw","raf","rw2","orf","pef","srw","raw","3fr","iiq","nrw","mrw", NULL };
  for(int k = 0; raw[k]; k++) if(!strcmp(e, raw[k])) return 1;
  return 0;
}

static void recipe_save(void);
static int  open_image(const char *image);    // (re)build the warm graph for an image

// timestamped log to stderr + the configured logfile. tag groups the source (srv/cli/err).
static void lg(const char *tag, const char *fmt, ...)
{
  char ts[16]; time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
  strftime(ts, sizeof(ts), "%H:%M:%S", &tm);
  char msg[1024]; va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof(msg), fmt, ap); va_end(ap);
  fprintf(stderr, "[%s] %-3s %s\n", ts, tag, msg); fflush(stderr);
  if(g_logf) { fprintf(g_logf, "[%s] %-3s %s\n", ts, tag, msg); fflush(g_logf); }
}

static void load_config(const char *path)
{
  FILE *f = fopen(path, "r"); if(!f) return;
  char line[1280];
  while(fgets(line, sizeof(line), f))
  {
    line[strcspn(line, "\r\n")] = 0;
    char *k = line; while(*k==' '||*k=='\t') k++;
    if(*k=='#' || !*k) continue;
    char *eq = strchr(k, '='); if(!eq) continue;
    *eq = 0; char *v = eq+1; while(*v==' '||*v=='\t') v++;
    char *ke = eq; while(ke>k && (ke[-1]==' '||ke[-1]=='\t')) *--ke = 0;
    if     (!strcmp(k,"port"))        snprintf(g_conf.port,    sizeof(g_conf.port),    "%s", v);
    else if(!strcmp(k,"docroot"))     snprintf(g_conf.docroot, sizeof(g_conf.docroot), "%s", v);
    else if(!strcmp(k,"cfg"))         snprintf(g_conf.cfg,     sizeof(g_conf.cfg),     "%s", v);
    else if(!strcmp(k,"cfg_raw"))     snprintf(g_conf.cfg_raw, sizeof(g_conf.cfg_raw), "%s", v);
    else if(!strcmp(k,"image"))       snprintf(g_conf.image,   sizeof(g_conf.image),   "%s", v);
    else if(!strcmp(k,"libdir"))      snprintf(g_conf.libdir,  sizeof(g_conf.libdir),  "%s", v);
    else if(!strcmp(k,"logfile"))     snprintf(g_conf.logfile, sizeof(g_conf.logfile), "%s", v);
    else if(!strcmp(k,"proxy_max"))   g_conf.proxy_max   = atoi(v);
    else if(!strcmp(k,"preview_max")) g_conf.preview_max = atoi(v);
    else if(!strcmp(k,"quality"))     g_conf.quality     = atoi(v);
    else if(!strcmp(k,"preview_format")) g_conf.preview_webp = !strcmp(v, "webp");
  }
  fclose(f);
}

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
static inline int is_crop(const dt_ui_param_t *p)
{ // crop rect (x,X,y,Y) fractions, float[4]
  return p->widget.type == dt_token("crop") && p->type == dt_token("float") && p->cnt == 4;
}
static inline int is_straight(const dt_ui_param_t *p)
{ // rotate angle (degrees), float[1] — vkdt "straight" widget
  return p->widget.type == dt_token("straight") && p->type == dt_token("float") && p->cnt == 1;
}
static inline int is_editable(const dt_ui_param_t *p){ return is_radial_slider(p) || is_wheel(p) || is_crop(p) || is_straight(p); }

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

// binary WS frames carry a 1-byte type tag so the client can tell preview from histogram:
//   0x00 = preview image, 0x01 = histogram (waveform) image. the client sniffs byte 0
//   (a jpeg starts 0xFF, webp 'R'=0x52) so it stays compatible with an un-tagged old server.
#define FRAME_PREVIEW 0x00
#define FRAME_HIST    0x01
static void ws_send_tagged(struct mg_connection *c, uint8_t type, const unsigned char *data, size_t n)
{
  unsigned char *buf = malloc(n + 1);
  if(!buf) return;
  buf[0] = type;
  memcpy(buf + 1, data, n);
  mg_websocket_write(c, MG_WEBSOCKET_OPCODE_BINARY, (const char *)buf, n + 1);
  free(buf);
}

// slurp a whole file into a fresh buffer. caller frees. NULL on failure.
static unsigned char *slurp_file(const char *path, size_t *out_len)
{
  FILE *f = fopen(path, "rb");
  if(!f) return NULL;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  if(n <= 0) { fclose(f); return NULL; }
  unsigned char *buf = malloc(n);
  size_t rd = buf ? fread(buf, 1, n, f) : 0;
  fclose(f);
  if(!buf || rd != (size_t)n) { free(buf); return NULL; }
  *out_len = n; return buf;
}

static void push_frame(struct mg_connection *c, dt_graph_run_t extra)
{
  pthread_mutex_lock(&g_lock);
  size_t n = 0; double ms = 0;
  unsigned char *b = render_to_jpeg(&n, &ms, extra);
  pthread_mutex_unlock(&g_lock);
  if(!b) return;
  ws_send_tagged(c, FRAME_PREVIEW, b, n);
  free(b);
}

// send the current histogram image (written by the last render's hist sink). the client
// requests this (cmd `scope`) when the histogram widget is open and on drag-end — keeping
// the histogram off the per-scrub-frame hot path.
static void push_hist_frame(struct mg_connection *c)
{
  pthread_mutex_lock(&g_lock);
  size_t n = 0;
  unsigned char *b = slurp_file(g_histpath, &n);
  pthread_mutex_unlock(&g_lock);
  if(!b) return;
  ws_send_tagged(c, FRAME_HIST, b, n);
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
      else if(is_crop(p))
      {
        const float *c = (const float *)((uint8_t *)g_graph.module[m].param + p->offset);
        o += snprintf(o, e-o, "\",\"kind\":\"crop\",\"parid\":%d,\"cur\":[%g,%g,%g,%g]}",
            pi, c[0],c[1],c[2],c[3]);
      }
      else if(is_straight(p))
      { // rotate: expose as a 1D slider in degrees; 1337 is the EXIF sentinel -> show 0
        float cur = param_get(m, pi); if(cur == 1337.0f) cur = 0.0f;
        o += snprintf(o, e-o, "\",\"kind\":\"slider\",\"parid\":%d,\"min\":-180,\"max\":180,\"def\":0,\"cur\":%g}",
            pi, cur);
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

// send the user-facing history stack (edits after the baseline snapshot) as JSON.
// each item carries its absolute index (for `jump`) and a short label parsed from the
// stored config line "param:<module>:<inst>:<name>:<value...>".
static void send_history(struct mg_connection *c)
{
  static char buf[16384];
  char *o = buf, *e = buf + sizeof(buf);
  pthread_mutex_lock(&g_lock);
  o += snprintf(o, e-o, "{\"type\":\"history\",\"cur\":%u,\"base\":%u,\"items\":[",
      g_graph.history_item_cur, g_history_base);
  int first = 1;
  for(uint32_t i = g_history_base; i < g_graph.history_item_end; i++)
  {
    char tmp[256]; strncpy(tmp, g_graph.history_item[i], sizeof(tmp)-1); tmp[sizeof(tmp)-1] = 0;
    char *save = 0;
    strtok_r(tmp, ":", &save);                 // "param"
    char *mod = strtok_r(0, ":", &save);       // module
    strtok_r(0, ":", &save);                   // inst
    char *nam = strtok_r(0, ":", &save);       // param name
    char *val = save ? save : "";              // value (may contain ':')
    char label[160];
    snprintf(label, sizeof(label), "%s · %s %.40s", mod?mod:"", nam?nam:"", val);
    if(!first) o += snprintf(o, e-o, ","); first = 0;
    o += snprintf(o, e-o, "{\"i\":%u,\"label\":\"%s\"}", i, label);
  }
  o += snprintf(o, e-o, "]}");
  pthread_mutex_unlock(&g_lock);
  mg_websocket_write(c, MG_WEBSOCKET_OPCODE_TEXT, buf, o-buf);
}

// send the module DAG (topology) for the node-graph view: modules + input-connection links.
static void send_graph(struct mg_connection *c)
{
  static char buf[32768];
  char *o = buf, *e = buf + sizeof(buf);
  pthread_mutex_lock(&g_lock);
  o += snprintf(o, e-o, "{\"type\":\"graph\",\"modules\":[");
  int first = 1;
  for(int m = 0; m < g_graph.num_modules; m++)
  {
    if(!g_graph.module[m].so) continue;
    if(!first) o += snprintf(o, e-o, ","); first = 0;
    o += snprintf(o, e-o, "{\"id\":%d,\"name\":\"%"PRItkn"\",\"inst\":\"%"PRItkn"\"}",
        m, dt_token_str(g_graph.module[m].name), dt_token_str(g_graph.module[m].inst));
  }
  o += snprintf(o, e-o, "],\"links\":[");
  first = 1;
  for(int m = 0; m < g_graph.num_modules; m++)
  {
    dt_module_t *mod = g_graph.module + m; if(!mod->so) continue;
    for(int cc = 0; cc < mod->num_connectors; cc++)
    {
      dt_connector_t *cn = mod->connector + cc;
      if(dt_connector_input(cn) && cn->connected.i >= 0)
      {
        if(!first) o += snprintf(o, e-o, ","); first = 0;
        o += snprintf(o, e-o, "{\"from\":%d,\"fc\":%d,\"to\":%d,\"tc\":%d}", cn->connected.i, cn->connected.c, m, cc);
      }
    }
  }
  o += snprintf(o, e-o, "]}");
  pthread_mutex_unlock(&g_lock);
  mg_websocket_write(c, MG_WEBSOCKET_OPCODE_TEXT, buf, o-buf);
}

// after a history op (undo/redo/jump/reset) the whole edit state changed: re-render,
// re-send the dynamic menu (so the client's param values stay in sync) and the history.
static void after_graph_change(struct mg_connection *c)
{
  push_frame(c, s_graph_run_all);
  pthread_mutex_lock(&g_lock); build_menu_json(); pthread_mutex_unlock(&g_lock);
  mg_websocket_write(c, MG_WEBSOCKET_OPCODE_TEXT, g_menu_json, strlen(g_menu_json));
  send_history(c);
  g_dirty = 1;
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

  if(!strncmp(tmp, "save", 4)) { recipe_save(); return 1; }  // explicit save (autosave also runs)
  if(!strncmp(tmp, "log ", 4))  // client-forwarded console/error message (may exceed tmp)
  { char m[1024]; size_t z = len > 4 ? len - 4 : 0; if(z >= sizeof(m)) z = sizeof(m)-1;
    memcpy(m, data+4, z); m[z] = 0; lg("cli", "%s", m); return 1; }

  if(!strncmp(tmp, "open ", 5))
  { // switch to a library image: flush current edits, rebuild graph, resync client
    char name[120]; snprintf(name, sizeof(name), "%s", tmp + 5);
    char *bn = name; for(char *p = name; *p; p++) if(*p=='/' || *p=='\\') bn = p+1;
    if(*bn && !strstr(bn, ".."))
    {
      if(g_dirty) recipe_save();
      char path[1024]; snprintf(path, sizeof(path), "%s/%s", g_conf.libdir, bn);
      lg("srv", "open %s", path);
      pthread_mutex_lock(&g_lock);
      int err = open_image(path);
      pthread_mutex_unlock(&g_lock);
      if(err) lg("err", "open failed: %s", path);
      else { after_graph_change(c); g_dirty = 0; }  // freshly opened: not dirty
    }
    return 1;
  }

  if(!strncmp(tmp, "scope", 5)) { push_hist_frame(c); return 1; }   // histogram widget: send current waveform
  if(!strncmp(tmp, "hist", 4)) { send_history(c); return 1; }
  if(!strncmp(tmp, "graph", 5)) { send_graph(c); return 1; }
  if(!strncmp(tmp, "undo", 4))
  {
    pthread_mutex_lock(&g_lock);
    if(g_graph.history_item_cur > g_history_base) dt_graph_history_set(&g_graph, g_graph.history_item_cur - 2);
    pthread_mutex_unlock(&g_lock);
    after_graph_change(c); return 1;
  }
  if(!strncmp(tmp, "redo", 4))
  {
    pthread_mutex_lock(&g_lock);
    if(g_graph.history_item_cur < g_graph.history_item_end) dt_graph_history_set(&g_graph, g_graph.history_item_cur);
    pthread_mutex_unlock(&g_lock);
    after_graph_change(c); return 1;
  }
  { int hi;
    if(sscanf(tmp, "jump %d", &hi) == 1)
    {
      pthread_mutex_lock(&g_lock);
      if(hi >= (int)g_history_base - 1 && hi < (int)g_graph.history_item_end) dt_graph_history_set(&g_graph, hi);
      pthread_mutex_unlock(&g_lock);
      after_graph_change(c); return 1;
    }
  }
  if(!strncmp(tmp, "reset", 5))
  { // back to default: reset all editable modules' params, keep i-/o- (input/output) intact
    pthread_mutex_lock(&g_lock);
    for(int m = 0; m < g_graph.num_modules; m++)
    {
      dt_module_so_t *so = g_graph.module[m].so; if(!so) continue;
      const char *nm = dt_token_str(g_graph.module[m].name);
      if(nm[0] && nm[1]=='-' && (nm[0]=='i' || nm[0]=='o')) continue;
      for(int p = 0; p < so->num_params; p++)
      {
        const dt_ui_param_t *pp = so->param[p];
        memcpy(g_graph.module[m].param + pp->offset, pp->val, dt_ui_param_size(pp->type, pp->cnt));
      }
    }
    dt_graph_history_reset(&g_graph);
    g_history_base = g_graph.history_item_end;
    pthread_mutex_unlock(&g_lock);
    after_graph_change(c); return 1;
  }

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
    if(is_straight(p)) fl |= s_graph_run_all;  // rotate changes ROI -> needs modify_roi re-run
    g_graph.active_module = modid;
    dt_graph_history_append(&g_graph, modid, parid, 2.0);  // throttle: a drag coalesces to one entry
    size_t n = 0; double ms = 0;
    unsigned char *b = render_to_jpeg(&n, &ms, fl);
    pthread_mutex_unlock(&g_lock);
    if(b)
    {
      mg_websocket_write(c, MG_WEBSOCKET_OPCODE_BINARY, (const char *)b, n);
      free(b);
      fprintf(stderr, "[srv] set %d:%d = %g  render %.1f ms  %zu B\n", modid, parid, v, ms, n);
    }
    g_dirty = 1;
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
    if(is_crop(p)) fl |= s_graph_run_all;  // crop changes ROI -> needs modify_roi re-run
    g_graph.active_module = modid;
    dt_graph_history_append(&g_graph, modid, parid, 2.0);  // throttle: a drag coalesces to one entry
    size_t n = 0; double ms = 0;
    unsigned char *b = render_to_jpeg(&n, &ms, fl);
    pthread_mutex_unlock(&g_lock);
    if(b)
    {
      mg_websocket_write(c, MG_WEBSOCKET_OPCODE_BINARY, (const char *)b, n);
      free(b);
      fprintf(stderr, "[srv] setvec %d:%d  render %.1f ms  %zu B\n", modid, parid, ms, n);
    }
    g_dirty = 1;
  }
  return 1;
}

// make a downscaled preview proxy in-process via libjpeg (scaled decode + encode).
// no external process. the full-res source would otherwise be re-decoded per frame
// (~400ms/12MP); editing the proxy keeps the loop fast. returns 0 on success.
struct jpgerr_t { struct jpeg_error_mgr pub; jmp_buf jb; };
static void jpg_err(j_common_ptr ci){ longjmp(((struct jpgerr_t*)ci->err)->jb, 1); }
static int make_proxy(const char *in, const char *out, int maxdim)
{
  FILE *fi = fopen(in, "rb"); if(!fi) return 1;
  struct jpeg_decompress_struct di; struct jpgerr_t de;
  di.err = jpeg_std_error(&de.pub); de.pub.error_exit = jpg_err;
  unsigned char *buf = 0; FILE *fo = 0;
  if(setjmp(de.jb)) { jpeg_destroy_decompress(&di); if(fi)fclose(fi); if(fo)fclose(fo); free(buf); return 1; }
  jpeg_create_decompress(&di); jpeg_stdio_src(&di, fi); jpeg_read_header(&di, TRUE);
  int full = di.image_width > di.image_height ? di.image_width : di.image_height;
  di.scale_num = 1; di.scale_denom = 1;            // libjpeg supports 1/2,1/4,1/8
  while(di.scale_denom < 8 && full/(di.scale_denom*2) >= maxdim) di.scale_denom *= 2;
  jpeg_start_decompress(&di);
  const int w = di.output_width, h = di.output_height, c = di.output_components;
  buf = malloc((size_t)w*h*c);
  while(di.output_scanline < di.output_height){ unsigned char *row = buf + (size_t)di.output_scanline*w*c; jpeg_read_scanlines(&di, &row, 1); }
  jpeg_finish_decompress(&di); jpeg_destroy_decompress(&di); fclose(fi); fi = 0;

  fo = fopen(out, "wb"); if(!fo){ free(buf); return 1; }
  struct jpeg_compress_struct co; struct jpgerr_t ce;
  co.err = jpeg_std_error(&ce.pub); ce.pub.error_exit = jpg_err;
  if(setjmp(ce.jb)) { jpeg_destroy_compress(&co); fclose(fo); free(buf); return 1; }
  jpeg_create_compress(&co); jpeg_stdio_dest(&co, fo);
  co.image_width = w; co.image_height = h; co.input_components = c;
  co.in_color_space = c == 1 ? JCS_GRAYSCALE : JCS_RGB;
  jpeg_set_defaults(&co); jpeg_set_quality(&co, 90, TRUE); jpeg_start_compress(&co, TRUE);
  while(co.next_scanline < co.image_height){ unsigned char *row = buf + (size_t)co.next_scanline*w*c; jpeg_write_scanlines(&co, &row, 1); }
  jpeg_finish_compress(&co); jpeg_destroy_compress(&co); fclose(fo); free(buf);
  return 0;
}

// (re)build the warm graph for `image` (NULL = use the cfg's own input). Tears down any
// previous graph, exports the template pipeline against a downscaled preview proxy,
// restores the image's recipe sidecar (param overlay), and baselines history. Call with
// g_lock held when re-opening at runtime.
static int open_image(const char *image)
{
  // pick pipeline + input by file type: raw -> i-raw graph, fed the ORIGINAL file (vkdt
  // decodes/demosaics it; only the streamed preview is jpeg). jpeg -> i-jpg graph + a
  // small preview proxy. The proxy/decode runs BEFORE the graph teardown, so a bad file
  // leaves the current session intact.
  const int raw = image && is_raw_path(image);
  const char *cfg = raw ? g_conf.cfg_raw : g_conf.cfg;
  const char *inmod = raw ? "i-raw" : "i-jpg";
  char proxypath[1024] = "", abspath[1024]; const char *use_image = image;
  if(image && !raw)
  {
    snprintf(proxypath, sizeof(proxypath), "rabbit_proxy.jpg");
    if(make_proxy(image, proxypath, g_conf.proxy_max) == 0 && access(proxypath, R_OK) == 0)
    { use_image = proxypath; lg("srv", "preview proxy <- %s", image); }
    else { lg("err", "cannot decode %s (unsupported/corrupt?) — keeping current image", image); return 1; }
  }
  else if(raw)
  { // i-raw needs a resolvable path: pass it absolute (modify_roi_out early-returns on an
    // unresolvable filename -> the o-jpg sink gets an uninited size and export fails).
    if(realpath(image, abspath)) use_image = abspath;
    lg("srv", "raw input <- %s", use_image);
  }
  // remember the ORIGINAL (absolute) image for full-res export
  if(image) { char a[1024]; snprintf(g_cur_image, sizeof(g_cur_image), "%s", realpath(image, a) ? a : image); }

  static int built = 0;
  if(built) { dt_graph_history_cleanup(&g_graph); dt_graph_cleanup(&g_graph); }
  dt_graph_init(&g_graph, s_queue_compute);
  snprintf(g_graph.searchpath, sizeof(g_graph.searchpath), ".");
  built = 1;

  const dt_token_t sinkmod = g_conf.preview_webp ? dt_token("o-webp") : dt_token("o-jpg");
  dt_graph_export_t param = {0};
  param.p_cfgfile  = cfg;
  param.output_cnt = 2;
  param.output[0].inst             = dt_token("main");
  param.output[0].mod              = sinkmod;
  param.output[0].p_filename       = g_jpgbase;
  param.output[0].quality          = g_conf.quality;
  param.output[0].colour_primaries = s_colour_primaries_srgb;
  param.output[0].colour_trc       = s_colour_trc_srgb;
  param.output[0].max_width        = g_conf.preview_max;
  param.output[0].max_height       = g_conf.preview_max;
  // second sink: the waveform histogram (display:hist in the cfg). same format as the
  // preview; streamed to the client's histogram widget on request (cmd `scope`).
  param.output[1].inst             = dt_token("hist");
  param.output[1].mod              = sinkmod;
  param.output[1].p_filename       = g_histbase;
  param.output[1].quality          = g_conf.quality;
  param.output[1].colour_primaries = s_colour_primaries_srgb;
  param.output[1].colour_trc       = s_colour_trc_srgb;

  char imgline[1280]; char *extra[1];
  if(image)
  {
    snprintf(imgline, sizeof(imgline), "param:%s:main:filename:%s", inmod, use_image);
    extra[0] = imgline; param.extra_param_cnt = 1; param.p_extra_param = extra;
  }
  if(dt_graph_export(&g_graph, &param) != VK_SUCCESS)
  { lg("err", "graph export failed for '%s'", cfg); return 1; }
  snprintf(g_jpgpath,  sizeof(g_jpgpath),  g_conf.preview_webp ? "%s.webp" : "%s.jpg", g_jpgbase);
  snprintf(g_histpath, sizeof(g_histpath), g_conf.preview_webp ? "%s.webp" : "%s.jpg", g_histbase);

  // restore this image's recipe (param overlay onto the template; skip i-jpg input)
  g_recipe_path[0] = 0;
  if(image)
  {
    snprintf(g_recipe_path, sizeof(g_recipe_path), "%s.cfg", image);
    if(access(g_recipe_path, R_OK) == 0)
    {
      FILE *rf = fopen(g_recipe_path, "r");
      if(rf)
      {
        char line[4096]; int applied = 0;
        while(fgets(line, sizeof(line), rf))
        {
          line[strcspn(line, "\r\n")] = 0;
          if(strncmp(line, "param:", 6)) continue;
          if(!strncmp(line, "param:i-jpg:", 12) || !strncmp(line, "param:i-raw:", 12)) continue;  // server-managed input
          if(dt_graph_read_config_line(&g_graph, line) == 0) applied++;
        }
        fclose(rf);
        size_t n = 0; double ms = 0; unsigned char *b = render_to_jpeg(&n, &ms, s_graph_run_all); free(b);
        lg("srv", "recipe restored: %d params from %s", applied, g_recipe_path);
      }
    }
  }
  dt_graph_history_init(&g_graph);
  dt_graph_history_reset(&g_graph);
  g_history_base = g_graph.history_item_end;
  build_menu_json();
  return 0;
}

// persist the current edit as a vkdt .cfg sidecar next to the image. The recipe is
// server-authoritative; the phone holds nothing canonical. (See wiki: Session, Recipe
// & Library Model.) The written .cfg references the preview proxy as input — on load we
// only overlay the param: lines onto the freshly built template pipeline.
static void recipe_save(void)
{
  if(!g_recipe_path[0]) return;
  pthread_mutex_lock(&g_lock);
  int err = dt_graph_write_config_ascii(&g_graph, g_recipe_path);
  pthread_mutex_unlock(&g_lock);
  lg(err ? "err" : "srv", "recipe %s -> %s", err ? "WRITE FAILED" : "saved", g_recipe_path);
}
// debounced autosave: write at most every ~2s while there are pending edits.
static void *autosave_thread(void *u)
{
  (void)u;
  while(!g_stop) { sleep(2); if(g_dirty) { g_dirty = 0; recipe_save(); } }
  return NULL;
}

// HTTP POST /upload?name=foo.jpg — save the raw body into the library dir; returns the
// stored basename. The client then sends a WS `open <name>` to switch to it.
static int upload_handler(struct mg_connection *c, void *u)
{
  (void)u;
  const struct mg_request_info *ri = mg_get_request_info(c);
  char name[256] = "upload.jpg";
  if(ri->query_string) mg_get_var(ri->query_string, strlen(ri->query_string), "name", name, sizeof(name));
  char *bn = name; for(char *p = name; *p; p++) if(*p=='/' || *p=='\\') bn = p+1;  // basename only
  if(!*bn || strstr(bn, "..")) bn = "upload.jpg";
  char path[1024]; snprintf(path, sizeof(path), "%s/%s", g_conf.libdir, bn);
  FILE *f = fopen(path, "wb");
  if(!f) { lg("err", "upload: cannot open %s", path); mg_printf(c, "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n"); return 500; }
  char buf[1<<16]; int n; long long total = 0;
  while((n = mg_read(c, buf, sizeof(buf))) > 0) { fwrite(buf, 1, n, f); total += n; }
  fclose(f);
  lg("srv", "upload %s (%lld B)", path, total);
  mg_printf(c, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nCache-Control: no-store\r\n"
               "Access-Control-Allow-Origin: *\r\nContent-Length: %zu\r\n\r\n%s", strlen(bn), bn);
  return 200;
}

// HTTP GET /export?q=92&max=0 : render the current edit at full resolution from the
// ORIGINAL image (no preview proxy) in a fresh graph, return as a JPEG download.
// q = jpeg quality, max = longest edge (0 = native). (WebP/EXR: future.)
static int export_handler(struct mg_connection *c, void *u)
{
  (void)u;
  const struct mg_request_info *ri = mg_get_request_info(c);
  char qs[8] = "92", ms[8] = "0";
  if(ri->query_string)
  {
    mg_get_var(ri->query_string, strlen(ri->query_string), "q",   qs, sizeof(qs));
    mg_get_var(ri->query_string, strlen(ri->query_string), "max", ms, sizeof(ms));
  }
  int quality = atoi(qs); if(quality < 1 || quality > 100) quality = 92;
  int maxdim = atoi(ms);  if(maxdim < 0) maxdim = 0;
  if(!g_cur_image[0]) { mg_printf(c, "HTTP/1.1 409 Conflict\r\nContent-Length: 0\r\n\r\n"); return 409; }
  if(g_dirty) recipe_save();

  pthread_mutex_lock(&g_lock);
  const int raw = is_raw_path(g_cur_image);
  dt_graph_t ex; dt_graph_init(&ex, s_queue_compute);
  snprintf(ex.searchpath, sizeof(ex.searchpath), ".");
  dt_graph_export_t p = {0};
  p.p_cfgfile  = raw ? g_conf.cfg_raw : g_conf.cfg;
  p.output_cnt = 2;
  p.output[0].inst             = dt_token("main");
  p.output[0].mod              = dt_token("o-jpg");
  p.output[0].p_filename       = "rabbit_export";
  p.output[0].quality          = quality;
  p.output[0].colour_primaries = s_colour_primaries_srgb;
  p.output[0].colour_trc       = s_colour_trc_srgb;
  p.output[0].max_width        = 0;   // size is controlled by the resize module below
  p.output[0].max_height       = 0;
  // the cfg has a display:hist branch; replace it with a throwaway sink too so the
  // headless export graph has no leftover display node. (the histogram file is ignored.)
  p.output[1].inst             = dt_token("hist");
  p.output[1].mod              = dt_token("o-jpg");
  p.output[1].p_filename       = "rabbit_export_hist";
  p.output[1].quality          = quality;
  p.output[1].colour_primaries = s_colour_primaries_srgb;
  p.output[1].colour_trc       = s_colour_trc_srgb;
  char imgline[1280];
  snprintf(imgline, sizeof(imgline), "param:%s:main:filename:%s", raw ? "i-raw" : "i-jpg", g_cur_image);
  char *extra[1] = { imgline }; p.extra_param_cnt = 1; p.p_extra_param = extra;

  int ok = 0;
  if(dt_graph_export(&ex, &p) == VK_SUCCESS)
  { // overlay the current recipe (skip input + output module params), then re-render
    if(g_recipe_path[0])
    {
      FILE *rf = fopen(g_recipe_path, "r");
      if(rf)
      {
        char line[4096];
        while(fgets(line, sizeof(line), rf))
        {
          line[strcspn(line, "\r\n")] = 0;
          if(strncmp(line, "param:", 6)) continue;
          if(!strncmp(line, "param:i-jpg:", 12) || !strncmp(line, "param:i-raw:", 12) || !strncmp(line, "param:o-", 8)) continue;
          dt_graph_read_config_line(&ex, line);
        }
        fclose(rf);
      }
    }
    // override the preview resize cap so export is full-res: resize<=maxdim (0 = native).
    // resize only ever downscales (scale=MIN(1,...)), so this never upscales.
    char rz[64];
    snprintf(rz, sizeof(rz), "param:resize:01:width:%d", maxdim);  dt_graph_read_config_line(&ex, rz);
    snprintf(rz, sizeof(rz), "param:resize:01:height:%d", maxdim); dt_graph_read_config_line(&ex, rz);
    ex.runflags = s_graph_run_all | s_graph_run_download_sink | s_graph_run_wait_done | s_graph_run_record_cmd_buf;
    if(dt_graph_run(&ex, ex.runflags) == VK_SUCCESS) ok = 1;
  }
  dt_graph_cleanup(&ex);
  pthread_mutex_unlock(&g_lock);

  unsigned char *buf = 0; long n = 0;
  if(ok)
  {
    FILE *f = fopen("rabbit_export.jpg", "rb");
    if(f) { fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
            if(n > 0) { buf = malloc(n); if(fread(buf, 1, n, f) != (size_t)n) { free(buf); buf = 0; } } fclose(f); }
  }
  if(!buf) { lg("err", "export failed"); mg_printf(c, "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n"); return 500; }
  const char *base = strrchr(g_cur_image, '/'); base = base ? base + 1 : g_cur_image;
  mg_printf(c, "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nCache-Control: no-store\r\n"
               "Content-Disposition: attachment; filename=\"rabbit_%s.jpg\"\r\nContent-Length: %ld\r\n\r\n", base, n);
  mg_write(c, (const char *)buf, n); free(buf);
  lg("srv", "export %s.jpg %ld B (q%d max%d)", base, n, quality, maxdim);
  return 200;
}

static void on_sigint(int s) { (void)s; g_stop = 1; }

int main(int argc, char *argv[])
{
  // config: a single .conf path, else ./rabbit.conf with legacy positional overrides
  // (docroot port cfg image) so existing launch scripts keep working.
  if(argc == 2 && strstr(argv[1], ".conf")) load_config(argv[1]);
  else
  {
    load_config("rabbit.conf");
    if(argc > 1) snprintf(g_conf.docroot, sizeof(g_conf.docroot), "%s", argv[1]);
    if(argc > 2) snprintf(g_conf.port,    sizeof(g_conf.port),    "%s", argv[2]);
    if(argc > 3) snprintf(g_conf.cfg,     sizeof(g_conf.cfg),     "%s", argv[3]);
    if(argc > 4) snprintf(g_conf.image,   sizeof(g_conf.image),   "%s", argv[4]);
  }
  if(g_conf.logfile[0]) g_logf = fopen(g_conf.logfile, "a");

  dt_log_init(s_log_cli);
  dt_pipe_global_init();
  threads_global_init();
  if(qvk_init(0, -1, 0, 0, 0)) { lg("err", "qvk_init failed"); return 1; }

  mkdir(g_conf.libdir, 0755);            // library / upload directory (relative to cwd)
  const char *image = g_conf.image[0] ? g_conf.image : NULL;
  if(open_image(image)) { lg("err", "initial open failed"); return 1; }
  lg("srv", "graph warm. menu=%zu B  libdir=%s  log=%s  preview=%s", strlen(g_menu_json), g_conf.libdir, g_conf.logfile, g_conf.preview_webp ? "webp" : "jpeg");

  signal(SIGINT, on_sigint);
  mg_init_library(0);
  const char *opts[] = {
    "document_root",   g_conf.docroot,
    "listening_ports", g_conf.port,
    "num_threads",     "4",
    "enable_directory_listing", "no",
    "tcp_nodelay",     "1",
    "static_file_max_age", "0",
    "additional_header", "Cache-Control: no-store",  // never cache the client during dev
    NULL
  };
  struct mg_callbacks cb; memset(&cb, 0, sizeof(cb));
  struct mg_context *ctx = mg_start(&cb, NULL, opts);
  if(!ctx) { lg("err", "mg_start failed (port %s)", g_conf.port); return 1; }
  mg_set_websocket_handler(ctx, "/ws", ws_connect, ws_ready, ws_data, ws_close, NULL);
  mg_set_request_handler(ctx, "/upload", upload_handler, NULL);
  mg_set_request_handler(ctx, "/export", export_handler, NULL);

  pthread_t as; pthread_create(&as, NULL, autosave_thread, NULL);  // debounced recipe autosave

  lg("srv", "listening on :%s  docroot=%s  (Ctrl-C to stop)", g_conf.port, g_conf.docroot);
  while(!g_stop) sleep(1);

  lg("srv", "shutting down");
  pthread_join(as, NULL);
  if(g_dirty) recipe_save();  // flush any pending edit
  mg_stop(ctx);
  mg_exit_library();
  dt_graph_history_cleanup(&g_graph);
  dt_graph_cleanup(&g_graph);
  threads_global_cleanup();
  qvk_cleanup();
  if(g_logf) fclose(g_logf);
  return 0;
}
