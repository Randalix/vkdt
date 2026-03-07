#pragma once
// drag-to-adjust: hold a key and drag the mouse to change a parameter.
// config files live in bin/dragkeys/ and ~/.config/vkdt/dragkeys/.
// format:
//   # comment
//   key:<keyname>
//   <module>:<instance>:<param>:<component>:<sensitivity>
//   ...
// multiple component lines per file are supported (e.g. for white balance).

#include "core/log.h"
#include "gui/gui.h"
#include "gui/view.h"
#include "gui/hotkey.h"
#include "pipe/graph.h"
#include "pipe/graph-history.h"
#include "pipe/modules/api.h"
#include "pipe/res.h"

#include <dirent.h>
#include <limits.h>

#define DT_DRAGKEY_MAX       16
#define DT_DRAGKEY_MAX_COMP   8

typedef struct dt_dragkey_comp_t
{
  dt_token_t module;
  dt_token_t instance;
  dt_token_t param;
  int        component;    // index into parameter array
  float      sensitivity;  // parameter change per pixel of mouse movement
  // resolved at drag start:
  int        modid;
  int        parid;
  float      start_val;
}
dt_dragkey_comp_t;

typedef struct dt_dragkey_t
{
  int    key;              // GLFW key code
  char   name[64];         // display name (from filename)
  dt_dragkey_comp_t comp[DT_DRAGKEY_MAX_COMP];
  int    comp_cnt;
}
dt_dragkey_t;

typedef struct dt_dragkeys_t
{
  dt_dragkey_t dk[DT_DRAGKEY_MAX];
  int    cnt;
  int    active;           // index of active drag, or -1
  double start_x;          // mouse x when drag began
}
dt_dragkeys_t;

// map a short key name (from config) to GLFW key code, using the hk_keys table
static inline int
dt_dragkey_name_to_glfw(const char *name)
{
  for(int y = 0; y < 6; y++)
  {
    int x = 0;
    while(hk_keys[y][x].lib)
    {
      if(!strcasecmp(hk_keys[y][x].lib, name))
        return hk_keys[y][x].key;
      x++;
    }
  }
  // single uppercase letter A-Z
  if(name[0] >= 'A' && name[0] <= 'Z' && name[1] == 0)
    return GLFW_KEY_A + (name[0] - 'A');
  if(name[0] >= 'a' && name[0] <= 'z' && name[1] == 0)
    return GLFW_KEY_A + (name[0] - 'a');
  return 0;
}

static inline int
dt_dragkey_load_file(dt_dragkey_t *dk, const char *path, const char *basename)
{
  FILE *f = fopen(path, "rb");
  if(!f) return 1;
  dk->key = 0;
  dk->comp_cnt = 0;
  snprintf(dk->name, sizeof(dk->name), "%s", basename);
  char line[256];
  while(fgets(line, sizeof(line), f))
  {
    // strip newline
    char *nl = strchr(line, '\n');
    if(nl) *nl = 0;
    if(line[0] == '#' || line[0] == 0) continue;
    if(!strncmp(line, "key:", 4))
    {
      dk->key = dt_dragkey_name_to_glfw(line + 4);
    }
    else
    { // module:instance:param:component:sensitivity
      if(dk->comp_cnt >= DT_DRAGKEY_MAX_COMP) continue;
      dt_dragkey_comp_t *c = dk->comp + dk->comp_cnt;
      char mod[9] = {0}, inst[9] = {0}, par[9] = {0};
      int comp = 0;
      float sens = 0.002f;
      if(sscanf(line, "%8[^:]:%8[^:]:%8[^:]:%d:%f", mod, inst, par, &comp, &sens) >= 4)
      {
        c->module    = dt_token(mod);
        c->instance  = dt_token(inst);
        c->param     = dt_token(par);
        c->component = comp;
        c->sensitivity = sens;
        dk->comp_cnt++;
      }
    }
  }
  fclose(f);
  return (dk->key == 0 || dk->comp_cnt == 0) ? 1 : 0;
}

static inline void
dt_dragkeys_init(dt_dragkeys_t *dk)
{
  memset(dk, 0, sizeof(*dk));
  dk->active = -1;
  // load from home dir first (user overrides), then basedir (shipped defaults)
  for(int inbase = 0; inbase < 2; inbase++)
  {
    void *dirp = dt_res_opendir("dragkeys", inbase);
    if(!dirp) continue;
    const char *basename = 0;
    while((basename = dt_res_next_basename(dirp, inbase)))
    {
      if(basename[0] == '.') continue;
      if(dk->cnt >= DT_DRAGKEY_MAX) break;
      // check for duplicates (home overrides base)
      int dup = 0;
      for(int i = 0; i < dk->cnt; i++)
        if(!strcmp(dk->dk[i].name, basename)) { dup = 1; break; }
      if(dup) continue;
      char path[PATH_MAX];
      snprintf(path, sizeof(path), "%s/dragkeys/%s",
          inbase ? dt_pipe.basedir : dt_pipe.homedir, basename);
      if(!dt_dragkey_load_file(dk->dk + dk->cnt, path, basename))
      {
        dt_log(s_log_gui, "[dragkey] loaded '%s' key=%d components=%d",
            basename, dk->dk[dk->cnt].key, dk->dk[dk->cnt].comp_cnt);
        dk->cnt++;
      }
    }
    dt_res_closedir(dirp, inbase);
  }
}

static inline void
dt_dragkeys_cleanup(dt_dragkeys_t *dk)
{
  dk->cnt = 0;
  dk->active = -1;
}

// called from darkroom_keyboard. returns 1 if the event was consumed.
static inline int
dt_dragkey_keyboard(dt_dragkeys_t *dk, int key, int action)
{
  if(action == GLFW_PRESS && dk->active < 0)
  {
    for(int i = 0; i < dk->cnt; i++)
    {
      if(dk->dk[i].key != key) continue;
      // resolve modules and record start values
      dt_dragkey_t *d = dk->dk + i;
      int ok = 0;
      for(int j = 0; j < d->comp_cnt; j++)
      {
        dt_dragkey_comp_t *c = d->comp + j;
        c->modid = dt_module_get(&vkdt.graph_dev, c->module, c->instance);
        if(c->modid < 0) continue;
        c->parid = dt_module_get_param(vkdt.graph_dev.module[c->modid].so, c->param);
        if(c->parid < 0) continue;
        const float *val = dt_module_param_float(vkdt.graph_dev.module + c->modid, c->parid);
        if(!val) continue;
        c->start_val = val[c->component];
        ok = 1;
      }
      if(!ok) return 0;
      double mx, my;
      dt_view_get_cursor_pos(vkdt.win.window, &mx, &my);
      dk->start_x = mx;
      dk->active = i;
      return 1;
    }
  }
  else if(action == GLFW_RELEASE && dk->active >= 0)
  {
    dt_dragkey_t *d = dk->dk + dk->active;
    if(key != d->key) return 0;
    // commit all changed params to history
    for(int j = 0; j < d->comp_cnt; j++)
    {
      dt_dragkey_comp_t *c = d->comp + j;
      if(c->modid >= 0 && c->parid >= 0)
        dt_graph_history_append(&vkdt.graph_dev, c->modid, c->parid, 0.0);
    }
    dk->active = -1;
    return 1;
  }
  return 0;
}

// called from darkroom_mouse_position. returns 1 if drag is active.
static inline int
dt_dragkey_mouse_move(dt_dragkeys_t *dk, double x)
{
  if(dk->active < 0) return 0;
  dt_dragkey_t *d = dk->dk + dk->active;
  double dx = x - dk->start_x;
  char msg[128];
  int off = 0;
  off += snprintf(msg + off, sizeof(msg) - off, "%s:", d->name);
  for(int j = 0; j < d->comp_cnt; j++)
  {
    dt_dragkey_comp_t *c = d->comp + j;
    if(c->modid < 0 || c->parid < 0) continue;
    float new_val = c->start_val + (float)(dx * c->sensitivity);
    float *val = (float *)(vkdt.graph_dev.module[c->modid].param
        + vkdt.graph_dev.module[c->modid].so->param[c->parid]->offset);
    val[c->component] = new_val;
    if(off < (int)sizeof(msg) - 20)
      off += snprintf(msg + off, sizeof(msg) - off, " %.3f", new_val);
  }
  vkdt.graph_dev.runflags = s_graph_run_all;
  dt_gui_notification("%s", msg);
  return 1;
}
