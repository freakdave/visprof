# Contributing

Load the KOS environment and run `make check` before sending a change.
It compiles the library and demo, checks enabled and disabled builds, and runs
host tests of the timing and layout logic. Rendering changes also need an
emulator or console test.

Use C99 with the GNU cleanup attribute and preserve C++11 compatibility. Keep
comments short. Explain timing, memory and rendering requirements where the
code alone is insufficient. Use the terminology in the public header and
README.

Keep allocation and string formatting out of `visprof_draw()`. Preserve the
elapsed-time clock, accounting between frames, graph column merging and the
host application's PVR state. Keep the full MIT notice in source files.

For a bug report, include the KOS version, steps to reproduce it and what
happened. Rendering reports should include the console model or emulator
version, a screenshot and the demo console output.

Build and run the demo with your usual KOS tools:

```sh
source /opt/toolchains/dc/kos/environ.sh
make -C examples/basic
```

In the demo, A toggles visibility, B switches timing modes, X adds extra work,
Y shows the depth-test bars, D-pad right moves the overlay, and Start exits. Check
text legibility, the graph, zone timing and all three depth test bars. Compare
both timing modes. The console prints a summary every 120 frames.

Both demos have passed a Dreamcast hardware check. Changes to rendering still
need hardware verification. CPU/GPU overhead still needs an isolated
benchmark.
