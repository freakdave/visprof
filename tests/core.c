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
#include "visprof_internal.h"
#include <dc/video.h>
#include <dc/biosfont.h>
#include <kos/thread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static vp_vid_mode_t screen = { 640, 480 };
vp_vid_mode_t *vid_mode = &screen;
unsigned test_irq_mask;
static uint64_t now_ns;
static kthread_t main_thread = { 1, "main" };
static kthread_t worker = { 2, "worker" };
static uint64_t worker_cpu_ms;
static int worker_present;
static int atlas_result;
static int failures;
static int checks;
static unsigned panic_lines;

#define CHECK(expr) do { \
    ++checks; \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
        ++failures; \
    } \
} while (0)

static int near(float a, float b) { return a > b - 0.01f && a < b + 0.01f; }
uint64_t timer_ns_gettime64(void) { return now_ns; }
kthread_t *thd_get_current(void) { return &main_thread; }
uint64_t thd_get_cpu_time(kthread_t *thread) {
    CHECK(test_irq_mask != 0);
    return thread == &worker ? worker_cpu_ms : 0;
}
int thd_each(int (*cb)(kthread_t *, void *), void *data) {
    CHECK(test_irq_mask != 0);
    cb(&main_thread, data);
    return worker_present ? cb(&worker, data) : 0;
}
int dbglog(int level, const char *fmt, ...) { (void)level; (void)fmt; return 0; }
int visprof_atlas_bake(uint32_t color, int half) {
    (void)color; (void)half;
    return atlas_result;
}
void visprof_atlas_free(void) {}
void bfont_draw_str_vram_fmt(uint32_t x, uint32_t y, bool opaque, const char *fmt, ...) {
    char line[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    (void)opaque;
    CHECK(x + strlen(line) * 12 <= screen.width);
    CHECK(y + 24 <= screen.height);
    for (const unsigned char *p = (const unsigned char *)line; *p; ++p)
        CHECK(*p >= 32 && *p <= 126);
    ++panic_lines;
}

/* Every case below reads the panel text of the frame it just ran, so unless a
 * case chose a refresh rate it gets the every-frame rate, which is what the
 * library did before 0.4.0. The cadence has its own cases at the end. */
static void reset(const visprof_config_t *cfg) {
    visprof_config_t use = { 0 };
    if (cfg != NULL)
        use = *cfg;
    if (use.text_refresh_hz == 0)
        use.text_refresh_hz = VISPROF_TEXT_REFRESH_EVERY_FRAME;
    visprof_shutdown();
    now_ns = 1000000000;
    screen.width = 640;
    screen.height = 480;
    worker_present = 0;
    worker_cpu_ms = 0;
    test_irq_mask = 0;
    atlas_result = 0;
    CHECK(visprof_init(&use) == 0);
}
static void frame(float ms) {
    visprof_frame_begin();
    now_ns += (uint64_t)(ms * 1000000.0f);
    visprof_frame_end();
}
static int text_has(const char *part) {
    for (uint32_t i = 0; i < visprof_g.text_count; ++i)
        if (strstr(visprof_g.text[i].s, part)) return 1;
    return 0;
}

static void timing(void) {
    visprof_config_t cfg = { 0 };
    cfg.history_len = 1;
    reset(&cfg);
    visprof_phase_t phase = visprof_phase_register("Sim", 0);
    visprof_frame_begin();
    visprof_phase_begin(phase);
    visprof_zone_t parent = visprof_zone_begin("parent");
    now_ns += 1000000;
    visprof_zone_t child = visprof_zone_begin("child");
    now_ns += 2000000;
    visprof_zone_end(child);
    visprof_zone_end(parent);
    visprof_phase_end(phase);
    visprof_frame_end();
    CHECK(near(visprof_last_frame_ms(), 3));
    CHECK(near(visprof_last_phase_ms(phase), 3));
    CHECK(near(visprof_g.zone[parent].ms, 3));
    CHECK(near(visprof_g.zone[child].ms, 2));
    now_ns += 4000000;
    frame(1);
    CHECK(near(visprof_last_frame_ms(), 5));
    CHECK(near(visprof_last_phase_ms(phase), 0));
    visprof_set_mode(VISPROF_EXCLUSIVE);
    visprof_frame_begin();
    visprof_phase_begin(phase);
    now_ns += 5000000;
    visprof_g.self_phase_ns[phase] = 1000000;
    visprof_g.self_frame_ns = 1000000;
    visprof_phase_end(phase);
    visprof_frame_end();
    CHECK(near(visprof_last_frame_ms(), 4));
    CHECK(near(visprof_last_phase_ms(phase), 4));
    CHECK(near(visprof_self_ms(), 1));
    CHECK(near(vp_elapsed_ms(20, 10), 0));
    CHECK(near(vp_elapsed_ms(0, 5000000000ull), 4000));
    CHECK(vp_elapsed_ns(20, 10) == 0);
    visprof_set_mode((visprof_mode_t)99);
    CHECK(visprof_get_mode() == VISPROF_INCLUSIVE);
    visprof_frame_begin();
    parent = visprof_zone_begin(NULL);
    now_ns += 1000000;
    visprof_zone_end(parent);
    CHECK(visprof_g.zone[parent].name != NULL);
    if (visprof_g.zone[parent].name != NULL)
        CHECK(strcmp(visprof_g.zone[parent].name, "?") == 0);
    visprof_frame_end();
}

static void capture(void) {
    reset(NULL);
    const float samples[] = { 30, 29, 28, 27, 26, 25 };
    for (unsigned i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i)
        frame(samples[i]);
    CHECK(near(visprof_g.top_frame_ms, 27));
    frame(29.7f);
    CHECK(near(visprof_g.top_frame_ms, 29.7f));
    for (unsigned i = 0; i < 181; ++i) frame(10);
    CHECK(near(visprof_g.top_frame_ms, 10));

    reset(NULL);
    visprof_set_visible(1);
    visprof_phase_t phase = visprof_phase_register("Simulation", 0);
    visprof_frame_begin();
    visprof_phase_begin(phase);
    now_ns += 12000000;
    visprof_phase_end(phase);
    visprof_zone_add_ms("first", 12);
    now_ns += 18000000;
    visprof_frame_end();
    visprof_frame_begin();
    visprof_phase_begin(phase);
    now_ns += 7000000;
    visprof_phase_end(phase);
    visprof_zone_add_ms("second", 7);
    now_ns += 21000000;
    visprof_frame_end();
    CHECK(text_has("max 30.0 ms"));
    CHECK(text_has("captured frame 28.0 ms"));
    CHECK(text_has("Simulation 7.0 ms"));
    CHECK(text_has("other 21.0 ms"));
    CHECK(text_has("second 7.0 ms"));
    CHECK(!text_has("first 12.0 ms"));
    frame(10);
    CHECK(text_has("captured frame 28.0 ms"));
    CHECK(text_has("Simulation 7.0 ms"));
    CHECK(!text_has("dT=") && !text_has("(+"));
    visprof_set_mode(VISPROF_EXCLUSIVE);
    frame(10);
    CHECK(text_has(" adjusted"));

}

static void median_zero_samples(void) {
    visprof_config_t cfg = { 0 };
    cfg.history_len = 4;
    reset(&cfg);
    visprof_set_visible(1);
    frame(10);
    CHECK(text_has("med 10.0 max 10.0 ms"));
    frame(0);
    frame(0);
    CHECK(text_has("med 0.0 max 10.0 ms"));
    frame(20);
    CHECK(text_has("med 0.0 max 20.0 ms"));
}

static void fps_readout(void) {
    reset(NULL);
    visprof_set_visible(1);
    frame(20);
    CHECK(text_has("FPS -- raw"));
    for (unsigned i = 1; i < 25; ++i) frame(20);
    CHECK(text_has("FPS 50.0 raw"));
    visprof_set_mode(VISPROF_EXCLUSIVE);
    for (unsigned i = 0; i < 25; ++i) {
        visprof_frame_begin();
        now_ns += 20000000;
        visprof_g.self_frame_ns = 5000000;
        visprof_frame_end();
    }
    CHECK(near(visprof_last_frame_ms(), 15));
    CHECK(text_has("FPS 50.0 adjusted"));
    visprof_set_mode(VISPROF_INCLUSIVE);
    for (unsigned i = 0; i < 5; ++i) frame(100);
    CHECK(text_has("FPS 10.0 raw"));
    visprof_set_visible(0);
    for (unsigned i = 0; i < 25; ++i) frame(20);
    CHECK(near(visprof_g.fps, 50));
    now_ns -= 1000000000;
    frame(0);
    CHECK(near(visprof_g.fps, 0));
}

static void layout(void) {
    static const char *names[] = { "SimulationWithLongName", "RenderingWithLongName",
                                  "WaitingWithLongName", "OtherLongPhase" };
    static uint32_t counter = 4294967295u;
    for (int font = 0; font < 2; ++font) {
        for (int anchor = 0; anchor < 9; ++anchor) {
            visprof_config_t cfg = { 0 };
            cfg.text_scale = font ? 1.0f : 0.5f;
            cfg.anchor = (visprof_anchor_t)anchor;
            reset(&cfg);
            for (unsigned p = 0; p < 4; ++p) visprof_phase_register(names[p], 0);
            CHECK(visprof_phase_register("overflow", 0) == VISPROF_INVALID_PHASE);
            visprof_counter_register("counter_name_too_long", &counter);
            visprof_set_visible(1);
            frame(16);
            float x = visprof_g.graph_x, y = visprof_g.graph_base_y;
            for (unsigned f = 0; f < 30; ++f) {
                visprof_frame_begin();
                visprof_zone_add_ms("measured", 2);
                now_ns += 16000000;
                visprof_frame_end();
            }
            CHECK(near(x, visprof_g.graph_x) && near(y, visprof_g.graph_base_y));
            CHECK(visprof_g.panel_x >= 0 && visprof_g.panel_y >= 0);
            CHECK(visprof_g.panel_x + visprof_g.panel_w <= 640);
            CHECK(visprof_g.panel_y + visprof_g.panel_h <= 480);
            CHECK(text_has("4294967295"));
            for (uint32_t t = 0; t < visprof_g.text_count; ++t) {
                const vp_text_t *line = &visprof_g.text[t];
                CHECK(line->x >= 0 && line->y >= 0);
                CHECK(line->x + strlen(line->s) * visprof_g.char_w <= 640);
                CHECK(line->y + visprof_g.text_h <= 480);
            }
        }
    }
    reset(NULL);
    visprof_set_visible(1);
    visprof_frame_begin();
    for (unsigned i = 0; i < 33; ++i) visprof_zone_add_ms("short", 0.01f);
    now_ns += 16000000;
    visprof_frame_end();
    CHECK(visprof_g.top_count == 0);
    CHECK(text_has("1 more zones dropped"));
    reset(NULL);
    visprof_set_visible(1);
    visprof_counter_register("demo quads", &counter);
    frame(16);
    CHECK(visprof_g.panel_h < 380);
    CHECK(text_has("prof 0.00 ms"));
    CHECK(text_has("demo quads 4294967295"));
    counter = 7;
    frame(16);
    CHECK(text_has("demo quads 7"));
    counter = 0;
    frame(16);
    CHECK(text_has("demo quads 0"));
    CHECK(!text_has("gly ") && !text_has("rct "));
    visprof_set_visible(0);
    frame(17);
    CHECK(near(visprof_last_frame_ms(), 17));
    visprof_set_visible(1);
    frame(16);
    CHECK(visprof_g.layout_valid != 0);
    reset(NULL);
    visprof_phase_register("", 0);
    visprof_set_visible(1);
    frame(16);
    CHECK(text_has("? 0.0 ms"));
}

static void threads(void) {
    visprof_config_t cfg = { 0 };
    cfg.history_len = 2;
    cfg.thief_scan = 1;
    reset(&cfg);
    worker_present = 1;
    worker_cpu_ms = 20;
    for (unsigned i = 0; i < 61; ++i) frame(20);
    CHECK(visprof_g.last_scan_wall_ns != 0);
    worker_cpu_ms += 100;
    for (unsigned i = 0; i < 60; ++i) frame(20);
    CHECK(strstr(visprof_g.thread_line, "83.3 ms/s") != NULL);
    CHECK(test_irq_mask == 0);
    worker_present = 0;
    for (unsigned i = 0; i < 60; ++i) frame(20);
    for (unsigned i = 0; i < VP_MAX_THREADS; ++i)
        CHECK(visprof_g.thread[i].tid != worker.tid);
    worker.tid = 3;
    worker_present = 1;
    for (unsigned i = 0; i < 60; ++i) frame(20);
    worker_cpu_ms += 60;
    for (unsigned i = 0; i < 60; ++i) frame(20);
    CHECK(strstr(visprof_g.thread_line, "50.0 ms/s") != NULL);
}

static void panic_bounds(void) {
    reset(NULL);
    static const char name[] = "long\n\t\xff_name_that_cannot_fit_on_one_small_screen_row";
    for (unsigned i = 0; i < 4; ++i) visprof_phase_register(name, 0);
    visprof_frame_begin();
    for (unsigned i = 0; i < 8; ++i) visprof_zone_add_ms(name, 1);
    now_ns += 16000000;
    visprof_frame_end();
    screen.width = 160;
    screen.height = 72;
    panic_lines = 0;
    visprof_panic_dump();
    CHECK(panic_lines == 2);
    visprof_shutdown();
    atlas_result = -1;
    CHECK(visprof_init(NULL) == -1);
    visprof_set_visible(1);
    frame(16);
    CHECK(visprof_g.text_ok == 0 && visprof_g.zbox_h == 0);
    visprof_shutdown();
}

/* ── Text refresh cadence (0.4.0) ────────────────────────────────────────── */

static uint32_t tick;

/* A registered counter is the cheapest thing that changes what the panel
 * says: it is sampled at every frame end, but only DRAWN when the text is
 * rebuilt, so "tick 2" appears exactly when a rebuild happened. */
static void cadence_start(uint32_t hz, float peak_ms) {
    visprof_config_t cfg = { 0 };
    cfg.history_len = 180;
    cfg.text_refresh_hz = hz;
    reset(&cfg);
    visprof_counter_register("tick", &tick);
    visprof_set_visible(1);
    tick = 1;
    /* One large frame first. It leaves a high retained peak, so the ordinary
     * frames that follow are outside the ten percent capture band and do not
     * force a rebuild of their own. */
    frame(peak_ms);
    CHECK(text_has("tick 1"));
}

static void cadence_clock(void) {
    cadence_start(4, 60);            /* four rebuilds per second */
    tick = 2;
    /* Ten 16 ms frames are 160 ms, under the 250 ms period. */
    for (unsigned i = 0; i < 10; ++i) frame(16);
    CHECK(text_has("tick 1"));
    CHECK(!text_has("tick 2"));
    /* Six more cross 250 ms, so that frame end rebuilds. */
    for (unsigned i = 0; i < 6; ++i) frame(16);
    CHECK(text_has("tick 2"));
    CHECK(!text_has("tick 1"));

    /* The bars read the history every frame, so the graph is never stale:
     * the sample the panel text was built from is already two frames old. */
    tick = 3;
    frame(16);
    frame(24);
    CHECK(!text_has("tick 3"));
    CHECK(near(visprof_last_frame_ms(), 24));
}

/* Each event rebuilds the text at the next frame end, whatever the clock says.
 * Every block spends two 16 ms frames, far inside the 250 ms period, so only
 * the event can explain the rebuild. */
static void cadence_events(void) {
    cadence_start(4, 60);
    tick = 2;
    frame(16);
    CHECK(!text_has("tick 2"));
    visprof_set_mode(VISPROF_EXCLUSIVE);
    frame(16);
    CHECK(text_has("tick 2"));

    tick = 3;
    frame(16);
    CHECK(!text_has("tick 3"));
    visprof_set_anchor(VISPROF_TOP_RIGHT);
    frame(16);
    CHECK(text_has("tick 3"));

    visprof_set_visible(0);
    tick = 4;
    frame(16);
    CHECK(!text_has("tick 4"));
    visprof_set_visible(1);
    frame(16);
    CHECK(text_has("tick 4"));

    /* A spike raises the retained peak, so the captured frame, the phase
     * breakdown and the zone box all changed. */
    tick = 5;
    frame(16);
    CHECK(!text_has("tick 5"));
    frame(90);
    CHECK(text_has("tick 5"));
    CHECK(text_has("captured frame 90.0 ms"));

    /* A capture that only refreshes inside the ten percent band repeats the
     * numbers already on the panel, so it waits for the cadence. */
    tick = 6;
    frame(85);
    CHECK(near(visprof_g.top_frame_ms, 85));
    CHECK(!text_has("tick 6"));
}

/* A thread scan rewrites the thread CPU line. It runs at most once per 60
 * frames, so the frames here are short enough to stay inside one refresh
 * period of a whole second. */
static void cadence_thread_scan(void) {
    visprof_config_t cfg = { 0 };
    cfg.history_len = 4;
    cfg.thief_scan = 1;
    cfg.text_refresh_hz = 1;
    reset(&cfg);
    visprof_counter_register("tick", &tick);
    worker_present = 1;
    worker_cpu_ms = 20;
    visprof_set_visible(1);
    tick = 1;
    frame(1);
    CHECK(text_has("tick 1"));
    tick = 2;
    for (unsigned i = 0; i < 59; ++i) frame(1);
    CHECK(visprof_g.last_scan_frame == 0);
    CHECK(!text_has("tick 2"));
    frame(1);
    CHECK(visprof_g.last_scan_frame != 0);
    CHECK(text_has("tick 2"));
}

/* `prof` on the panel is the mean over the frames since the last rebuild. */
static void prof_mean(void) {
    visprof_config_t cfg = { 0 };
    cfg.history_len = 180;
    cfg.text_refresh_hz = 16;        /* 62.5 ms between rebuilds */
    reset(&cfg);
    visprof_set_visible(1);
    frame(60);
    CHECK(text_has("prof 0.00 ms"));

    static const uint64_t cost_ns[4] = { 1000000, 2000000, 3000000, 4000000 };
    for (unsigned i = 0; i < 4; ++i) {
        visprof_frame_begin();
        now_ns += 16000000;
        visprof_g.self_frame_ns = cost_ns[i];
        visprof_frame_end();
        if (i < 3)
            CHECK(text_has("prof 0.00 ms"));   /* 48 ms: not due yet */
    }
    /* 64 ms crossed the period on the fourth frame. */
    CHECK(text_has("prof 2.50 ms"));
    CHECK(near(visprof_self_ms(), 4));

    /* A hidden panel builds no text, so its frames belong to no interval. The
     * reading that appears when it is shown again is that frame alone. */
    visprof_set_visible(0);
    for (unsigned i = 0; i < 20; ++i) {
        visprof_frame_begin();
        now_ns += 16000000;
        visprof_g.self_frame_ns = 9000000;
        visprof_frame_end();
    }
    CHECK(visprof_g.self_samples == 0);
    CHECK(text_has("prof 2.50 ms"));
    visprof_set_visible(1);
    visprof_frame_begin();
    now_ns += 16000000;
    visprof_g.self_frame_ns = 1000000;
    visprof_frame_end();
    CHECK(text_has("prof 1.00 ms"));
}

int main(void) {
    timing(); capture(); median_zero_samples(); fps_readout(); layout(); threads(); panic_bounds();
    cadence_clock(); cadence_events(); cadence_thread_scan(); prof_mean();
    printf("core: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
