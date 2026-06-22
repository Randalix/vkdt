// rabbit_editor M1 probe: resident render-core proof.
//
// Goal: show that we can keep a vkdt processing graph + its GPU buffers warm in
// a long-lived process and re-render incrementally on each parameter change --
// the core of the planned web preview server. vkdt-cli is one-shot (cold Vulkan
// init per invocation); here we init once, then drive many re-renders in a loop
// and measure the warm re-render latency.
//
// usage: vkdt-server <graph.cfg> [iterations]
// writes m1_warmup.jpg (cold first render) and m1_iter_NN.jpg (warm re-renders).

#include "qvk/qvk.h"
#include "pipe/graph.h"
#include "pipe/graph-io.h"
#include "pipe/graph-export.h"
#include "pipe/global.h"
#include "pipe/modules/api.h"
#include "core/log.h"
#include "core/threads.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static inline double now_ms()
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(int argc, char *argv[])
{
  const char *cfg  = argc > 1 ? argv[1]       : "examples/m1.cfg";
  const int   iters = argc > 2 ? atoi(argv[2]) : 20;

  dt_log_init(s_log_cli);
  dt_log_init_arg(argc, argv);
  dt_pipe_global_init();
  threads_global_init();

  if(qvk_init(0, -1, 0, 0, 0)) { fprintf(stderr, "[M1] qvk_init failed\n"); return 1; }

  dt_graph_t graph;
  dt_graph_init(&graph, s_queue_compute);
  snprintf(graph.searchpath, sizeof(graph.searchpath), ".");

  // --- one-shot setup: read cfg, replace display by an o-jpg sink, render the
  // first (cold) frame. after this the graph is fully built and warm. ---
  dt_graph_export_t param = {0};
  param.p_cfgfile  = cfg;
  param.output_cnt = 1;
  param.output[0].inst             = dt_token("main");
  param.output[0].mod              = dt_token("o-jpg");
  param.output[0].p_filename       = "m1_warmup";
  param.output[0].quality          = 90;
  param.output[0].colour_primaries = s_colour_primaries_srgb;
  param.output[0].colour_trc       = s_colour_trc_srgb;

  double t0 = now_ms();
  VkResult res = dt_graph_export(&graph, &param);
  double t_setup = now_ms() - t0;
  if(res != VK_SUCCESS) { fprintf(stderr, "[M1] export(setup) failed: %d\n", res); return 1; }
  fprintf(stderr, "[M1] cold setup + first render : %8.1f ms\n", t_setup);

  // --- locate the module whose parameter we will sweep, and the output sink. ---
  // eq:01 is wired into logo.cfg (logo -> eq -> blend -> lens -> display); its
  // "edges" parameter visibly changes the output, so each change forces a real
  // downstream recompute -- exactly the slider-drag case we care about.
  const int eqmod  = dt_module_get(&graph, dt_token("eq"),    dt_token("01"));
  const int outmod = dt_module_get(&graph, dt_token("o-jpg"), dt_token("main"));
  if(eqmod < 0) { fprintf(stderr, "[M1] no eq:01 module in graph\n"); return 1; }
  fprintf(stderr, "[M1] eq module id=%d  o-jpg sink id=%d\n", eqmod, outmod);

  // --- warm incremental re-render loop. ---
  double sum = 0.0, mn = 1e30, mx = 0.0;
  for(int i = 0; i < iters; i++)
  {
    const float edges = 0.05f + (0.90f * i) / (iters > 1 ? iters - 1 : 1);
    dt_module_set_param_float(graph.module + eqmod, dt_token("edges"), edges);
    if(outmod >= 0)
    {
      char fn[64];
      snprintf(fn, sizeof(fn), "m1_iter_%02d", i);
      dt_module_set_param_string(graph.module + outmod, dt_token("filename"), fn);
    }

    const double s = now_ms();
    // same flags the gui uses to commit a parameter change, plus sink download so
    // the o-jpg module actually writes the result out.
    graph.runflags = s_graph_run_record_cmd_buf | s_graph_run_download_sink | s_graph_run_wait_done;
    res = dt_graph_run(&graph, graph.runflags);
    const double dt = now_ms() - s;
    if(res != VK_SUCCESS) { fprintf(stderr, "[M1] run %d failed: %d\n", i, res); break; }

    sum += dt; if(dt < mn) mn = dt; if(dt > mx) mx = dt;
    fprintf(stderr, "[M1] iter %02d  edges=%.3f  warm re-render %7.1f ms\n", i, edges, dt);
  }
  if(iters > 0)
    fprintf(stderr, "[M1] warm re-render: avg %.1f ms  min %.1f  max %.1f  (n=%d)\n",
        sum / iters, mn, mx, iters);

  dt_graph_cleanup(&graph);
  threads_global_cleanup();
  qvk_cleanup();
  return 0;
}
