# o-webp: write webp files

supports writing lossy webp images to disk. [convert to srgb or a similar colour
space](../colenc/readme.md) before connecting to this module.

Used by the rabbit_editor web server for the streamed preview: webp frames are
markedly smaller than jpeg over WiFi, which lowers the mobile transfer latency
(the render cost is unchanged). The full-res export path stays jpeg.

## connectors

* `input` the 8-bit srgb buffer to be written to disk

## parameters

* `filename` the filename on disk to write to. `.webp` will be appended.
* `quality` 0-100 webp quality (lossy)
