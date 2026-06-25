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
#include "cfg_rewire.h"   // pure rewire cfg-transform (shared with rewire_test.c)

static dt_graph_t      g_graph;
static pthread_mutex_t g_lock   = PTHREAD_MUTEX_INITIALIZER;
static const char     *g_jpgbase = "preview";
static char            g_jpgpath[512];
static const char     *g_histbase = "histogram";  // second sink: the waveform histogram image
static char            g_histpath[512];
static char            g_bypass[16][48];           // bypassed modules ("name:inst") — routed around in the cfg
static int             g_bypass_cnt = 0;
// user-added modules spliced into an edge (inverse of bypass): each is inserted right after
// `after` ("name:inst") on that module's `output` connector, fanning out to all its consumers.
static struct { char mod[40]; char inst[16]; char after[48]; } g_insert[16];
static int             g_insert_cnt = 0;
// user re-wirings: each repoints a module's input connector to a new output source. stored fully
// resolved ("name:inst:conn") so they survive graph rebuilds (modids aren't stable, tokens are).
static rb_rewire_t     g_rewire[16];
static int             g_rewire_cnt = 0;
static volatile int    g_stop = 0;
static char            g_menu_json[65536];
static char            g_recipe_path[1024];   // server-authoritative recipe (vkdt .cfg sidecar)
static char            g_cur_image[1024];      // current ORIGINAL image (not the proxy) — for full-res export
static volatile int    g_dirty = 0;           // edits pending an autosave
static int             g_export_ver = 0;       // per-image export version counter (reset on open)
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
static int  is_inserted(const char *ni);      // is "name:inst" a user-added (spliced-in) module?

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
static inline int is_combo(const dt_ui_param_t *p)
{
  return p->widget.type == dt_token("combo") && p->type == dt_token("int") && p->cnt == 1 && p->widget.data;
}
static inline int is_editable(const dt_ui_param_t *p){ return is_radial_slider(p) || is_wheel(p) || is_crop(p) || is_straight(p) || is_combo(p); }

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
      else if(is_combo(p))
      { // int param with a combo widget -> a toggle/cycle. options are \0-separated in widget.data
        const int cur = *(const int *)((uint8_t *)g_graph.module[m].param + p->offset);
        o += snprintf(o, e-o, "\",\"kind\":\"combo\",\"parid\":%d,\"cur\":%d,\"options\":[", pi, cur);
        const char *opt = (const char *)p->widget.data; int firsto = 1;
        while(*opt) { o += snprintf(o, e-o, "%s\"%s\"", firsto ? "" : ",", opt); firsto = 0; opt += strlen(opt) + 1; }
        o += snprintf(o, e-o, "]}");
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
  o += snprintf(o, e-o, "],\"bypassed\":[");   // routed-out modules (not in the graph) so the client can re-enable them
  for(int i = 0; i < g_bypass_cnt; i++) o += snprintf(o, e-o, "%s\"%s\"", i ? "," : "", g_bypass[i]);
  o += snprintf(o, e-o, "],\"inserted\":[");   // user-added modules ("name:inst") so the client can mark + remove them
  for(int i = 0; i < g_insert_cnt; i++) o += snprintf(o, e-o, "%s\"%s:%s\"", i ? "," : "", g_insert[i].mod, g_insert[i].inst);
  o += snprintf(o, e-o, "]}");
  pthread_mutex_unlock(&g_lock);
  mg_websocket_write(c, MG_WEBSOCKET_OPCODE_TEXT, buf, o-buf);
}

// send the catalog of modules the client can ADD: every loaded module class that has a clear
// input->output chain (has_inout_chain) — i.e. exactly the modules that can be spliced inline
// onto an edge. excludes sources/sinks/display and multi-output modules automatically.
static void send_mods(struct mg_connection *c)
{
  static char buf[32768];
  char *o = buf, *e = buf + sizeof(buf);
  o += snprintf(o, e-o, "{\"type\":\"mods\",\"mods\":[");
  int first = 1;
  for(uint32_t i = 0; i < dt_pipe.num_modules && o < e; i++)
  {
    if(!dt_pipe.module[i].has_inout_chain) continue;
    if(!first) o += snprintf(o, e-o, ","); first = 0;
    if(o < e) o += snprintf(o, e-o, "\"%"PRItkn"\"", dt_token_str(dt_pipe.module[i].name));
  }
  if(o < e) o += snprintf(o, e-o, "]}");
  // snprintf returns the would-be length, so o can run past e on overflow; clamp the write.
  mg_websocket_write(c, MG_WEBSOCKET_OPCODE_TEXT, buf, o > e ? (size_t)(e-buf) : (size_t)(o-buf));
}

// find a live module by "name:inst"; returns its modid or -1.
static int find_modid(const char *nameinst)
{
  char ni[64]; snprintf(ni, sizeof(ni), "%s", nameinst);
  char *colon = strrchr(ni, ':'); if(!colon) return -1; *colon = 0;
  dt_token_t nm = dt_token(ni), in = dt_token(colon + 1);
  for(int m = 0; m < g_graph.num_modules; m++)
    if(g_graph.module[m].so && g_graph.module[m].name == nm && g_graph.module[m].inst == in) return m;
  return -1;
}

// is `name` a module class that can be added (loaded + has an input->output chain)?
static int is_addable(const char *name)
{
  dt_token_t nm = dt_token(name);
  for(uint32_t i = 0; i < dt_pipe.num_modules; i++)
    if(dt_pipe.module[i].has_inout_chain && dt_pipe.module[i].name == nm) return 1;
  return 0;
}

// pick a free 2-digit instance for `name` not used by any live module or pending insert
static void gen_inst(const char *name, char *out, int outsz)
{
  dt_token_t nm = dt_token(name);
  for(int k = 1; k < 100; k++)
  {
    char cand[8]; snprintf(cand, sizeof(cand), "%02d", k);
    dt_token_t ct = dt_token(cand);
    int used = 0;
    for(int m = 0; m < g_graph.num_modules && !used; m++)
      if(g_graph.module[m].so && g_graph.module[m].name == nm && g_graph.module[m].inst == ct) used = 1;
    for(int i = 0; i < g_insert_cnt && !used; i++)
      if(!strcmp(g_insert[i].mod, name) && !strcmp(g_insert[i].inst, cand)) used = 1;
    if(!used) { snprintf(out, outsz, "%s", cand); return; }
  }
  snprintf(out, outsz, "99");
}

// does following output edges downstream from `start` ever reach `target`? used as a cycle
// guard before a rewire: adding the edge `target.input <- start.output` would close a loop iff
// `start` is already reachable downstream from `target` (target ->* start). consumers are found
// by scanning every module's input connectors for `connected.i == m`, so ALL output connectors
// are followed (not only the canonically-named `output`). `seen` is marked at push, so each
// module enters the stack at most once -> stack bounded by num_modules. call with g_lock held.
static int reaches_downstream(int start, int target)
{
  static char seen[4096]; static int stack[4096];
  if(g_graph.num_modules > (int)(sizeof(seen)/sizeof(seen[0]))) return 1;   // pathological: refuse rather than overrun
  memset(seen, 0, sizeof(seen));
  int sp = 0; seen[start] = 1; stack[sp++] = start;
  while(sp > 0)
  {
    int m = stack[--sp];
    if(m == target) return 1;
    for(int x = 0; x < g_graph.num_modules; x++)   // x is a consumer of m if any of its inputs is fed by m
    {
      dt_module_t *mx = g_graph.module + x; if(!mx->so || seen[x]) continue;
      for(int cc = 0; cc < mx->num_connectors; cc++)
        if(dt_connector_input(mx->connector + cc) && mx->connector[cc].connected.i == m)
        { seen[x] = 1; stack[sp++] = x; break; }
    }
  }
  return 0;
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
  if(!strncmp(tmp, "mods", 4))  { send_mods(c);  return 1; }   // catalog of addable modules
  { int mi;   // fitcrop <modid> : auto-fit the crop to the rotated inscribed rect (vkdt's own geometry, item 32 v2)
    if(sscanf(tmp, "fitcrop %d", &mi) == 1 && mi >= 0 && mi < g_graph.num_modules)
    {
      pthread_mutex_lock(&g_lock);
      dt_module_t *m = g_graph.module + mi;
      // guard: only the crop module's ui_callback does the inscribed-crop fit. other modules
      // also define ui_callback with unrelated (graph-mutating) effects (e.g. pick) — don't run those.
      if(m->so && m->name == dt_token("crop") && m->so->ui_callback)
      {
        m->so->ui_callback(m, dt_token("crop"));   // writes the inscribed crop into the crop param
        const int cp = dt_module_get_param(m->so, dt_token("crop"));
        if(cp >= 0) { g_graph.active_module = mi; dt_graph_history_append(&g_graph, mi, cp, 0.0); }
      }
      pthread_mutex_unlock(&g_lock);
      after_graph_change(c); return 1;   // re-render + push menu (new crop cur) + history
    }
  }
  { char bn[48]; int on;   // bypass <name:inst> <0|1> : route a module out of / back into the graph (cfg rewrite + reload)
    if(sscanf(tmp, "bypass %47s %d", bn, &on) == 2)
    {
      // never bypass input/output/display modules — routing those out has no valid splice
      if(!strncmp(bn, "i-", 2) || !strncmp(bn, "o-", 2) || !strncmp(bn, "display", 7))
      { lg("err", "refusing to bypass %s", bn); return 1; }
      // snapshot for rollback if the rebuild fails (don't leave a stuck broken session)
      char save[16][48]; int savecnt = g_bypass_cnt; memcpy(save, g_bypass, sizeof(save));
      int idx = -1; for(int i = 0; i < g_bypass_cnt; i++) if(!strcmp(g_bypass[i], bn)) idx = i;
      if(on && idx < 0 && g_bypass_cnt < 16) snprintf(g_bypass[g_bypass_cnt++], 48, "%s", bn);
      else if(!on && idx >= 0) { g_bypass[idx][0] = 0; for(int i = idx; i < g_bypass_cnt - 1; i++) memcpy(g_bypass[i], g_bypass[i+1], 48); g_bypass_cnt--; }
      pthread_mutex_lock(&g_lock); int err = open_image(g_cur_image[0] ? g_cur_image : NULL); pthread_mutex_unlock(&g_lock);
      if(err)
      { // revert the bypass set and rebuild the last good graph
        lg("err", "bypass rebuild failed for %s — reverting", bn);
        memcpy(g_bypass, save, sizeof(save)); g_bypass_cnt = savecnt;
        pthread_mutex_lock(&g_lock); open_image(g_cur_image[0] ? g_cur_image : NULL); pthread_mutex_unlock(&g_lock);
      }
      lg("srv", "bypass %s = %d (%d total)", bn, on, g_bypass_cnt);
      after_graph_change(c); send_graph(c);
      return 1;
    }
  }
  { char nm[40], af[48];   // addmod <name> <after:inst> : splice a new module onto after's output edge (inverse of bypass)
    if(sscanf(tmp, "addmod %39s %47s", nm, af) == 2)
    {
      if(!is_addable(nm)) { lg("err", "addmod: unknown/non-chain module %s", nm); return 1; }
      // require that `after` exists and its output feeds at least one consumer — else the new
      // module would be an orphan (no edge to splice into).
      pthread_mutex_lock(&g_lock);
      int mi = find_modid(af), consumers = 0;
      if(mi >= 0) { int mo[16], co[16]; consumers = dt_module_get_module_after(&g_graph, g_graph.module + mi, mo, co, 16); }
      pthread_mutex_unlock(&g_lock);
      if(consumers <= 0) { lg("err", "addmod: %s has no output consumer to splice into", af); return 1; }
      if(g_insert_cnt >= 16) { lg("err", "addmod: insert limit reached"); return 1; }
      char inst[16]; gen_inst(nm, inst, sizeof(inst));
      int idx = g_insert_cnt;
      snprintf(g_insert[idx].mod,   sizeof(g_insert[idx].mod),   "%s", nm);
      snprintf(g_insert[idx].inst,  sizeof(g_insert[idx].inst),  "%s", inst);
      snprintf(g_insert[idx].after, sizeof(g_insert[idx].after), "%s", af);
      g_insert_cnt++;
      pthread_mutex_lock(&g_lock); int err = open_image(g_cur_image[0] ? g_cur_image : NULL); pthread_mutex_unlock(&g_lock);
      if(err)
      { // incompatible connectors / bad splice -> revert and rebuild the last good graph
        lg("err", "addmod rebuild failed for %s after %s — reverting", nm, af);
        g_insert_cnt--;
        pthread_mutex_lock(&g_lock); open_image(g_cur_image[0] ? g_cur_image : NULL); pthread_mutex_unlock(&g_lock);
      }
      else lg("srv", "addmod %s:%s after %s (%d total)", nm, inst, af, g_insert_cnt);
      after_graph_change(c); send_graph(c);
      return 1;
    }
  }
  { char ni[48];   // rmmod <name:inst> : remove a user-added (inserted) module (template modules use bypass instead)
    if(sscanf(tmp, "rmmod %47s", ni) == 1)
    {
      if(!is_inserted(ni)) { lg("err", "rmmod: %s is not a user-added module", ni); return 1; }
      struct { char mod[40]; char inst[16]; char after[48]; } save[16]; int savecnt = g_insert_cnt;
      memcpy(save, g_insert, sizeof(save));
      for(int i = 0; i < g_insert_cnt; i++)
      { char k[64]; snprintf(k, sizeof(k), "%s:%s", g_insert[i].mod, g_insert[i].inst);
        if(!strcmp(k, ni)) { for(int j = i; j < g_insert_cnt - 1; j++) g_insert[j] = g_insert[j+1]; g_insert_cnt--; break; } }
      pthread_mutex_lock(&g_lock); int err = open_image(g_cur_image[0] ? g_cur_image : NULL); pthread_mutex_unlock(&g_lock);
      if(err)
      { lg("err", "rmmod rebuild failed for %s — reverting", ni);
        memcpy(g_insert, save, sizeof(save)); g_insert_cnt = savecnt;
        pthread_mutex_lock(&g_lock); open_image(g_cur_image[0] ? g_cur_image : NULL); pthread_mutex_unlock(&g_lock); }
      else lg("srv", "rmmod %s (%d total)", ni, g_insert_cnt);
      after_graph_change(c); send_graph(c);
      return 1;
    }
  }
  { int tm, fm;   // rewire <to_modid> <from_modid> : repoint to's main input to from's main output
    if(sscanf(tmp, "rewire %d %d", &tm, &fm) == 2)
    {
      pthread_mutex_lock(&g_lock);
      int ok = 1; char tokey[64] = "", fromval[80] = "";
      if(tm < 0 || tm >= g_graph.num_modules || fm < 0 || fm >= g_graph.num_modules || tm == fm) ok = 0;
      else if(!g_graph.module[tm].so || !g_graph.module[fm].so) ok = 0;
      else
      { // resolve to's first input connector and from's first output connector -> "name:inst:conn"
        dt_module_t *mt = g_graph.module + tm, *mf = g_graph.module + fm;
        int tc = -1, fc = -1;
        // resolve to's input as the first CONNECTED input (the edge the client's input dot stands
        // for) — not merely index 0, so the rewrite drops the real existing connect rather than
        // appending a second edge on a different (unconnected) input of a multi-input module.
        for(int cc = 0; cc < mt->num_connectors; cc++) if(dt_connector_input (mt->connector + cc) && mt->connector[cc].connected.i >= 0) { tc = cc; break; }
        for(int cc = 0; cc < mf->num_connectors; cc++) if(dt_connector_output(mf->connector + cc)) { fc = cc; break; }
        if(tc < 0 || fc < 0) ok = 0;                            // to has no connected input / from has no output
        else if(reaches_downstream(tm, fm)) ok = 0;             // cycle guard: from is downstream of to
        else
        {
          snprintf(tokey,   sizeof(tokey),   "%"PRItkn":%"PRItkn":%"PRItkn,
              dt_token_str(mt->name), dt_token_str(mt->inst), dt_token_str(mt->connector[tc].name));
          snprintf(fromval, sizeof(fromval), "%"PRItkn":%"PRItkn":%"PRItkn,
              dt_token_str(mf->name), dt_token_str(mf->inst), dt_token_str(mf->connector[fc].name));
        }
      }
      pthread_mutex_unlock(&g_lock);
      if(!ok) { lg("err", "rewire %d <- %d rejected (bad id / no connector / cycle)", tm, fm); send_graph(c); return 1; }
      // snapshot for rollback; dedupe by `to` (an input has exactly one source) — last wins
      rb_rewire_t save[16]; int savecnt = g_rewire_cnt; memcpy(save, g_rewire, sizeof(save));
      int idx = -1; for(int i = 0; i < g_rewire_cnt; i++) if(!strcmp(g_rewire[i].to, tokey)) idx = i;
      if(idx < 0 && g_rewire_cnt < 16) idx = g_rewire_cnt++;
      if(idx < 0) { lg("err", "rewire: limit reached"); send_graph(c); return 1; }
      snprintf(g_rewire[idx].to, sizeof(g_rewire[idx].to), "%s", tokey);
      snprintf(g_rewire[idx].from, sizeof(g_rewire[idx].from), "%s", fromval);
      pthread_mutex_lock(&g_lock); int err = open_image(g_cur_image[0] ? g_cur_image : NULL); pthread_mutex_unlock(&g_lock);
      if(err)
      { // incompatible connectors / bad rewrite -> revert and rebuild the last good graph
        lg("err", "rewire rebuild failed (%s <- %s) — reverting", tokey, fromval);
        memcpy(g_rewire, save, sizeof(save)); g_rewire_cnt = savecnt;
        pthread_mutex_lock(&g_lock); open_image(g_cur_image[0] ? g_cur_image : NULL); pthread_mutex_unlock(&g_lock);
      }
      else lg("srv", "rewire %s <- %s (%d total)", tokey, fromval, g_rewire_cnt);
      after_graph_change(c); send_graph(c);
      return 1;
    }
  }
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

  // int set (combo toggles): "I <modid> <parid> <ival>"
  { int im, ip, iv;
    if(sscanf(tmp, "I %d %d %d", &im, &ip, &iv) == 3 && im >= 0 && im < g_graph.num_modules)
    {
      pthread_mutex_lock(&g_lock);
      dt_module_t *m = g_graph.module + im;
      const dt_ui_param_t *p = m->so->param[ip];
      int *val = (int *)((uint8_t *)m->param + p->offset);
      int old = *val; *val = iv;
      dt_graph_run_t fl = s_graph_run_none;
      if(m->so->check_params) fl = m->so->check_params(m, ip, 0, &old);
      g_graph.active_module = im;
      dt_graph_history_append(&g_graph, im, ip, 2.0);
      size_t n = 0; double ms = 0;
      unsigned char *b = render_to_jpeg(&n, &ms, fl);
      pthread_mutex_unlock(&g_lock);
      if(b) { mg_websocket_write(c, MG_WEBSOCKET_OPCODE_BINARY, (const char *)b, n); free(b);
              fprintf(stderr, "[srv] setint %d:%d = %d  render %.1f ms  %zu B\n", im, ip, iv, ms, n); }
      g_dirty = 1;
      return 1;
    }
  }
  return 1;
}

// make a downscaled preview proxy in-process via libjpeg (scaled decode + encode).
// no external process. the full-res source would otherwise be re-decoded per frame
// (~400ms/12MP); editing the proxy keeps the loop fast. returns 0 on success.
struct jpgerr_t { struct jpeg_error_mgr pub; jmp_buf jb; };
static void jpg_err(j_common_ptr ci){ longjmp(((struct jpgerr_t*)ci->err)->jb, 1); }

// read the EXIF Orientation tag (0x0112) from the saved APP1 marker. returns 1..8 (1=normal).
// vkdt's i-jpg doesn't apply EXIF orientation, so we bake it into the preview proxy instead.
static int exif_orientation(j_decompress_ptr di)
{
  for(jpeg_saved_marker_ptr m = di->marker_list; m; m = m->next)
  {
    if(m->marker != JPEG_APP0 + 1 || m->data_length < 14) continue;
    const unsigned char *d = m->data; if(memcmp(d, "Exif\0\0", 6)) continue;
    const unsigned char *t = d + 6; const unsigned len = m->data_length - 6;
    const int le = t[0] == 'I';
    #define E16(p) (le ? ((p)[0] | ((p)[1]<<8)) : (((p)[0]<<8) | (p)[1]))
    #define E32(p) (le ? ((p)[0] | ((p)[1]<<8) | ((unsigned)(p)[2]<<16) | ((unsigned)(p)[3]<<24)) : (((unsigned)(p)[0]<<24) | ((p)[1]<<16) | ((p)[2]<<8) | (p)[3]))
    unsigned ifd = E32(t + 4); if(ifd + 2 > len) continue;
    const int n = E16(t + ifd);
    for(int i = 0; i < n; i++)
    {
      const unsigned char *e = t + ifd + 2 + (unsigned)i * 12;
      if(e + 12 > d + m->data_length) break;
      if(E16(e) == 0x0112) { const int v = E16(e + 8); return (v >= 1 && v <= 8) ? v : 1; }
    }
    #undef E16
    #undef E32
  }
  return 1;
}

// rotate an interleaved w*h*c buffer to upright per the EXIF orientation (handles the
// 90/180/270 rotations 3/6/8; mirrored orientations are rare from cameras and left as-is).
// updates *pw/*ph; returns the new buffer (frees the old) or the original on no-op/oom.
static unsigned char *apply_orientation(unsigned char *buf, int *pw, int *ph, int c, int orient)
{
  if(orient != 3 && orient != 6 && orient != 8) return buf;
  const int w = *pw, h = *ph;
  const int nw = (orient == 3) ? w : h, nh = (orient == 3) ? h : w;
  unsigned char *o = malloc((size_t)nw * nh * c); if(!o) return buf;
  for(int dy = 0; dy < nh; dy++) for(int dx = 0; dx < nw; dx++)
  {
    int sx, sy;
    if(orient == 3)      { sx = w - 1 - dx; sy = h - 1 - dy; }   // 180
    else if(orient == 6) { sx = dy;         sy = h - 1 - dx; }   // 90 CW
    else                 { sx = w - 1 - dy; sy = dx;         }   // 90 CCW (8)
    memcpy(o + ((size_t)dy * nw + dx) * c, buf + ((size_t)sy * w + sx) * c, c);
  }
  free(buf); *pw = nw; *ph = nh; return o;
}

static int make_proxy(const char *in, const char *out, int maxdim)
{
  FILE *fi = fopen(in, "rb"); if(!fi) return 1;
  struct jpeg_decompress_struct di; struct jpgerr_t de;
  di.err = jpeg_std_error(&de.pub); de.pub.error_exit = jpg_err;
  unsigned char *buf = 0; FILE *fo = 0;
  if(setjmp(de.jb)) { jpeg_destroy_decompress(&di); if(fi)fclose(fi); if(fo)fclose(fo); free(buf); return 1; }
  jpeg_create_decompress(&di); jpeg_stdio_src(&di, fi);
  jpeg_save_markers(&di, JPEG_APP0 + 1, 0xFFFF);   // keep the EXIF marker so we can read orientation
  jpeg_read_header(&di, TRUE);
  const int orient = exif_orientation(&di);
  int full = di.image_width > di.image_height ? di.image_width : di.image_height;
  di.scale_num = 1; di.scale_denom = 1;            // libjpeg supports 1/2,1/4,1/8
  while(di.scale_denom < 8 && full/(di.scale_denom*2) >= maxdim) di.scale_denom *= 2;
  jpeg_start_decompress(&di);
  int w = di.output_width, h = di.output_height; const int c = di.output_components;
  buf = malloc((size_t)w*h*c);
  while(di.output_scanline < di.output_height){ unsigned char *row = buf + (size_t)di.output_scanline*w*c; jpeg_read_scanlines(&di, &row, 1); }
  jpeg_finish_decompress(&di); jpeg_destroy_decompress(&di); fclose(fi); fi = 0;
  buf = apply_orientation(buf, &w, &h, c, orient);   // bake EXIF orientation into the proxy (vkdt i-jpg ignores it)

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
// is "name:inst" in the bypass set?
static int is_bypassed(const char *ni)
{
  for(int i = 0; i < g_bypass_cnt; i++) if(!strcmp(g_bypass[i], ni)) return 1;
  return 0;
}

// is "name:inst" in the user-added (inserted) set?
static int is_inserted(const char *ni)
{
  for(int i = 0; i < g_insert_cnt; i++)
  { char k[64]; snprintf(k, sizeof(k), "%s:%s", g_insert[i].mod, g_insert[i].inst); if(!strcmp(k, ni)) return 1; }
  return 0;
}

// splice the user-added modules into the cfg text (in place, in cap bytes): for each insert
// "NEW after AFTER:inst", add NEW's module line and re-point every consumer of AFTER's `output`
// connector through NEW (fan-out safe). NEW's module line is emitted FIRST so every connect
// referencing NEW comes after its declaration (vkdt resolves modules in cfg order).
// returns 1 if the rewrite overflowed its scratch buffer (cfg would be truncated/malformed -> caller must treat as an error and roll back), else 0.
static int apply_inserts(char *text, size_t cap)
{
  for(int j = 0; j < g_insert_cnt; j++)
  {
    static char work[65536]; snprintf(work, sizeof(work), "%s", text);   // strtok destroys; work on a copy
    char *line[2048]; int nl = 0;
    for(char *p = strtok(work, "\r\n"); p && nl < 2048; p = strtok(0, "\r\n")) line[nl++] = p;
    char fromkey[80]; snprintf(fromkey, sizeof(fromkey), "%s:output", g_insert[j].after);   // e.g. "colour:01:output"
    static char out2[65536]; char *o = out2, *e = out2 + sizeof(out2);
    // declare NEW first, then feed it from AFTER:output (its module line precedes both connects below)
    o += snprintf(o, e - o, "module:%s:%s:0:0\n", g_insert[j].mod, g_insert[j].inst);
    for(int i = 0; i < nl && o < e; i++)
    {
      if(!strncmp(line[i], "connect:", 8))
      {
        char t[256]; snprintf(t, sizeof(t), "%s", line[i] + 8); char *tk[6]; int tn = 0;
        for(char *p = strtok(t, ":"); p && tn < 6; p = strtok(0, ":")) tk[tn++] = p;
        if(tn >= 6)
        {
          char ff[80]; snprintf(ff, sizeof(ff), "%s:%s:%s", tk[0], tk[1], tk[2]);
          if(!strcmp(ff, fromkey))   // this consumer was fed by AFTER:output -> feed it from NEW:output
          { o += snprintf(o, e - o, "connect:%s:%s:output:%s:%s:%s\n", g_insert[j].mod, g_insert[j].inst, tk[3], tk[4], tk[5]); continue; }
        }
      }
      o += snprintf(o, e - o, "%s\n", line[i]);
    }
    if(o < e) o += snprintf(o, e - o, "connect:%s:output:%s:%s:input\n", g_insert[j].after, g_insert[j].mod, g_insert[j].inst);
    if(o >= e) return 1;   // overflowed -> out2 is truncated; bail so the caller rolls back instead of building a malformed graph
    snprintf(text, cap, "%s", out2);
  }
  return 0;
}

// regenerate the cfg with bypassed modules routed around and user-added modules spliced in:
// bypass re-points every consumer of a bypassed module's output to that module's main input
// source (fan-out safe), drops the module's own connects (incl. side-inputs like luts) and
// prunes orphans; then apply_inserts() splices the added modules into their target edges.
// validated against vkdt-cli for colour/grade/filmsim/crop. returns the rewritten file path if
// anything changed, else the base path.
static int g_cfg_overflow;   // set by effective_cfg if the rewrite truncated; open_image rejects -> caller rolls back
static const char *effective_cfg(const char *base)
{
  g_cfg_overflow = 0;
  if(g_bypass_cnt <= 0 && g_insert_cnt <= 0 && g_rewire_cnt <= 0) return base;
  FILE *f = fopen(base, "rb"); if(!f) return base;
  static char buf[65536]; size_t n = fread(buf, 1, sizeof(buf) - 1, f); fclose(f); buf[n] = 0;
  // rewire pass FIRST: edits the base connect lines (repoint inputs) before bypass/insert run on top
  if(rb_apply_rewires(buf, sizeof(buf), g_rewire, g_rewire_cnt)) g_cfg_overflow = 1;
  char *line[2048]; int nl = 0;
  for(char *p = strtok(buf, "\r\n"); p && nl < 2048; p = strtok(0, "\r\n")) line[nl++] = p;

  static char out[65536]; char *o = out, *e = out + sizeof(out);
  if(g_bypass_cnt > 0)
  {
    // pass 1: main-input source ("fA:iA:cA") for each bypassed module ("fB:iB")
    char srck[16][48], srcv[16][96]; int srcn = 0;
    for(int i = 0; i < nl; i++)
    {
      if(strncmp(line[i], "connect:", 8)) continue;
      char t[256]; snprintf(t, sizeof(t), "%s", line[i] + 8); char *tk[6]; int tn = 0;
      for(char *p = strtok(t, ":"); p && tn < 6; p = strtok(0, ":")) tk[tn++] = p;
      if(tn < 6) continue;
      char to[48]; snprintf(to, sizeof(to), "%s:%s", tk[3], tk[4]);
      if(!strcmp(tk[5], "input") && is_bypassed(to) && srcn < 16)
      { snprintf(srck[srcn], 48, "%s", to); snprintf(srcv[srcn], 96, "%s:%s:%s", tk[0], tk[1], tk[2]); srcn++; }
    }
    // pass 2: build rewritten connects + the set of referenced modules
    char nc[256][96]; int ncn = 0; char ref[64][48]; int refn = 0;
    for(int i = 0; i < nl; i++)
    {
      if(strncmp(line[i], "connect:", 8)) continue;
      char t[256]; snprintf(t, sizeof(t), "%s", line[i] + 8); char *tk[6]; int tn = 0;
      for(char *p = strtok(t, ":"); p && tn < 6; p = strtok(0, ":")) tk[tn++] = p;
      if(tn < 6) continue;
      char from[48], to[48]; snprintf(from, 48, "%s:%s", tk[0], tk[1]); snprintf(to, 48, "%s:%s", tk[3], tk[4]);
      if(is_bypassed(to)) continue;                       // drop connects INTO a bypassed module
      char fA[16], iA[16], cA[16]; snprintf(fA, 16, "%s", tk[0]); snprintf(iA, 16, "%s", tk[1]); snprintf(cA, 16, "%s", tk[2]);
      if(is_bypassed(from))
      { // consumer of a bypassed output -> repoint to that module's input source
        const char *s = 0; for(int k = 0; k < srcn; k++) if(!strcmp(srck[k], from)) { s = srcv[k]; break; }
        if(!s) continue;                                  // no source -> drop
        char sc[96]; snprintf(sc, sizeof(sc), "%s", s); char *st[3]; int sn = 0;
        for(char *p = strtok(sc, ":"); p && sn < 3; p = strtok(0, ":")) st[sn++] = p;
        if(sn < 3) continue;
        snprintf(fA, 16, "%s", st[0]); snprintf(iA, 16, "%s", st[1]); snprintf(cA, 16, "%s", st[2]);
      }
      if(ncn < 256) snprintf(nc[ncn++], 96, "connect:%s:%s:%s:%s:%s:%s", fA, iA, cA, tk[3], tk[4], tk[5]);
      char a[48]; snprintf(a, 48, "%s:%s", fA, iA);
      int ha = 0, hb = 0; for(int k = 0; k < refn; k++) { if(!strcmp(ref[k], a)) ha = 1; if(!strcmp(ref[k], to)) hb = 1; }
      if(!ha && refn < 64) snprintf(ref[refn++], 48, "%s", a);
      if(!hb && refn < 64) snprintf(ref[refn++], 48, "%s", to);
    }
    // emit non-connect lines (drop bypassed + orphaned module lines), then the rewritten connects
    for(int i = 0; i < nl; i++)
    {
      if(!strncmp(line[i], "connect:", 8)) continue;
      if(!strncmp(line[i], "module:", 7))
      {
        char t[128]; snprintf(t, sizeof(t), "%s", line[i] + 7); char *tk[2]; int tn = 0;
        for(char *p = strtok(t, ":"); p && tn < 2; p = strtok(0, ":")) tk[tn++] = p;
        if(tn >= 2)
        {
          char ni[48]; snprintf(ni, 48, "%s:%s", tk[0], tk[1]);
          if(is_bypassed(ni)) continue;
          int referenced = 0; for(int k = 0; k < refn; k++) if(!strcmp(ref[k], ni)) { referenced = 1; break; }
          const int keep = !strncmp(tk[0], "i-jpg", 5) || !strncmp(tk[0], "i-raw", 5) || !strcmp(ni, "display:main");
          if(!referenced && !keep) continue;              // prune orphan
        }
      }
      if(o < e) o += snprintf(o, e - o, "%s\n", line[i]);   // defensive: never snprintf with a wrapped size
    }
    for(int i = 0; i < ncn && o < e; i++) o += snprintf(o, e - o, "%s\n", nc[i]);
  }
  else for(int i = 0; i < nl && o < e; i++) o += snprintf(o, e - o, "%s\n", line[i]);   // no bypass: verbatim

  if(o >= e) g_cfg_overflow = 1;   // bypass pass truncated the cfg -> caller must reject
  if(g_insert_cnt > 0 && apply_inserts(out, sizeof(out))) g_cfg_overflow = 1;

  const char *path = "rabbit_eff.cfg";
  FILE *w = fopen(path, "wb"); if(!w) return base;
  fwrite(out, 1, strlen(out), w); fclose(w);   // apply_inserts may have rewritten out -> use strlen, not o-out
  return path;
}

static int open_image(const char *image)
{
  // pick pipeline + input by file type: raw -> i-raw graph, fed the ORIGINAL file (vkdt
  // decodes/demosaics it; only the streamed preview is jpeg). jpeg -> i-jpg graph + a
  // small preview proxy. The proxy/decode runs BEFORE the graph teardown, so a bad file
  // leaves the current session intact.
  const int raw = image && is_raw_path(image);
  const char *cfg = effective_cfg(raw ? g_conf.cfg_raw : g_conf.cfg);   // route around bypassed modules
  if(g_cfg_overflow) { lg("err", "effective_cfg overflowed scratch buffer — rejecting rebuild"); return 1; }
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
  if(image) { char a[1024]; snprintf(g_cur_image, sizeof(g_cur_image), "%s", realpath(image, a) ? a : image); g_export_ver = 0; }

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
  // never persist while a module is bypassed: the built graph is the routed-around one, so the
  // bypassed module's params are missing from the live graph -> writing the sidecar would drop
  // them (un-bypass would restore them at defaults = edit loss). bypass is a runtime-only view;
  // the recipe stays at the last full-graph state and un-bypass restores everything intact.
  // (rewire is NOT guarded: it drops no module, so all params are still written, and the reload
  // path applies only `param:` lines from the sidecar — the rewired connects are inert there.)
  if(g_bypass_cnt > 0) return;
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
  p.p_cfgfile  = effective_cfg(raw ? g_conf.cfg_raw : g_conf.cfg);   // export reflects bypassed modules
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
  // download filename: <source basename without extension>_v<NN>.jpg, version auto-incremented
  // per image (so successive exports don't overwrite). NB: a web page can't set the download
  // DIRECTORY (browser security) — the user picks the folder in their browser settings.
  const char *bn = strrchr(g_cur_image, '/'); bn = bn ? bn + 1 : g_cur_image;
  char base[256]; snprintf(base, sizeof(base), "%s", bn);
  char *dot = strrchr(base, '.'); if(dot) *dot = 0;   // strip source extension
  const int ver = ++g_export_ver;
  mg_printf(c, "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nCache-Control: no-store\r\n"
               "Content-Disposition: attachment; filename=\"%s_v%02d.jpg\"\r\nContent-Length: %ld\r\n\r\n", base, ver, n);
  mg_write(c, (const char *)buf, n); free(buf);
  lg("srv", "export %s_v%02d.jpg %ld B (q%d max%d)", base, ver, n, quality, maxdim);
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
    "num_threads",     "32",   // civetweb holds one worker per live connection (incl. the persistent /ws); 4 wedges as soon as a PWA fires parallel asset loads + the WebSocket
    "enable_directory_listing", "no",
    "tcp_nodelay",     "1",
    "static_file_max_age", "0",
    "additional_header", "Cache-Control: no-store",  // never cache the client during dev
    "extra_mime_types", ".webmanifest=application/manifest+json",  // PWA manifest (not in civetweb's builtin table)
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
