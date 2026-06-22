# server (rabbit_editor web remote)

Work in progress: a resident process that keeps a vkdt graph + its GPU buffers
warm and re-renders incrementally, for a web-based remote frontend (GPU on the
host, thin browser client). See the project notes for the full design.

`vkdt-cli` is one-shot (cold Vulkan init per run); this directory grows the
long-lived counterpart.

## current state — M1 probe

`m1_resident.c` proves the warm incremental re-render loop: init Vulkan + build
the graph once via `dt_graph_export`, then sweep one module parameter and call
`dt_graph_run` repeatedly, timing each warm re-render.

```
  cd bin/
  make server          # builds ../bin/vkdt-server
  ./vkdt-server examples/m1.cfg 20
```

Writes `m1_warmup.jpg` (cold first render) and `m1_iter_NN.jpg` (warm).
