# libvisprof

An on-screen frame profiler for Dreamcast applications using KallistiOS.
Supports C99 with GNU extensions and C++11 or later.

![libvisprof in Flycast](docs/visprof-flycast.png)

## Build

```sh
source /opt/toolchains/dc/kos/environ.sh
make
```

Add `-I<libvisprof>/include` to your compiler flags and
`<libvisprof>/libvisprof.a` to your link command. The library depends on KOS
and libc.

```sh
make -C examples/basic     # basic demo
make -C examples/2ndmix    # instrumented KOS sample
make check                # host tests and KOS build checks
make clean                # remove all library and example build outputs
```

`make check` also needs native GCC. The [2ndmix
example](examples/2ndmix/README.md) uses source and assets from your KOS
installation. Both demos have been checked in Flycast and on Dreamcast
hardware.

## Use

Initialize after PVR setup. Register phases for parts of the frame and use
`VISPROF_SCOPE` to time individual functions or blocks. This example assumes
`update()` and `draw_scene()` belong to your application:

```c
#include <kos.h>
#include <visprof/visprof.h>

/* After PVR initialization, in your application's main function. */
visprof_init(NULL);
visprof_phase_t update_phase = visprof_phase_register("Update", 0);
visprof_set_visible(1);

for (;;) {
    visprof_frame_begin();
    visprof_phase_begin(update_phase);
    {
        VISPROF_SCOPE("update");
        update();
    }
    visprof_phase_end(update_phase);

    pvr_wait_ready();
    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_TR_POLY);
    draw_scene();
    visprof_draw();
    pvr_list_finish();
    pvr_scene_finish();
    visprof_frame_end();
}
```

- Draw in the open translucent list using direct rendering, after your scene.
- Use `pvr_init_defaults()` or provide translucent bins and enough polygon-list
  overflow space. Both examples set `opb_overflow_count = 3`. Insufficient
  space causes missing geometry on hardware.
- Keep scene depths below `1.00e9`, where the overlay begins.
- Call the profiler from one frame thread. Phases must not overlap. End every
  scope before `visprof_frame_end()`.
- Keep registered names and counter storage valid until shutdown. Wait for
  pending PVR work to finish before calling `visprof_shutdown()`.

The [basic demo](examples/basic/main.c) shows phases, counters and controls.
The [public header](include/visprof/visprof.h) documents each function.

## Reading the overlay

Times are elapsed wall time, including waits. `raw` includes profiler
overhead. `adjusted` subtracts measured profiler overhead from frame and phase
times. Zones always include child zones. Neither mode measures GPU execution
time.

FPS uses actual elapsed time in both modes. `med` and `max` describe the frame
history. The captured phase and zone timings belong to one retained frame,
which can differ from the history maximum. Counters and `prof` show values
from the last completed frame.

The library supports four phases, 32 zones per frame and 16 application
counters. Its font atlas uses 64 KiB of video memory.

See the [reference](docs/reference.md) for configuration, display labels,
capture rules, rendering requirements and alternative build methods.

## Disable

Build the library and your application with `VISPROF_ENABLED=0` to compile out
profiling. The setting must match in both:

```sh
make VISPROF_ENABLED=0
```

## License

[MIT](LICENSE). See [third-party notices](THIRD-PARTY.md) for KOS and
[CONTRIBUTING.md](CONTRIBUTING.md) for development checks.
