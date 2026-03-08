#include "modules/api.h"

#define LUT_N 17

void
create_nodes(
    dt_graph_t  *graph,
    dt_module_t *module)
{
  // normalized LUT: LUT_N*LUT_N x LUT_N in rgba:f16
  const int lut_wd = LUT_N * LUT_N;
  const int lut_ht = LUT_N;
  dt_roi_t roi_lut = { .wd = lut_wd, .ht = lut_ht, .full_wd = lut_wd, .full_ht = lut_ht, .scale = 1 };

  // build LUT: each thread handles one cell, scans a grid of samples
  const int id_build = dt_node_add(graph, module, "cmatch", "build",
      lut_wd, lut_ht, 1, 0, 0, 3,
      "input",  "read",  "*",    "*",   dt_no_roi,
      "target", "read",  "*",    "*",   dt_no_roi,
      "output", "write", "rgba", "f16", &roi_lut);

  // apply LUT to full-res input
  const int id_apply = dt_node_add(graph, module, "cmatch", "apply",
      module->connector[0].roi.wd, module->connector[0].roi.ht, 1, 0, 0, 3,
      "input",  "read",  "*",    "*",   dt_no_roi,
      "lut",    "read",  "rgba", "f16", dt_no_roi,
      "output", "write", "rgba", "f16", &module->connector[2].roi);

  dt_connector_copy(graph, module, 0, id_build, 0);
  dt_connector_copy(graph, module, 1, id_build, 1);
  dt_connector_copy(graph, module, 0, id_apply, 0);
  CONN(dt_node_connect(graph, id_build, 2, id_apply, 1));
  dt_connector_copy(graph, module, 2, id_apply, 2);
}
