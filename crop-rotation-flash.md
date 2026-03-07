when entering the crop widget on an already-rotated image, the unrotated image
flashes for one frame before the crop overlay appears.

`render_darkroom.h:716` sets rotation to 0 and triggers `s_graph_run_all`
(line 729) to show the uncropped/unrotated image for interactive editing. the
pipeline reruns and presents this reset state for one frame before the crop
overlay draws on top.

the same mechanism causes a related issue: entering the crop widget a second
time briefly shows the rotation from the previous edit, because the module
param is read (line 715), zeroed (line 716), and the pipeline reruns — all
in separate frames.

```c
// line 715-716
float rot = dt_module_param_float(..., dt_token("rotate"))[0];
dt_module_set_param_float(..., dt_token("rotate"), 0.0f);
// line 729
vkdt.graph_dev.runflags = s_graph_run_all;
```

the reset is intentional (so you edit against the undistorted image) but the
visual flash is not. possible fix: suppress display output for the reset frame,
or apply the rotation reset and the crop overlay atomically in the same frame.
