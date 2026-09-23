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
value is the mean profiler cost over the frames since the panel's last text
rebuild, in both modes. `visprof_self_ms()` returns the last frame alone. See
[Text refresh](#text-refresh).

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
starts after the first `history_len` frames. KOS can compile `dbglog()` out:
stock `KOS_CFLAGS` carry `-DDBGLOG_DISABLED`, so the library's own Makefile
filters that flag out of its compile. A build that does not filter it loses
spike logging silently. If requested logging is unavailable, initialization
prints a warning.

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
Invalid anchors become `VISPROF_TOP_LEFT`. Zero `text_refresh_hz` selects four
panel text rebuilds per second, described in [Text refresh](#text-refresh).

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

## Text refresh

Formatting every panel value at every frame end costs work without making the
numbers more useful. It would format about sixty values per frame, so the
library rebuilds the text on a timed cadence instead.

`text_refresh_hz` sets that cadence in hertz. Zero selects four rebuilds per
second, the default. `VISPROF_TEXT_REFRESH_EVERY_FRAME` rebuilds at every frame
end, which is what releases before 0.4.0 did. The cadence runs on the wall
clock, not on a frame count, because the frame rate varies.

Between two rebuilds the text lines, the phase swatches, the panel size and
the panel position all stand still. These events rebuild the text at the next
frame end whatever the clock says:

- the panel became visible, and the first frame end after initialization.
- a captured frame raised the retained peak, or the previous capture expired,
  so the `captured frame` line, the phase breakdown and the zone box changed.
- a thread scan ran, so the `thread CPU` line changed.
- `visprof_set_mode()` or `visprof_set_anchor()` was called.

A capture that only refreshes the retained frame inside the ten percent
tolerance does not rebuild the text. It repeats numbers already on the panel,
and it happens on nearly every frame once the frame time is steady, so forcing
a rebuild for it would remove the cadence.

The graph is not text. `visprof_draw()` reads the history ring at every frame,
so the bars and the red frame line keep moving at the frame rate whatever the
cadence is. Counters are still sampled at every frame end. The panel shows the
value sampled at the frame of the last rebuild.

`prof` uses the same cadence. It is the mean profiler cost over the
frames since the last rebuild, not one frame's sample, so it is a steady
reading. `visprof_self_ms()` still answers with the last frame alone.

## Capture

`visprof_capture_now(path)` writes the framebuffer that is being scanned out
to `path` as a binary PPM: the ASCII header `P6\n<width> <height>\n255\n` and
then `width * height * 3` bytes of red, green and blue. It returns `0`, or one
of `VISPROF_CAPTURE_EARG`, `EMODE`, `EFORMAT`, `EOPEN` or `EWRITE`, and prints
one line either way. It allocates nothing: one row of red, green and blue
bytes lives on the caller's stack, which is why `VISPROF_CAPTURE_MAX_WIDTH`
(1024) is the widest mode it converts.

The start address comes from the PVR's own display register, `PVR_FB_ADDR`,
and the pixel format from bits 3:2 of `PVR_FB_CFG_1`. Both are what the
hardware is displaying right now, so whichever framebuffer the driver's double
buffering has current is the one that is read. KOS's `vram_l` pointer is not
used: `vid_set_start()` writes the register first and that pointer second, and
any caller of `vid_set_vram()` can move the pointer without moving the
display. Bit 0 of `PVR_FB_CFG_1` must be set, or the call returns `EFORMAT`
because nothing is being displayed. The four framebuffer formats are handled:
RGB555, RGB565, packed RGB888 and RGB0888. Width and height come from
`vid_mode`, because an interlaced mode's display size register holds half the
height. An interlaced framebuffer needs no other special handling: the two
fields are one line apart in one linear image.

The display can flip during a capture, but the buffer being read stays stable.
The view changes only in the vertical blank handler, and only when a render
has completed. This function blocks the frame thread, so no new scene is
submitted while it runs: the render already in flight can complete and swap
once, and nothing writes into the buffer being read.

Capture blocks the frame thread. Reading a 640x480 16-bit framebuffer and
writing 921,600 bytes took 38.5 ms in an emulator writing to the KOS ramdisk,
and a write over dc-load is slower. That time is measured, reported by
`visprof_capture_ms()`, and subtracted from the frame and phase samples of the
frame it happened in, in both timing modes. `VISPROF_EXCLUSIVE` subtracts
profiler overhead because a `raw` reading should show it. A requested
screenshot is separate work, so its cost is removed from both readings. Without
that subtraction one capture would enter the history as a frame of over 100 ms,
become the retained captured frame, and set `max` for the next `history_len`
frames. The FPS row is not adjusted: it counts completed frames over real
elapsed time, so a capture does lower it for one interval.

`visprof_capture_poll()` handles request-file captures. Set `capture_request` and
`capture_dir` together, and the host calls this once a frame:

- at `capture_poll_hz` (default one look per second) it opens `capture_request`.
- when that file exists it reads the first line as the wanted base name,
  accepts letters, digits, `-` and `_` up to `VISPROF_CAPTURE_NAME_MAX` (40)
  characters, and uses `capture` for anything else, including an empty line.
- it deletes the request before capturing, so one request is one picture.
- it captures to `<capture_dir>/<name>.ppm`.

With both strings `NULL`, the default, the function returns at once and opens
nothing. One of the two alone is a configuration error: `visprof_init()`
clears both and prints a warning, as it does for half a span sink.

A quiet poll costs one failed file open per interval, and that cost leaves the
frame and phase samples exactly as a capture's does. Measured on a Dreamcast
over dcload-ip: most of those opens cost a few milliseconds, but about one in
six took roughly 400 ms, which became the panel's `max` and its retained
captured frame. Without excluding that probe, `max` would report filesystem
latency as if it were game work. To measure the probe itself, disable polling
and compare the timings.
`capture_dir` carries no trailing separator.
The directory must exist. Neither dc-load's `/pc` nor the KOS ramdisk can
create one, so on the ramdisk use `/ram` itself.

Run `tools/capture.py` on the host. It writes a request to the directory
dc-tool serves as `/pc`, waits until the picture stops growing, then converts
it to PNG using only the Python standard library:

```
tools/capture.py --root <dc-tool directory> --name shot1 --out shot1.png
tools/capture.py --ppm captures/shot1.ppm --out shot1.png
```

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
| `prof` | Mean measured profiler overhead over the frames since the last text rebuild, in milliseconds |
| `demo quads` | Demo geometry submitted through its quad helper, excluding the overlay |
| Red line | Total frame time |
| White / yellow lines | Reference times for 60 / 30 FPS |
| `ms` | Graph time unit. The zero tick is labelled `0`. |

The first line shows FPS followed by `raw` or `adjusted`. FPS updates over
intervals of at least half a second and includes profiler overhead in both
modes. It shows `--` until the first interval completes. Median and maximum
times are on the next line and follow the selected timing mode.

Counters are sampled at `visprof_frame_end()`. The next text rebuild shows
those values, including decreases and zero. `demo quads` is registered by the demo
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
- No allocation, string formatting or division occurs in `visprof_draw()`.
  Glyph texture coordinates are computed once when the atlas is baked.
- All calls must come from one frame thread. The thread scan briefly masks
  interrupts while it walks KOS's thread list.
- Registration failure returns `VISPROF_INVALID_PHASE`,
  `VISPROF_INVALID_ZONE` or `VISPROF_INVALID_COUNTER` as applicable.

The BIOS font is read from the console firmware by KOS at initialization. No
font data is stored in this repository. If atlas allocation or baking fails,
the graph and bars still draw without text. `visprof_init()` returns `0` when
text is available and `-1` when only the graph is available.

## Rendering requirements

Submit the overlay in an open `PVR_LIST_TR_POLY` list. By default the library
writes each record itself with `pvr_dr_target()` and `pvr_dr_commit(pointer)`,
which requires direct rendering. A host that submits the translucent list some
other way supplies a span sink instead, described below. The host owns PVR
initialization and list management.

### Span sink

`visprof_config_t` carries three optional fields: `reserve`, `commit` and
`user`. Both function pointers `NULL`, the default, selects the direct path
above. Set both and the library stops writing store queues: it asks the host
for a span of memory, writes its finished records straight into that span, and
hands it back. Setting only one would interleave two submission paths in one
list, so `visprof_init()` clears both, prints a warning and keeps direct
rendering.

```c
void *reserve(uint32_t bytes, void *user);
void  commit(void *span, uint32_t bytes, void *user);
```

The span sink matters because KOS prepares store queues only for a list with no
registered DMA vertex buffer. An application that routes its translucent list
through such a buffer cannot use `pvr_dr_target()` and would otherwise have to
change that routing for the frames that show the overlay. Where that buffer is
the span, nothing is copied at all.

The contract:

- `bytes` is a multiple of 32 and never more than `VISPROF_SINK_MAX_SPAN`,
  which is 128: one quad. A header asks for 32.
- `reserve` returns a writable span of exactly `bytes` bytes, aligned to 32,
  that stays valid until the matching `commit`.
- The library writes complete KOS records into the span: one `pvr_poly_hdr_t`
  in a 32-byte span, or four `pvr_vertex_t` in a 128-byte span. Every field is
  set, including `oargb`. The fourth vertex of every quad carries
  `PVR_CMD_VERTEX_EOL` and closes the strip.
- `commit` receives the same pointer and the same byte count. The library
  commits exactly the bytes it reserved, and commits every span it reserved.
- The sink is never re-entered between a `reserve` and its `commit`. Both are
  called on the caller's thread, inside `visprof_draw()`, in submission order.
  Preserve that order.
- `reserve` may return `NULL` to drop that span. The library then skips the
  commit and the whole quad, so no partial strip ever reaches the tile
  accelerator. A host that drops a header span must drop the vertex spans
  behind it as well, or the hardware sees vertices with no header. A monotonic
  cursor does that by itself: no request is smaller than a header.
- The panel must still be the last translucent submission in a presorted
  scene, because the sink only moves the records, not their depth.
- The texture pointer inside the textured header addresses the font atlas and
  stays valid until `visprof_shutdown()`.
- Do not call any `visprof_` function from a sink.
- `user` is passed back unchanged and may be `NULL`.

`visprof_rects_drawn()` and `visprof_glyphs_drawn()` count what was submitted,
so a dropped span is counted for nothing: four vertex records always accompany
each counted quad.

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
together. Compile `src/visprof.c`, `src/visprof_draw.c` and
`src/visprof_capture.c` with `-Iinclude -Isrc`. Keep the private header. Clean
these objects when changing `VISPROF_ENABLED`.

The optional [kos-ports metadata](../port/Makefile) targets
`https://github.com/freakdave/libvisprof.git` at tag `v0.5.0`. Once that
repository and tag are published, copy `port/` to `$KOS_PORTS/libvisprof` and
run `make install`. Applications can then link with `-lvisprof`.

`VISPROF_ENABLED` must match across the application and archive. Disabled
builds use inline no-ops and contain no profiler implementation symbols.
Compiler-generated file symbols may remain. This setting is independent of
`NDEBUG`, which KOS defines in normal builds.

## Verification

`make check` runs host timing and layout tests, span sink tests, strict KOS
compilation, C/C++ links, build mode transitions, dependency checks and
failure checks. It reports the demo's code and static memory difference
between enabled and disabled builds. It needs native GCC and uses at most two
build jobs.

The span sink tests compile `src/visprof_draw.c` for the host against a stub
PVR and run the real drawing code through both paths. Without a sink, every
record reaches `pvr_dr_commit()`. With a sink, store queues stay untouched.
Every request is a multiple of 32 bytes and no larger than 128. Every 32-byte
span holds a header and every 128-byte span holds four vertices ending in
`PVR_CMD_VERTEX_EOL`. Each commit matches its own reserve. The sink is never
re-entered between the two. Both paths submit the same number of records.
A reserve that returns `NULL` for one quad drops exactly that quad, produces
no commit for it, and the rest of the draw still reaches the sink. The tests
also cover an incomplete sink pair and a hidden overlay, and print the spans,
bytes and records of one draw.

The capture tests run the production reader against stub video memory, stub
display registers and a stub filesystem. They check the PPM header and the
body length, one pixel of each of the four framebuffer formats, an odd width
and a second row at the right stride, that the picture follows the display
register when it changes, and every refusal: no path, no mode, a mode too
wide, the display disabled, an open that fails and a write that goes short.
None of these leaves a file behind. The request state machine is covered as
well: no configuration opens nothing at all, half a configuration is cleared
at initialization, a quiet poll is one failed open per interval and no more,
a request becomes exactly one picture named by its first line, the request is
deleted before the picture is opened, and five kinds of unusable name all
become `capture` while forty characters still count as a name. The capture's
time is excluded from frame and phase timings in both modes.

Timing and drawing tests both cover text refresh. In timing tests, the stub clock
is advanced by hand: at four rebuilds per second the panel repeats its lines
until 250 ms have passed, a mode change, an anchor change, the panel becoming
visible, a spike capture and a thread scan each rebuild them at the next frame
end, a capture inside the ten percent tolerance does not, and the `prof` row
holds the mean over the frames of the interval. In the sink tests one more
frame changes the committed bar geometry while the text lines stay identical,
and the glyph coordinate table is compared with the divisions it replaced, for
every character at both text sizes.

Both demos passed Flycast and Dreamcast hardware checks on 2026-09-10. These
covered display, controls and 2ndmix music. CPU/GPU overhead has not been
measured in an isolated benchmark. `prof` excludes GPU rendering cost. The
font atlas uses 64 KiB of video memory and a temporary 64 KiB CPU buffer
during initialization.
