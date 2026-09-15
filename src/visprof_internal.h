/*
 * MIT License
 *
 * Copyright (c) 2026 David Reichelt
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * SPDX-License-Identifier: MIT
 */

/* Private state shared by timing and drawing. */
#ifndef VISPROF_INTERNAL_H
#define VISPROF_INTERNAL_H

#include "visprof/visprof.h"

#if VISPROF_ENABLED

#include <stdint.h>
#include <dc/pvr.h>
#include <kos/timer.h>

/* Use elapsed time. SH4 performance counters stop during CPU sleep. */
static inline uint64_t vp_now_ns(void) {
    return timer_ns_gettime64();
}

/* Clamp before narrowing to avoid a 64-bit float conversion on SH4. */
static inline float vp_ns_to_ms(uint64_t ns) {
    if (ns > 4000000000ull)
        ns = 4000000000ull;
    return (float)(uint32_t)ns * 1.0e-6f;
}

static inline float vp_elapsed_ms(uint64_t start_ns, uint64_t end_ns) {
    return (end_ns > start_ns) ? vp_ns_to_ms(end_ns - start_ns) : 0.0f;
}

static inline uint64_t vp_elapsed_ns(uint64_t start_ns, uint64_t end_ns) {
    return (end_ns > start_ns) ? end_ns - start_ns : 0;
}

#define VP_ATLAS_W      256
#define VP_ATLAS_H      128
#define VP_CELL_W        13
#define VP_CELL_H        25
#define VP_COLS          19
#define VP_FONT_W        12
#define VP_FONT_H        24
#define VP_FIRST_CH      32
#define VP_LAST_CH      126
#define VP_GLYPH_COUNT  (VP_LAST_CH - VP_FIRST_CH + 1)

#define VP_LINE_CHARS    48
#define VP_MAX_TEXTS     48
#define VP_MAX_THREADS    8
#define VP_COUNTER_ROWS   4
#define VP_COUNTER_COLS  32

#define VP_BAKE_NO_INK   1
#define VP_BAKE_NO_RAM  (-1)
#define VP_BAKE_NO_VRAM (-2)

#define VP_GRID_STEP_MS  8.0f
#define VP_GRID_MAX_MS  32.0f
#define VP_GRAPH_MS     36.0f

/* Depth layers are spaced far enough apart to remain distinct as floats. */
#define VP_Z_PANEL     1.00e9f
#define VP_Z_GUIDE     1.005e9f
#define VP_Z_BAR       1.01e9f
#define VP_Z_BAR_STEP  1.0e6f
#define VP_Z_LINE      1.03e9f
#define VP_Z_TEXT      1.04e9f

typedef struct { float phase_ms[VISPROF_MAX_PHASES]; float frame_ms; } vp_sample_t;
typedef struct { const char *name; uint64_t start_ns; float ms; uint8_t depth; } vp_zone_t;
typedef struct { const char *name; float ms; } vp_top_t;
typedef struct { const char *name; const uint32_t *cell; uint32_t shown; } vp_counter_t;
typedef struct { int tid; uint64_t cpu_ms; uint32_t seen_scan; } vp_thread_t;
typedef struct { float x, y; char s[VP_LINE_CHARS]; } vp_text_t;
typedef struct { float x, y; uint32_t argb; } vp_swatch_t;

/* Atlas corners of one glyph cell, computed once when the atlas is baked so
 * that drawing a glyph needs no division. */
typedef struct { float u0, v0, u1, v1; } vp_uv_t;

typedef struct {
    int inited;
    int visible;
    int text_ok;
    visprof_config_t cfg;

    uint32_t    canary_pre;
    vp_sample_t hist[VISPROF_MAX_HISTORY];
    uint32_t    canary_post;

    uint32_t hist_len;
    uint32_t index;
    uint32_t frame_count;
    uint32_t last_anomalous_slot;
    uint64_t fps_start_ns;
    uint32_t fps_frames;
    float fps;
    uint64_t frame_start_ns;
    uint64_t phase_start_ns[VISPROF_MAX_PHASES];
    uint64_t last_end_ns;

    const char *phase_name[VISPROF_MAX_PHASES];
    uint32_t    phase_argb[VISPROF_MAX_PHASES];
    uint32_t    phase_count;

    vp_zone_t zone[VISPROF_MAX_ZONES];
    uint32_t  zone_count;
    uint32_t  zone_dropped;
    uint8_t   zone_depth;

    vp_top_t top[VISPROF_TOP_ZONES];
    uint32_t top_count;
    uint32_t top_capture_frame;
    float    top_frame_ms;
    float    top_phase_ms[VISPROF_MAX_PHASES];
    float    top_peak_ms;         /* Reference for the 10% capture tolerance. */
    uint32_t top_dropped;

    uint32_t last_spike_log_frame;

    vp_counter_t counter[VISPROF_MAX_COUNTERS];
    uint32_t     counter_count;

    uint32_t glyphs_drawn;
    uint32_t rects_drawn;

    visprof_phase_t open_phase;
    uint64_t self_phase_ns[VISPROF_MAX_PHASES];
    uint64_t self_frame_ns;
    uint64_t carry_self_ns;       /* Frame-end cost charged to the next frame. */
    float    self_ms;

    /* Screen capture. Its cost is held apart from self_* because it is
     * subtracted in BOTH timing modes: a capture is a one-shot operation the
     * user asked for, not profiler overhead a `raw` reading should show. */
    uint64_t capture_phase_ns[VISPROF_MAX_PHASES];
    uint64_t capture_frame_ns;
    uint64_t capture_poll_ns;     /* Period between two looks for a request. */
    uint64_t capture_next_ns;     /* Wall clock of the next look. */
    float    capture_ms;          /* Elapsed time of the last capture. */

    vp_thread_t thread[VP_MAX_THREADS];
    char        thread_line[VP_LINE_CHARS];
    uint32_t    last_scan_frame;
    uint32_t    last_run_frame;
    uint32_t    scan_generation;
    int         scan_armed;
    uint64_t    last_scan_wall_ns;

    float pending_scan_ms;
    float pending_log_ms;

    vp_text_t   text[VP_MAX_TEXTS];
    uint32_t    text_count;
    vp_swatch_t swatch[VISPROF_MAX_PHASES];
    uint32_t    swatch_count;

    uint64_t text_interval_ns;  /* Rebuild period. Zero rebuilds every frame. */
    uint64_t last_build_ns;     /* Wall clock of the last text rebuild. */
    int      text_forced;       /* An event needs a rebuild at the next end. */
    float    self_sum_ms;       /* Sum of self_ms since the last rebuild. */
    uint32_t self_samples;      /* Frames in that sum. */
    float    display_self_ms;   /* The mean the panel shows as `prof`. */

    float panel_x, panel_y, panel_w, panel_h;
    float graph_x, graph_base_y, graph_w, graph_h;
    float guide_x2;
    float zbox_x, zbox_y, zbox_w, zbox_h;
    float zbox_w_hw;
    float glyph_w, glyph_h;
    float char_w, text_h;
    int   layout_valid;
} vp_state_t;

extern vp_state_t visprof_g;

/* Filled by visprof_atlas_bake(), indexed by character minus VP_FIRST_CH. */
extern vp_uv_t visprof_uv[VP_GLYPH_COUNT];

int  visprof_atlas_bake(uint32_t panel_argb, int half);
void visprof_atlas_free(void);

#endif
#endif
