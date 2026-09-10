# Reference

[Quick start](../README.md) | [Public API](../include/visprof/visprof.h)

## Measurement

The first frame period starts at `visprof_frame_begin()`. Later periods start
at the preceding `visprof_frame_end()`, including work between calls. Frame
end records the sample and prepares the next draw. Register phases and
counters before the first frame. End scoped zones before frame end.

The timer is KOS `timer_ns_gettime64()`. It measures elapsed wall time, so it
includes time while the CPU waits for PVR work or sleeps. It does not measure
CPU execution time alone. Clock intervals that go backward are treated as
zero. Intervals above 4,000 ms are clamped to 4,000 ms.

A phase is a named part of a frame, shown as a colored graph band. Phases must
not overlap or nest. Use one begin/end pair per phase per frame. A zone is a
timed region of code, usually a function. A phase can contain zones. Zones
always include their child zones. Nested zones are separate entries and can
overlap in time. `visprof_zone_add_ms()` adds an elapsed value measured by the
caller.

`VISPROF_INCLUSIVE`, displayed as `raw`, reports the elapsed frame and phase
values with measured profiler work included. `VISPROF_EXCLUSIVE`, displayed as
`adjusted`, subtracts profiler work measured by libvisprof from those values.
It does not remove child zones, and it does not subtract GPU time spent
drawing the overlay. Zone values are unchanged by the mode. The `prof` display
value is the profiler cost measured for the last frame in both modes.

The history ring has two to 256 slots, with one reserved for writing. It
retains up to `history_len - 1` completed samples. `history_len` zero selects
180. A value of one is clamped to two. The graph width is the selected history
length. The statistics use completed samples only. For an even sample count,
the median is the upper middle sample.

The phase breakdown and zone box show the same captured frame. Frames within
ten percent of a retained peak refresh the capture. A larger frame also raises
that peak. This keeps repeated spikes visible without gradually lowering the
capture threshold. After `history_len` frames without a refresh, the current
frame becomes the reference. Frames at or above 500 ms are excluded from
capture. Zone entries below 0.1 ms are omitted from the box. The box shows up
to eight entries, sorted by elapsed zone time. A parent and its children can
therefore add to more than the frame duration when read as separate rows.

Set `thief_scan` to enable thread CPU sampling. It samples KOS thread CPU
usage in milliseconds and reports the busiest of up to eight tracked threads
as milliseconds per second. It excludes the calling thread and the idle
thread. A frame over 18 ms requests a scan, at most once per 60 frames. The
first scan records initial CPU times. Thread records for exited threads are
reclaimed. The scan is disabled by default. The optional `spike_log` writes to
KOS `dbglog()` after a frame over 25 ms and below 500 ms, at most once per 60
frames. Its measured duration is charged to the following frame. Spike logging
starts after the first `history_len` frames. KOS can compile `dbglog()` out.
If requested logging is unavailable, initialization prints a warning.

The overlay starts hidden. `visprof_set_visible(1)` enables drawing. Recording
and diagnostics continue while hidden, but layout and drawing stop. Phase and
zone timer call overhead is not calibrated out. The overhead measurement is
elapsed time and can include interruption by other threads. Mode changes leave
old history samples unchanged until they are overwritten. All scoped zones
must end before `visprof_frame_end()`.

## Configuration

See `visprof_config_t` in the [public header](../include/visprof/visprof.h)
for fields and defaults. Pass `NULL` to `visprof_init()` for defaults.

Zero or negative `text_scale` selects the native 12x24 font. A positive value
below one selects the 6x12 font. Values above one still use 12x24. Zero or
negative `px_per_ms` selects 3.0. Invalid modes become `VISPROF_INCLUSIVE`.
Invalid anchors become `VISPROF_TOP_LEFT`.

The panel alpha byte is ignored because the panel is fully opaque. Keep the
panel dark so white glyphs remain readable. The glyph background is baked at
initialization, so changing `panel_argb` requires shutdown and initialization.
RGB channel values divisible by eight reduce differences between the panel and
the atlas background, which stores five bits per channel.

Anchors are ordered in rows. The first three are top left, top center and top
right. The next three are the middle row. The last three are the bottom row.
The zone box is below a panel at the top and above a middle or bottom panel.
The pixel offset is clamped when the panel fits. The default layout targets
640 by 480 or larger. Smaller modes or a large `px_per_ms` can exceed the
available space. Reduce the text size or graph scale for those modes.
`visprof_set_anchor()` and `visprof_set_mode()` can change these settings
after initialization.

## Display labels

The panel contains FPS and timing mode, frame statistics, a stacked graph,
guide labels, phase names and counter rows. The zone box is a separate panel.

| Label | Meaning |
|---|---|
| `FPS` | Completed frames per second, measured from elapsed wall time |
| `med` | Upper median completed frame time in the history |
| `max` | Maximum completed frame time in the history |
| `raw`, `adjusted` | Frame and phase times before or after subtracting measured profiler overhead |
| `captured frame` | Elapsed time of the retained frame, in milliseconds |
| Phase names and `other` | Captured phase times and time outside the registered phases, in milliseconds |
| `captured zones` | Zone times from that same captured frame |
| `N more zones dropped` | Zones that exceeded the table for the captured frame |
| `thread CPU` | Busiest sampled other thread in milliseconds per second, independent of the capture |
| `prof` | Measured profiler overhead from the last frame, in milliseconds |
| `demo quads` | Demo geometry submitted through its quad helper, excluding the overlay |
| Red line | Total frame time |
| White / yellow lines | Reference times for 60 / 30 FPS |
| `ms` | Graph time unit. The zero tick is labelled `0`. |

The first line shows FPS followed by `raw` or `adjusted`. FPS updates over
intervals of at least half a second and includes profiler overhead in both
modes. It shows `--` until the first interval completes. Median and maximum
times are on the next line and follow the selected timing mode.

Counters are sampled at `visprof_frame_end()`. The next draw shows those
values, including decreases and zero. `demo quads` is registered by the demo
as an example of an application counter. It is not a library measurement of
all submitted geometry. Four counter rows are available. Names are clipped to
12 characters. Values display all digits of an unsigned 32-bit integer.
Counter items that do not fit remain registered but are not drawn. Phase names
are clipped to ten characters in two columns. Choose names that remain
distinct within that limit. Zone labels are clipped to 28 characters. The
graph has guide lines at 8, 16, 24 and 32 ms, with a 36 ms display height by
default.

## Public limits

- Four phases, 32 zones per frame, 16 application counters.
- Eight captured zone entries and eight sampled thread records.
- ASCII characters 32 through 126 in the 256 by 128 ARGB1555 atlas.
- The atlas uses 64 KiB of PVR memory and is freed by `visprof_shutdown()`.
- No allocation or string formatting occurs in `visprof_draw()`.
- All calls must come from one frame thread. The thread scan briefly masks
  interrupts while it walks KOS's thread list.
- Registration failure returns `VISPROF_INVALID_PHASE`,
  `VISPROF_INVALID_ZONE` or `VISPROF_INVALID_COUNTER` as applicable.

The BIOS font is read from the console firmware by KOS at initialization. No
font data is stored in this repository. If atlas allocation or baking fails,
the graph and bars still draw without text. `visprof_init()` returns `0` when
text is available and `-1` when only the graph is available.

## Rendering requirements

Submit the overlay in an open `PVR_LIST_TR_POLY` list using direct rendering.
It uses `pvr_dr_target()` and `pvr_dr_commit(pointer)`. DMA submission is not
supported. The host owns PVR initialization and list management.

The overlay uses `ONE, ZERO` blending and depths from `1.00e9` to `1.04e9`.
Keep host polygon depths below `1.00e9`. Submit the overlay last in presort
mode. Autosort also permits later host submissions below the overlay depth.
The profiler does not change global PVR registers.

Custom PVR settings need translucent bins and sufficient OPB overflow space.
Both examples use `opb_overflow_count = 3`, as in KOS defaults. Too little
space can cause missing geometry at tile boundaries. Larger scenes may need
more space.

Wait for pending tile-accelerator work and rendering to finish before
`visprof_shutdown()` frees the atlas. Shut down before reinitializing. A
second initialization returns `1` and keeps the existing configuration.

## Other build methods

To compile the library as part of an application, copy `include/` and `src/`
together. Compile `src/visprof.c` and `src/visprof_draw.c` with
`-Iinclude -Isrc`. Keep the private header. Clean these objects when changing
`VISPROF_ENABLED`.

The optional [kos-ports metadata](../port/Makefile) targets
`https://github.com/freakdave/libvisprof.git` at tag `v0.1.0`. Once that
repository and tag are published, copy `port/` to `$KOS_PORTS/libvisprof` and
run `make install`. Applications can then link with `-lvisprof`.

`VISPROF_ENABLED` must match across the application and archive. Disabled
builds use inline no-ops and contain no profiler implementation symbols.
Compiler-generated file symbols may remain. This setting is independent of
`NDEBUG`, which KOS defines in normal builds.

## Verification

`make check` runs host timing and layout tests, strict KOS compilation, C/C++
links, build mode transitions, dependency checks and failure checks. It
reports the demo's code and static memory difference between enabled and
disabled builds. It needs native GCC and uses at most two build jobs.

Both demos passed Flycast and Dreamcast hardware checks on 2026-09-10. These
covered display, controls and 2ndmix music. CPU/GPU overhead has not been
measured in an isolated benchmark. `prof` excludes GPU rendering cost. The
font atlas uses 64 KiB of video memory and a temporary 64 KiB CPU buffer
during initialization.
