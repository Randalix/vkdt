#pragma once
// Pure cfg-text transform for user re-wirings (rabbit web remote). Kept header-only and
// parameterised (no server globals) so the offline test (rewire_test.c) drives the exact same
// code the server runs. A rewire repoints a module's input connector to a new output source.
#include <string.h>
#include <stdio.h>

typedef struct { char to[64]; char from[80]; } rb_rewire_t;   // both "name:inst:conn"

// copy the first two ':'-tokens of `s` ("name:inst:conn") into out as "name:inst"
static inline void rb_two_tokens(const char *s, char *out, int outsz)
{
  char t[96]; snprintf(t, sizeof(t), "%s", s); char *tk[3]; int tn = 0;
  for(char *p = strtok(t, ":"); p && tn < 3; p = strtok(0, ":")) tk[tn++] = p;
  if(tn >= 2) snprintf(out, outsz, "%s:%s", tk[0], tk[1]); else snprintf(out, outsz, "%s", s);
}

// is `ni` ("name:inst") declared as a `module:` line in the cfg lines?
static inline int rb_text_has_module(char *const *line, int nl, const char *ni)
{
  for(int i = 0; i < nl; i++)
  {
    if(strncmp(line[i], "module:", 7)) continue;
    char t[160]; snprintf(t, sizeof(t), "%s", line[i] + 7); char *tk[2]; int tn = 0;
    for(char *p = strtok(t, ":"); p && tn < 2; p = strtok(0, ":")) tk[tn++] = p;
    if(tn >= 2) { char m[48]; snprintf(m, sizeof(m), "%s:%s", tk[0], tk[1]); if(!strcmp(m, ni)) return 1; }
  }
  return 0;
}

// Apply the user re-wirings to the cfg text in place (within cap bytes): for each rewire whose
// BOTH endpoints are present in this cfg, drop every existing `connect:*:*:*:to` (the input's old
// source) and append a fresh `connect:from:to`. An input has exactly one source, so drop-then-add
// is the whole edit; fan-out on the OLD source's other consumers is untouched (only connects INTO
// `to` are dropped). Rewirings with a missing endpoint (stale across image switches) are skipped.
// Returns 1 if the rewrite overflowed its scratch (caller must roll back), else 0.
static inline int rb_apply_rewires(char *text, size_t cap, const rb_rewire_t *rw, int rwn)
{
  if(rwn <= 0) return 0;
  static char work[65536]; snprintf(work, sizeof(work), "%s", text);   // strtok destroys; work on a copy
  char *line[2048]; int nl = 0;
  for(char *p = strtok(work, "\r\n"); p && nl < 2048; p = strtok(0, "\r\n")) line[nl++] = p;

  // which rewirings apply here (both endpoints declared in this cfg)?
  int active[64]; if(rwn > 64) rwn = 64;
  for(int j = 0; j < rwn; j++)
  {
    char tni[48], fni[48];
    rb_two_tokens(rw[j].to, tni, sizeof(tni));
    rb_two_tokens(rw[j].from, fni, sizeof(fni));
    active[j] = rb_text_has_module(line, nl, tni) && rb_text_has_module(line, nl, fni);
  }

  static char out[65536]; char *o = out, *e = out + sizeof(out);
  for(int i = 0; i < nl && o < e; i++)
  {
    int drop = 0;
    if(!strncmp(line[i], "connect:", 8))
    {
      char t[256]; snprintf(t, sizeof(t), "%s", line[i] + 8); char *tk[6]; int tn = 0;
      for(char *p = strtok(t, ":"); p && tn < 6; p = strtok(0, ":")) tk[tn++] = p;
      if(tn >= 6)
      {
        char tokey[64]; snprintf(tokey, sizeof(tokey), "%s:%s:%s", tk[3], tk[4], tk[5]);
        for(int j = 0; j < rwn; j++) if(active[j] && !strcmp(rw[j].to, tokey)) { drop = 1; break; }
      }
    }
    if(!drop) o += snprintf(o, e - o, "%s\n", line[i]);
  }
  for(int j = 0; j < rwn && o < e; j++)
    if(active[j]) o += snprintf(o, e - o, "connect:%s:%s\n", rw[j].from, rw[j].to);   // from + to are "name:inst:conn"
  if(o >= e) return 1;   // overflowed -> truncated; bail so the caller rolls back
  snprintf(text, cap, "%s", out);
  return 0;
}
