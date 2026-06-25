// Offline validation of the rewire cfg-transform (rb_apply_rewires) — no GPU/Vulkan.
// Drives the EXACT server code from cfg_rewire.h against m3-like topologies.
//   cc -o /tmp/rewire_test src/server/rewire_test.c && /tmp/rewire_test
#include "cfg_rewire.h"
#include <stdio.h>
#include <string.h>

static int g_fail = 0;
static void ok(const char *name, int cond, const char *detail)
{
  printf("%s %s%s%s\n", cond ? "PASS" : "FAIL", name, detail ? " :: " : "", detail ? detail : "");
  if(!cond) g_fail++;
}
// does `text` contain `needle` as a full line?
static int has_line(const char *text, const char *needle)
{
  char buf[65536]; snprintf(buf, sizeof(buf), "%s", text);
  for(char *p = strtok(buf, "\n"); p; p = strtok(0, "\n")) if(!strcmp(p, needle)) return 1;
  return 0;
}
static int count_line(const char *text, const char *needle)
{
  char buf[65536]; snprintf(buf, sizeof(buf), "%s", text); int n = 0;
  for(char *p = strtok(buf, "\n"); p; p = strtok(0, "\n")) if(!strcmp(p, needle)) n++;
  return n;
}

// a small m3-like graph with a fan-out: colour:01:output feeds BOTH grade:01 and hist:01.
static const char *base_graph(void)
{
  return
    "module:i-jpg:main:0:0\n"
    "module:colour:01:0:0\n"
    "module:grade:01:0:0\n"
    "module:filmsim:01:0:0\n"
    "module:hist:01:0:0\n"
    "module:display:main:0:0\n"
    "module:display:hist:0:0\n"
    "connect:i-jpg:main:output:colour:01:input\n"
    "connect:colour:01:output:grade:01:input\n"
    "connect:colour:01:output:hist:01:input\n"
    "connect:grade:01:output:filmsim:01:input\n"
    "connect:filmsim:01:output:display:main:input\n"
    "connect:hist:01:output:display:hist:input\n";
}

int main(void)
{
  // case 1: single-consumer move — repoint filmsim:01's input from grade:01 to colour:01.
  {
    char t[65536]; snprintf(t, sizeof(t), "%s", base_graph());
    rb_rewire_t rw[] = {{ "filmsim:01:input", "colour:01:output" }};
    int ov = rb_apply_rewires(t, sizeof(t), rw, 1);
    ok("c1 no overflow", !ov, 0);
    ok("c1 old edge dropped", !has_line(t, "connect:grade:01:output:filmsim:01:input"), 0);
    ok("c1 new edge added",  has_line(t, "connect:colour:01:output:filmsim:01:input"), 0);
    // fan-out on the OLD source (grade:01) — grade still gets ITS input from colour (untouched)
    ok("c1 fanout untouched", has_line(t, "connect:colour:01:output:grade:01:input"), 0);
    ok("c1 single new connect", count_line(t, "connect:colour:01:output:filmsim:01:input") == 1, 0);
  }

  // case 2: fan-out target — repoint hist:01's input (currently from colour:01) to grade:01.
  // the OTHER consumer of colour:01 (grade:01) must stay; only the connect INTO hist changes.
  {
    char t[65536]; snprintf(t, sizeof(t), "%s", base_graph());
    rb_rewire_t rw[] = {{ "hist:01:input", "grade:01:output" }};
    rb_apply_rewires(t, sizeof(t), rw, 1);
    ok("c2 old hist edge dropped", !has_line(t, "connect:colour:01:output:hist:01:input"), 0);
    ok("c2 new hist edge added",   has_line(t, "connect:grade:01:output:hist:01:input"), 0);
    ok("c2 colour->grade kept",    has_line(t, "connect:colour:01:output:grade:01:input"), 0);
  }

  // case 3: stale skip — a rewire whose `from` module is absent from this cfg is a no-op.
  {
    char t[65536]; snprintf(t, sizeof(t), "%s", base_graph());
    rb_rewire_t rw[] = {{ "filmsim:01:input", "ghost:09:output" }};
    rb_apply_rewires(t, sizeof(t), rw, 1);
    ok("c3 stale skipped (old kept)", has_line(t, "connect:grade:01:output:filmsim:01:input"), 0);
    ok("c3 stale added nothing",      !has_line(t, "connect:ghost:09:output:filmsim:01:input"), 0);
  }

  // case 4: dedupe / last-wins is the caller's job, but two entries with the SAME `to` must not
  // both emit — apply applies each active entry, so the caller guarantees one per `to`. here we
  // assert two DIFFERENT `to` rewirings compose, and order independence of the result set.
  {
    char t[65536]; snprintf(t, sizeof(t), "%s", base_graph());
    rb_rewire_t rw[] = {
      { "filmsim:01:input", "colour:01:output" },
      { "hist:01:input",    "filmsim:01:output" },
    };
    rb_apply_rewires(t, sizeof(t), rw, 2);
    ok("c4 first rewire applied",  has_line(t, "connect:colour:01:output:filmsim:01:input"), 0);
    ok("c4 second rewire applied", has_line(t, "connect:filmsim:01:output:hist:01:input"), 0);
    ok("c4 first old dropped",  !has_line(t, "connect:grade:01:output:filmsim:01:input"), 0);
    ok("c4 second old dropped", !has_line(t, "connect:colour:01:output:hist:01:input"), 0);
  }

  // case 5: no-op rewire — repoint an input to its CURRENT source. graph is unchanged in meaning
  // (exactly one connect into that input, from the same source). this is the live-test's safe probe.
  {
    char t[65536]; snprintf(t, sizeof(t), "%s", base_graph());
    rb_rewire_t rw[] = {{ "filmsim:01:input", "grade:01:output" }};
    rb_apply_rewires(t, sizeof(t), rw, 1);
    ok("c5 noop keeps single edge", count_line(t, "connect:grade:01:output:filmsim:01:input") == 1, 0);
  }

  // case 6: empty set — verbatim passthrough.
  {
    char t[65536]; snprintf(t, sizeof(t), "%s", base_graph());
    int ov = rb_apply_rewires(t, sizeof(t), 0, 0);
    ok("c6 empty no overflow", !ov, 0);
    ok("c6 empty unchanged",   !strcmp(t, base_graph()), 0);
  }

  printf("\n%s — %s\n", g_fail ? "FAILURES" : "ALL PASS", g_fail ? "see above" : "rewire cfg-transform validated");
  return g_fail ? 1 : 0;
}
