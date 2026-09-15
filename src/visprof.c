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

/* Frame timing and display layout. */
#include "visprof_internal.h"

#if !defined(VISPROF_ENABLED)
#  error "visprof/visprof.h did not define VISPROF_ENABLED - include path is wrong"
#endif

#if VISPROF_ENABLED

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <dc/biosfont.h>
#include <dc/video.h>
#include <kos/dbglog.h>
#include <kos/irq.h>
#include <kos/thread.h>
#include <kos/timer.h>

vp_state_t visprof_g;
static const char vp_unknown_zone[] = "?";

static const uint32_t vp_default_palette[VISPROF_MAX_PHASES] = {
    0xFF4C8CFFu, 0xFF00CC66u, 0xFFB050C8u, 0xFFFFAA33u
};

#define VP_CANARY 0xCAFEBABEu

int visprof_init(const visprof_config_t *cfg) {
    if (visprof_g.inited)
        return 1;

    memset(&visprof_g, 0, sizeof(visprof_g));
    if (cfg != NULL)
        visprof_g.cfg = *cfg;

    if (visprof_g.cfg.history_len == 0) visprof_g.cfg.history_len = 180;
    if (visprof_g.cfg.history_len < 2) visprof_g.cfg.history_len = 2;
    if (visprof_g.cfg.history_len > VISPROF_MAX_HISTORY) visprof_g.cfg.history_len = VISPROF_MAX_HISTORY;
    if (visprof_g.cfg.panel_argb == 0) visprof_g.cfg.panel_argb = 0xFF181818u;
    if (visprof_g.cfg.text_scale <= 0.0f) visprof_g.cfg.text_scale = 1.0f;
    if (visprof_g.cfg.px_per_ms <= 0.0f) visprof_g.cfg.px_per_ms = 3.0f;
    if ((uint32_t)visprof_g.cfg.anchor > VISPROF_BOTTOM_RIGHT) visprof_g.cfg.anchor = VISPROF_TOP_LEFT;
    if (visprof_g.cfg.text_refresh_hz == 0) visprof_g.cfg.text_refresh_hz = 4;
    if (visprof_g.cfg.mode != VISPROF_INCLUSIVE && visprof_g.cfg.mode != VISPROF_EXCLUSIVE)
        visprof_g.cfg.mode = VISPROF_INCLUSIVE;

    /* Every record of one list must travel the same path. A reserve without a
     * commit would leak the host's span and never publish it; a commit without
     * a reserve would never be called. Either half alone also interleaves
     * store-queue records with host records and corrupts the list, so an
     * incomplete pair falls back to direct rendering. */
    if ((visprof_g.cfg.reserve == NULL) != (visprof_g.cfg.commit == NULL)) {
        printf("visprof: reserve and commit must be set together. "
               "Using direct rendering.\n");
        visprof_g.cfg.reserve = NULL;
        visprof_g.cfg.commit = NULL;
    }

    /* A request file with nowhere to write the picture, or a directory with
     * nothing asking for one, is half a feature. Fail closed, as the sink
     * pair does. */
    if ((visprof_g.cfg.capture_request == NULL) != (visprof_g.cfg.capture_dir == NULL)) {
        printf("visprof: capture_request and capture_dir must be set together. "
               "Capture disabled.\n");
        visprof_g.cfg.capture_request = NULL;
        visprof_g.cfg.capture_dir = NULL;
    }
    if (visprof_g.cfg.capture_poll_hz == 0)
        visprof_g.cfg.capture_poll_hz = 1;
    visprof_g.capture_poll_ns = 1000000000ull / visprof_g.cfg.capture_poll_hz;

    /* The panel text is rebuilt on a wall clock, not on a frame count: the
     * frame rate varies. VISPROF_TEXT_REFRESH_EVERY_FRAME divides to zero
     * here, and a zero period is always due. */
    visprof_g.text_interval_ns = 1000000000ull / visprof_g.cfg.text_refresh_hz;
    visprof_g.last_build_ns = 0;
    visprof_g.text_forced = 1;

    visprof_g.hist_len = visprof_g.cfg.history_len;
    visprof_g.canary_pre = VP_CANARY;
    visprof_g.canary_post = VP_CANARY;
    visprof_g.last_anomalous_slot = 0xFFFFFFFFu;
    visprof_g.scan_armed = 1;
    visprof_g.open_phase = VISPROF_MAX_PHASES;

    for (uint32_t i = 0; i < VP_MAX_THREADS; ++i)
        visprof_g.thread[i].tid = -1;

    const int half = (visprof_g.cfg.text_scale < 1.0f);
    visprof_g.glyph_w = (float)(half ? VP_FONT_W / 2 : VP_FONT_W);
    visprof_g.glyph_h = (float)(half ? VP_FONT_H / 2 : VP_FONT_H);
    visprof_g.char_w = visprof_g.glyph_w;
    visprof_g.text_h = visprof_g.glyph_h;

    const int bake = visprof_atlas_bake(visprof_g.cfg.panel_argb, half);
    visprof_g.text_ok = (bake == 0);
    if (bake == VP_BAKE_NO_INK)
        printf("visprof: BIOS font is empty. Text disabled.\n");
    else if (bake == VP_BAKE_NO_VRAM)
        printf("visprof: 64 KiB font atlas allocation failed in video memory. Text disabled.\n");
    else if (bake == VP_BAKE_NO_RAM)
        printf("visprof: 64 KiB font buffer allocation failed in RAM. Text disabled.\n");

#if DBGLOG_LEVEL_SUPPORT < DBG_INFO

    if (visprof_g.cfg.spike_log)
        printf("visprof: spike_log unavailable at the compiled KOS log level\n");
#endif

    visprof_g.inited = 1;
    return visprof_g.text_ok ? 0 : -1;
}

void visprof_shutdown(void) {
    if (!visprof_g.inited)
        return;
    visprof_atlas_free();
    memset(&visprof_g, 0, sizeof(visprof_g));
}

visprof_phase_t visprof_phase_register(const char *name, uint32_t argb) {

    if (visprof_g.phase_count >= VISPROF_MAX_PHASES || name == NULL)
        return VISPROF_INVALID_PHASE;
    const visprof_phase_t p = visprof_g.phase_count++;
    visprof_g.phase_name[p] = name;
    visprof_g.phase_argb[p] = (argb != 0) ? argb : vp_default_palette[p];
    return p;
}

visprof_counter_t visprof_counter_register(const char *name, const uint32_t *host_cell) {
    if (visprof_g.counter_count >= VISPROF_MAX_COUNTERS || host_cell == NULL || name == NULL)
        return VISPROF_INVALID_COUNTER;
    const visprof_counter_t c = visprof_g.counter_count++;
    visprof_g.counter[c].name = name;
    visprof_g.counter[c].cell = host_cell;
    return c;
}

visprof_zone_t visprof_zone_begin(const char *literal_name) {
    if (!visprof_g.inited)
        return VISPROF_INVALID_ZONE;
    if (visprof_g.zone_count >= VISPROF_MAX_ZONES) {
        ++visprof_g.zone_dropped;
        return VISPROF_INVALID_ZONE;
    }
    vp_zone_t *z = &visprof_g.zone[visprof_g.zone_count];
    z->name = (literal_name != NULL) ? literal_name : vp_unknown_zone;
    z->start_ns = vp_now_ns();
    z->ms = 0.0f;
    z->depth = visprof_g.zone_depth++;
    return visprof_g.zone_count++;
}

void visprof_zone_end(visprof_zone_t z) {
    if (!visprof_g.inited)
        return;

    if (z >= visprof_g.zone_count)
        return;
    visprof_g.zone[z].ms = vp_elapsed_ms(visprof_g.zone[z].start_ns, vp_now_ns());
    if (visprof_g.zone_depth > 0)
        --visprof_g.zone_depth;
}

void visprof_zone_add_ms(const char *literal_name, float ms) {
    if (!visprof_g.inited)
        return;
    if (visprof_g.zone_count >= VISPROF_MAX_ZONES) {
        ++visprof_g.zone_dropped;
        return;
    }
    vp_zone_t *z = &visprof_g.zone[visprof_g.zone_count++];
    z->name = (literal_name != NULL) ? literal_name : vp_unknown_zone;
    z->start_ns = 0;
    z->ms = ms;
    z->depth = visprof_g.zone_depth;
}

void visprof_frame_begin(void) {
    if (!visprof_g.inited)
        return;

    const uint64_t entry_ns = vp_now_ns();

    visprof_g.self_frame_ns = visprof_g.carry_self_ns;
    /* Consume the previous frame-end cost once. */
    visprof_g.carry_self_ns = 0;
    memset(visprof_g.self_phase_ns, 0, sizeof(visprof_g.self_phase_ns));
    /* capture_frame_ns is NOT cleared here. A capture taken between two
     * frames still belongs to the period that visprof_frame_end() closes,
     * because that period starts at the previous frame end. Frame end clears
     * it after subtracting it. */
    memset(visprof_g.capture_phase_ns, 0, sizeof(visprof_g.capture_phase_ns));

    visprof_g.zone_count = 0;
    visprof_g.zone_dropped = 0;
    visprof_g.zone_depth = 0;

    /* Include work between frames by measuring from the previous frame end. */
    visprof_g.frame_start_ns = (visprof_g.last_end_ns != 0) ? visprof_g.last_end_ns : vp_now_ns();

    /* Deferred diagnostics belong to this frame. Their costs are already
     * subtracted in exclusive mode. */
    if (visprof_g.cfg.mode == VISPROF_INCLUSIVE) {
        if (visprof_g.pending_scan_ms > 0.0f)
            visprof_zone_add_ms("visprof thread scan", visprof_g.pending_scan_ms);
        if (visprof_g.pending_log_ms > 0.0f)
            visprof_zone_add_ms("visprof spike log", visprof_g.pending_log_ms);
    }
    visprof_g.pending_scan_ms = 0.0f;
    visprof_g.pending_log_ms = 0.0f;

    visprof_g.self_frame_ns += vp_elapsed_ns(entry_ns, vp_now_ns());
}

void visprof_phase_begin(visprof_phase_t p) {
    if (!visprof_g.inited || p >= VISPROF_MAX_PHASES)
        return;
    visprof_g.open_phase = p;
    visprof_g.phase_start_ns[p] = vp_now_ns();
}

void visprof_phase_end(visprof_phase_t p) {
    if (!visprof_g.inited || p >= VISPROF_MAX_PHASES)
        return;

    float ms = vp_elapsed_ms(visprof_g.phase_start_ns[p], vp_now_ns());
    if (visprof_g.cfg.mode == VISPROF_EXCLUSIVE) {
        ms -= vp_ns_to_ms(visprof_g.self_phase_ns[p]);
        if (ms < 0.0f)
            ms = 0.0f;
    }
    /* A capture leaves the phase in both modes. See visprof_capture.c. It is
     * consumed here: a second begin/end pair for the same phase in one frame
     * overwrites the value, and that second pair did not pay for the capture
     * the first one did. */
    if (visprof_g.capture_phase_ns[p] != 0) {
        ms -= vp_ns_to_ms(visprof_g.capture_phase_ns[p]);
        visprof_g.capture_phase_ns[p] = 0;
        if (ms < 0.0f)
            ms = 0.0f;
    }
    visprof_g.hist[visprof_g.index].phase_ms[p] = ms;

    if (visprof_g.open_phase == p)
        visprof_g.open_phase = VISPROF_MAX_PHASES;
}

static void vp_dump_zones(void) {
    for (uint32_t i = 0; i < visprof_g.zone_count; ++i) {
        if (visprof_g.zone[i].ms >= 1.0f)
            dbglog(DBG_INFO, "  zone %*s%s: %.2f ms\n",
                   (int)visprof_g.zone[i].depth * 2, "", visprof_g.zone[i].name, visprof_g.zone[i].ms);
    }
    if (visprof_g.zone_dropped > 0)
        dbglog(DBG_INFO, "  zone table full: %u dropped\n", (unsigned)visprof_g.zone_dropped);
}

static void vp_spike_log(const vp_sample_t *s) {
    if (visprof_g.frame_count <= visprof_g.hist_len)
        return;

    float total = 0.0f;
    for (uint32_t p = 0; p < visprof_g.phase_count; ++p)
        total += s->phase_ms[p];

    if (s->frame_ms > 25.0f && s->frame_ms < 500.0f &&
        visprof_g.frame_count - visprof_g.last_spike_log_frame >= 60) {
        visprof_g.last_spike_log_frame = visprof_g.frame_count;
        dbglog(DBG_INFO, "visprof: frame spike %.1f ms (phases %.1f) at frame %u\n",
               s->frame_ms, total, (unsigned)visprof_g.frame_count);
        vp_dump_zones();
    } else if (s->frame_ms > 0.5f && total < 0.25f) {

        if (visprof_g.index != visprof_g.last_anomalous_slot) {
            uint32_t bits;
            memcpy(&bits, &s->frame_ms, sizeof(bits));
            visprof_g.last_anomalous_slot = visprof_g.index;
            dbglog(DBG_INFO, "visprof: empty phases slot=%u frame=%08lx (%.2f ms)\n",
                   (unsigned)visprof_g.index, (unsigned long)bits, s->frame_ms);
        }
    }
}

typedef struct {
    kthread_t *self;
    uint64_t   best_delta_ms;
    char       best_label[24];
} vp_scan_ctx_t;

/* Copy at most dst_size - 1 bytes and terminate. The panel's text lines are
 * copied, never parsed, so this replaces a formatting call per line. */
static void vp_copy_bounded(char *dst, size_t dst_size, const char *src) {
    size_t n = 0;
    if (dst_size == 0)
        return;
    while (n + 1 < dst_size && src[n] != '\0')
        ++n;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int vp_scan_thread(kthread_t *thd, void *user) {
    vp_scan_ctx_t *ctx = (vp_scan_ctx_t *)user;
    if (thd == ctx->self || strncmp(thd->label, "[idle]", 6) == 0)
        return 0;

    const uint64_t cpu_ms = thd_get_cpu_time(thd);
    vp_thread_t *slot = NULL;
    vp_thread_t *free_slot = NULL;
    for (uint32_t i = 0; i < VP_MAX_THREADS; ++i) {
        if (visprof_g.thread[i].tid == thd->tid) { slot = &visprof_g.thread[i]; break; }
        if (free_slot == NULL && visprof_g.thread[i].tid < 0) free_slot = &visprof_g.thread[i];
    }
    if (slot == NULL) {

        if (free_slot != NULL) {
            free_slot->tid = thd->tid;
            free_slot->cpu_ms = cpu_ms;
            free_slot->seen_scan = visprof_g.scan_generation;
        }
        return 0;
    }

    slot->seen_scan = visprof_g.scan_generation;
    const uint64_t delta = (cpu_ms > slot->cpu_ms) ? cpu_ms - slot->cpu_ms : 0;
    slot->cpu_ms = cpu_ms;
    if (delta > ctx->best_delta_ms) {
        ctx->best_delta_ms = delta;
        vp_copy_bounded(ctx->best_label, sizeof(ctx->best_label), thd->label);
    }
    return 0;
}

static void vp_scan_threads(void) {
    vp_scan_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.self = thd_get_current();

    const uint64_t now_ns = vp_now_ns();
    const float span_s = (visprof_g.last_scan_wall_ns != 0 && now_ns > visprof_g.last_scan_wall_ns)
        ? (float)(now_ns - visprof_g.last_scan_wall_ns) * 1.0e-9f : 1.0f;
    visprof_g.last_scan_wall_ns = now_ns;

    if (++visprof_g.scan_generation == 0)
        ++visprof_g.scan_generation;
    /* Protect thread pointers and CPU counters from scheduler updates. */
    const irq_mask_t irq_state = irq_disable();
    thd_each(vp_scan_thread, &ctx);
    irq_restore(irq_state);
    for (uint32_t i = 0; i < VP_MAX_THREADS; ++i) {
        if (visprof_g.thread[i].tid >= 0 &&
            visprof_g.thread[i].seen_scan != visprof_g.scan_generation)
            visprof_g.thread[i].tid = -1;
    }

    const float ms_per_s = (float)ctx.best_delta_ms / span_s;
    if (ms_per_s > 0.5f)
        snprintf(visprof_g.thread_line, sizeof(visprof_g.thread_line), "thread CPU %s %.1f ms/s",
                 ctx.best_label, ms_per_s);
    else
        visprof_g.thread_line[0] = '\0';
}

static void vp_capture_top(float frame_ms) {
    visprof_g.top_frame_ms = frame_ms;
    memcpy(visprof_g.top_phase_ms, visprof_g.hist[visprof_g.index].phase_ms,
           sizeof(visprof_g.top_phase_ms));
    visprof_g.top_capture_frame = visprof_g.frame_count;
    visprof_g.top_count = 0;
    visprof_g.top_dropped = visprof_g.zone_dropped;

    for (uint32_t i = 0; i < visprof_g.zone_count; ++i) {
        const vp_zone_t *z = &visprof_g.zone[i];
        if (z->ms < 0.1f)
            continue;

        uint32_t pos = visprof_g.top_count;
        while (pos > 0 && visprof_g.top[pos - 1].ms < z->ms)
            --pos;
        if (pos >= VISPROF_TOP_ZONES)
            continue;

        const uint32_t last = (visprof_g.top_count < VISPROF_TOP_ZONES)
            ? visprof_g.top_count : VISPROF_TOP_ZONES - 1;
        for (uint32_t j = last; j > pos; --j)
            visprof_g.top[j] = visprof_g.top[j - 1];

        visprof_g.top[pos].name = z->name;
        visprof_g.top[pos].ms = z->ms;
        if (visprof_g.top_count < VISPROF_TOP_ZONES)
            ++visprof_g.top_count;
    }
}

static void vp_sample_counters(void) {
    for (uint32_t i = 0; i < visprof_g.counter_count; ++i)
        visprof_g.counter[i].shown = *visprof_g.counter[i].cell;
}

/* Select the upper middle element in place. */
static float vp_median_ms(float *v, uint32_t n) {

    if (n == 0)
        return 0.0f;

    uint32_t lo = 0, hi = n - 1;
    const uint32_t k = n / 2;
    while (lo < hi) {
        const float pivot = v[(lo + hi) / 2];
        uint32_t i = lo;
        uint32_t j = hi;
        for (;;) {
            while (v[i] < pivot) ++i;
            while (v[j] > pivot) --j;
            if (i >= j)
                break;
            const float t = v[i]; v[i] = v[j]; v[j] = t;
            ++i;
            --j;
        }
        if (k <= j)      hi = j;
        else             lo = j + 1;
    }
    return v[k];
}

static vp_text_t *vp_push_text(float x, float y, const char *s) {
    if (visprof_g.text_count >= VP_MAX_TEXTS)
        return NULL;
    vp_text_t *t = &visprof_g.text[visprof_g.text_count++];
    t->x = x;
    t->y = y;
    vp_copy_bounded(t->s, sizeof(t->s), s);
    return t;
}

static float vp_text_width(const char *s) {
    return (float)strlen(s) * visprof_g.char_w;
}

static float vp_unit_drop(void) {
    return visprof_g.text_h * 0.5f + 2.0f;
}

#define VP_SCREEN_MARGIN 16.0f
#define VP_ZONE_GAP       2.0f

static float vp_maxf(float a, float b) {
    return (a > b) ? a : b;
}

static float vp_round(float v) {
    return (float)(int)((v >= 0.0f) ? v + 0.5f : v - 0.5f);
}

#define VP_ZONE_NAME_MAX 28
#define VP_PHASE_NAME_MAX 10

static uint32_t vp_zone_lines(char lines[][VP_LINE_CHARS], uint32_t max) {
    const int dropped = visprof_g.top_dropped > 0;
    const int thread = visprof_g.thread_line[0] != '\0';
    /* Reserve diagnostic rows before adding zone entries. */
    const uint32_t tail = (uint32_t)dropped + (uint32_t)thread;
    const uint32_t body = (max > tail) ? max - tail : 0u;
    uint32_t n = 0;
    if (visprof_g.top_count > 0 && body > 0) {
        snprintf(lines[n++], VP_LINE_CHARS, "captured zones");
        for (uint32_t i = 0; i < visprof_g.top_count && n < body; ++i) {
            const vp_top_t *t = &visprof_g.top[i];
            snprintf(lines[n++], VP_LINE_CHARS, "%.*s %.1f ms",
                     VP_ZONE_NAME_MAX, t->name, t->ms);
        }
    }

    if (dropped && n < max)
        snprintf(lines[n++], VP_LINE_CHARS, "%u more zones dropped", (unsigned)visprof_g.top_dropped);
    if (thread && n < max)
        snprintf(lines[n++], VP_LINE_CHARS, "%s", visprof_g.thread_line);
    return n;
}

static void vp_stats_line(char *stats) {
    const vp_sample_t *worst = NULL;
    float frame_ms[VISPROF_MAX_HISTORY];
    uint32_t frame_count = 0;

    const uint32_t count = (visprof_g.frame_count < visprof_g.hist_len)
        ? visprof_g.frame_count : visprof_g.hist_len - 1u;
    for (uint32_t i = 1; i <= count; ++i) {
        const vp_sample_t *s = &visprof_g.hist[(visprof_g.index + visprof_g.hist_len - i) % visprof_g.hist_len];
        if (worst == NULL || s->frame_ms > worst->frame_ms) worst = s;
        frame_ms[frame_count++] = s->frame_ms;
    }

    const float median = vp_median_ms(frame_ms, frame_count);
    const float worst_ms = (worst != NULL) ? worst->frame_ms : 0.0f;

    snprintf(stats, VP_LINE_CHARS, "med %.1f max %.1f ms", median, worst_ms);

}

static uint32_t vp_counter_lines(char out[][VP_LINE_CHARS], size_t *chars) {
    uint32_t row = 0;
    size_t len = 0;
    size_t slot = 0;
    out[0][0] = '\0';
    *chars = 0;

    for (uint32_t i = 0; i < visprof_g.counter_count + 1; ++i) {
        const char *name;
        char value[16];
        if (i == 0) {
            name = "prof";
            snprintf(value, sizeof(value), "%.2f ms", (double)visprof_g.display_self_ms);
        } else {
            name = visprof_g.counter[i - 1].name;
            snprintf(value, sizeof(value), "%u", (unsigned)visprof_g.counter[i - 1].shown);
        }

        size_t name_len = strlen(name);
        if (name_len > 12)
            name_len = 12;
        /* Reserve ten digits so counter changes cannot move the panel. */
        const size_t width = name_len + 11u + (slot > 0 ? 2u : 0u);
        if (slot > 0 && slot + width > VP_COUNTER_COLS) {
            if (++row >= VP_COUNTER_ROWS)
                return row;
            len = 0;
            slot = 0;
            out[row][0] = '\0';
        }
        if (len < VP_LINE_CHARS - 1) {
            len += (size_t)snprintf(out[row] + len, VP_LINE_CHARS - len, "%s%.*s %s",
                                    (slot > 0) ? "  " : "", (int)name_len, name, value);
            if (len > VP_LINE_CHARS - 1)
                len = VP_LINE_CHARS - 1;
        }
        slot += name_len + 11u + (slot > 0 ? 2u : 0u);
        if (slot > *chars)
            *chars = slot;
    }
    return row + 1;
}

/* The panel shows the MEAN profiler cost over the frames since the last text
 * rebuild. One frame's value moves by about a tenth of a millisecond, and the
 * panel now stands for a quarter of a second, so a single sample would read as
 * a number that jumps for no reason. visprof_self_ms() is unchanged. */
static float vp_take_self_mean(void) {
    const float mean = (visprof_g.self_samples > 0u)
        ? visprof_g.self_sum_ms / (float)visprof_g.self_samples
        : visprof_g.self_ms;
    visprof_g.self_sum_ms = 0.0f;
    visprof_g.self_samples = 0u;
    return mean;
}

/* A rebuild is due on the clock, or at once when an event changed what the
 * text says. A zero period makes every frame due. */
static int vp_text_due(uint64_t now_ns) {
    if (visprof_g.text_forced)
        return 1;
    return (now_ns - visprof_g.last_build_ns) >= visprof_g.text_interval_ns;
}

static float vp_clampf(float v, float lo, float hi) {
    if (v > hi)
        v = hi;
    if (v < lo)
        v = lo;
    return v;
}

static void vp_build_display(void) {
    const float pad = 3.0f;
    const float row = visprof_g.text_h + 1.0f;
    const float cw = visprof_g.char_w;
    const float axis_margin = 2.0f * cw + 2.0f;
    const float swatch = visprof_g.text_h * 0.5f;
    char buf[VP_LINE_CHARS];

    visprof_g.graph_w = (float)visprof_g.hist_len;
    visprof_g.graph_h = visprof_g.cfg.px_per_ms * VP_GRAPH_MS;
    visprof_g.text_count = 0;
    visprof_g.swatch_count = 0;

    char stats[VP_LINE_CHARS];
    vp_stats_line(stats);
    char counters[VP_COUNTER_ROWS][VP_LINE_CHARS];
    size_t counter_chars;
    const uint32_t counter_rows = vp_counter_lines(counters, &counter_chars);

    /* Size from fixed limits so live values do not move the graph. */
    const uint32_t phase_rows = (visprof_g.phase_count + 1u) / 2u;
    const float phase_col_w = swatch + 3.0f + 21.0f * cw;
    float content = axis_margin + visprof_g.graph_w + cw + 2.0f + vp_text_width("60 FPS");
    content = vp_maxf(content, vp_text_width("med 0000.0 max 0000.0 ms"));
    if (visprof_g.phase_count > 0)
        content = vp_maxf(content, phase_col_w * (visprof_g.phase_count > 1 ? 2.0f : 1.0f));
    content = vp_maxf(content, (float)counter_chars * cw);
    visprof_g.panel_w = pad + content + pad;
    visprof_g.panel_h = pad + 2.0f * row + visprof_g.graph_h + vp_unit_drop() + (2u + phase_rows) * row
                      + (float)counter_rows * row + pad;

    const float sw = (vid_mode != NULL) ? (float)vid_mode->width : 640.0f;
    const float sh = (vid_mode != NULL) ? (float)vid_mode->height : 480.0f;
    const float fit_h = sh - 2.0f * VP_SCREEN_MARGIN - visprof_g.panel_h - VP_ZONE_GAP - 2.0f * pad;
    const uint32_t fit = (fit_h > row) ? (uint32_t)(fit_h / row) : 0u;
    uint32_t zone_rows = visprof_g.text_ok ? VISPROF_TOP_ZONES + 3u : 0u;
    if (zone_rows > fit)
        zone_rows = fit;
    if (zone_rows < 2u)
        zone_rows = 0;
    char zone[VISPROF_TOP_ZONES + 3][VP_LINE_CHARS];
    const uint32_t zone_n = vp_zone_lines(zone, zone_rows);
    /* Reserve all possible zone rows for stable middle anchors. */
    const float reserve = (zone_rows > 0)
        ? 2.0f * pad + (float)zone_rows * row + VP_ZONE_GAP : 0.0f;
    const float block_h = reserve + visprof_g.panel_h;

    const uint32_t col = (uint32_t)visprof_g.cfg.anchor % 3u;
    const uint32_t arow = (uint32_t)visprof_g.cfg.anchor / 3u;
    const float bx = (col == 0) ? VP_SCREEN_MARGIN
                   : (col == 1) ? (sw - visprof_g.panel_w) * 0.5f
                   : sw - VP_SCREEN_MARGIN - visprof_g.panel_w;
    const float by = (arow == 0) ? VP_SCREEN_MARGIN
                   : (arow == 1) ? (sh - block_h) * 0.5f
                   : sh - VP_SCREEN_MARGIN - block_h;
    const float block_x = vp_clampf(bx + visprof_g.cfg.x, 0.0f, sw - visprof_g.panel_w);
    const float block_y = vp_clampf(by + visprof_g.cfg.y, 0.0f, sh - block_h);

    const int zones_below = (arow == 0);
    visprof_g.panel_x = vp_round(block_x);
    visprof_g.panel_y = vp_round(zones_below ? block_y : block_y + reserve);

    float zone_w = 0.0f;
    for (uint32_t i = 0; i < zone_n; ++i)
        zone_w = vp_maxf(zone_w, vp_text_width(zone[i]));
    visprof_g.zbox_w_hw = vp_maxf(visprof_g.zbox_w_hw, pad + zone_w + pad);
    visprof_g.zbox_w = visprof_g.zbox_w_hw;
    visprof_g.zbox_h = (zone_n > 0) ? 2.0f * pad + (float)zone_n * row : 0.0f;
    const float zbox_x = (col == 0) ? visprof_g.panel_x
                       : (col == 1) ? visprof_g.panel_x + (visprof_g.panel_w - visprof_g.zbox_w) * 0.5f
                       : visprof_g.panel_x + visprof_g.panel_w - visprof_g.zbox_w;
    visprof_g.zbox_x = vp_round(vp_clampf(zbox_x, 0.0f, sw - visprof_g.zbox_w));
    visprof_g.zbox_y = zones_below
        ? visprof_g.panel_y + visprof_g.panel_h + VP_ZONE_GAP
        : visprof_g.panel_y - VP_ZONE_GAP - visprof_g.zbox_h;
    for (uint32_t i = 0; i < zone_n; ++i)
        vp_push_text(visprof_g.zbox_x + pad, visprof_g.zbox_y + pad + (float)i * row, zone[i]);

    const float left = visprof_g.panel_x + pad;
    float y = visprof_g.panel_y + pad;
    const char *mode = visprof_g.cfg.mode == VISPROF_EXCLUSIVE ? "adjusted" : "raw";
    if (visprof_g.fps > 0.0f)
        snprintf(buf, sizeof(buf), "FPS %.1f %s", visprof_g.fps, mode);
    else
        snprintf(buf, sizeof(buf), "FPS -- %s", mode);
    vp_push_text(left, y, buf);
    y += row;
    vp_push_text(left, y, stats);
    y += row;

    visprof_g.graph_x = left + axis_margin;
    visprof_g.graph_base_y = y + visprof_g.graph_h;
    const float graph_top = visprof_g.graph_base_y - visprof_g.graph_h;

    float label_step = VP_GRID_STEP_MS;
    while (label_step * visprof_g.cfg.px_per_ms < visprof_g.text_h && label_step < VP_GRID_MAX_MS)
        label_step *= 2.0f;
    for (float ms = 0.0f; ms <= VP_GRID_MAX_MS; ms += label_step) {
        snprintf(buf, sizeof(buf), "%d", (int)(ms + 0.5f));
        vp_push_text(visprof_g.graph_x - vp_text_width(buf) - 2.0f,
                     visprof_g.graph_base_y - ms * visprof_g.cfg.px_per_ms - visprof_g.text_h * 0.5f, buf);
    }

    visprof_g.guide_x2 = visprof_g.graph_x + visprof_g.graph_w + visprof_g.char_w;
    {
        const float x = visprof_g.guide_x2 + 2.0f;
        const float gap = 16.6667f * visprof_g.cfg.px_per_ms;
        const float y60 = visprof_g.graph_base_y - gap - visprof_g.text_h * 0.5f;
        float y30 = visprof_g.graph_base_y - 2.0f * gap - visprof_g.text_h * 0.5f;
        if (y30 < graph_top)
            y30 = graph_top;
        vp_push_text(x, visprof_g.graph_base_y - visprof_g.text_h * 0.5f, "ms");
        vp_push_text(x, y60, "60 FPS");
        if (y30 + visprof_g.text_h <= y60)
            vp_push_text(x, y30, "30 FPS");
    }
    y = visprof_g.graph_base_y + vp_unit_drop();

    snprintf(buf, sizeof(buf), "captured frame %.1f ms", visprof_g.top_frame_ms);
    vp_push_text(left, y, buf);
    y += row;
    float other = visprof_g.top_frame_ms;
    for (uint32_t p = 0; p < visprof_g.phase_count; ++p) {
        const float x = left + (p % 2u) * phase_col_w;
        const float py = y + (p / 2u) * row;
        visprof_g.swatch[p].x = x;
        visprof_g.swatch[p].y = py + (visprof_g.text_h - swatch) * 0.5f;
        visprof_g.swatch[p].argb = visprof_g.phase_argb[p];
        ++visprof_g.swatch_count;
        const char *name = visprof_g.phase_name[p];
        if (name == NULL || name[0] == '\0') name = "?";
        snprintf(buf, sizeof(buf), "%.*s %.1f ms", VP_PHASE_NAME_MAX,
                 name, visprof_g.top_phase_ms[p]);
        vp_push_text(x + swatch + 3.0f, py, buf);
        other -= visprof_g.top_phase_ms[p];
    }
    y += phase_rows * row;
    snprintf(buf, sizeof(buf), "other %.1f ms", other > 0.0f ? other : 0.0f);
    vp_push_text(left, y, buf);
    y += row;

    for (uint32_t r = 0; r < counter_rows; ++r) {
        vp_push_text(left, y, counters[r]);
        y += row;
    }
    visprof_g.layout_valid = 1;
}

/* FPS uses complete wall-clock periods, without overhead subtraction. */
static void vp_update_fps(uint64_t now) {
    if (visprof_g.fps_start_ns == 0)
        visprof_g.fps_start_ns = visprof_g.frame_start_ns;
    if (now <= visprof_g.fps_start_ns || now < visprof_g.last_end_ns) {
        visprof_g.fps_start_ns = now;
        visprof_g.fps_frames = 0;
        visprof_g.fps = 0.0f;
        return;
    }
    ++visprof_g.fps_frames;
    const uint64_t elapsed = now - visprof_g.fps_start_ns;
    if (elapsed >= 500000000ull) {
        visprof_g.fps = (float)visprof_g.fps_frames * 1.0e9f / (float)elapsed;
        visprof_g.fps_start_ns = now;
        visprof_g.fps_frames = 0;
    }
}

void visprof_frame_end(void) {
    if (!visprof_g.inited)
        return;

    /* Work after this timestamp belongs to the next frame period. */
    const uint64_t now = vp_now_ns();
    vp_sample_t *s = &visprof_g.hist[visprof_g.index];
    s->frame_ms = vp_elapsed_ms(visprof_g.frame_start_ns, now);

    /* Take the capture out before anything reads this sample: the spike log,
     * the retained-frame capture and the statistics all run below, and a
     * screenshot is not a frame the reader is meant to see in the graph. */
    if (visprof_g.capture_frame_ns != 0) {
        s->frame_ms -= vp_ns_to_ms(visprof_g.capture_frame_ns);
        if (s->frame_ms < 0.0f)
            s->frame_ms = 0.0f;
        visprof_g.capture_frame_ns = 0;
    }

    vp_update_fps(now);
    visprof_g.last_end_ns = now;

    visprof_g.self_ms = (float)visprof_g.self_frame_ns * 1.0e-6f;
    /* Only frames the panel is showing belong in its mean. A hidden panel
     * builds no text, so an unguarded sum would run for as long as the panel
     * stays hidden and be shown as one stale reading when it appears. */
    if (visprof_g.visible) {
        visprof_g.self_sum_ms += visprof_g.self_ms;
        ++visprof_g.self_samples;
    } else {
        visprof_g.self_sum_ms = 0.0f;
        visprof_g.self_samples = 0u;
    }
    if (visprof_g.cfg.mode == VISPROF_EXCLUSIVE) {
        s->frame_ms -= visprof_g.self_ms;
        if (s->frame_ms < 0.0f)
            s->frame_ms = 0.0f;
    }

    if (visprof_g.canary_pre != VP_CANARY || visprof_g.canary_post != VP_CANARY) {

        printf("visprof: history buffer corruption: pre=%08lx post=%08lx at frame %u\n",
               (unsigned long)visprof_g.canary_pre, (unsigned long)visprof_g.canary_post,
               (unsigned)visprof_g.frame_count);
        visprof_g.canary_pre = VP_CANARY;
        visprof_g.canary_post = VP_CANARY;
    }

    if (visprof_g.cfg.spike_log) {
        const uint64_t log_start = vp_now_ns();
        vp_spike_log(s);
        visprof_g.pending_log_ms = vp_elapsed_ms(log_start, vp_now_ns());
    }

    if (s->frame_ms > 18.0f && s->frame_ms < 500.0f && visprof_g.frame_count > visprof_g.hist_len) {
        if (visprof_g.frame_count - visprof_g.last_run_frame > 1)
            visprof_g.scan_armed = 1;
    }

    /* Compare with a retained peak to prevent repeated 10% reductions. */
    const int expired = (visprof_g.frame_count - visprof_g.top_capture_frame) >= visprof_g.hist_len;
    if (s->frame_ms < 500.0f && (expired || s->frame_ms >= 0.9f * visprof_g.top_peak_ms)) {
        const int fresh = (expired || s->frame_ms > visprof_g.top_peak_ms);
        vp_capture_top(s->frame_ms);
        if (fresh) {
            visprof_g.top_peak_ms = s->frame_ms;
            /* A spike, or a reference that aged out: show it at once. A
             * capture that only refreshes inside the ten percent band repeats
             * numbers the reader already sees, and happens on nearly every
             * frame once the frame time is steady, so it waits for the
             * cadence. */
            visprof_g.text_forced = 1;
        }
    }

    if (visprof_g.cfg.thief_scan && visprof_g.scan_armed && visprof_g.frame_count - visprof_g.last_scan_frame >= 60) {
        visprof_g.last_scan_frame = visprof_g.frame_count;
        visprof_g.last_run_frame = visprof_g.frame_count;
        visprof_g.scan_armed = 0;
        const uint64_t scan_start = vp_now_ns();
        vp_scan_threads();
        visprof_g.pending_scan_ms = vp_elapsed_ms(scan_start, vp_now_ns());
        visprof_g.text_forced = 1;  /* The thread CPU line changed. */
    }

    vp_sample_counters();

    visprof_g.index = (visprof_g.index + 1) % visprof_g.hist_len;
    ++visprof_g.frame_count;

    /* Clear the next slot so omitted phases cannot retain old samples. */
    memset(&visprof_g.hist[visprof_g.index], 0, sizeof(visprof_g.hist[visprof_g.index]));

    /* Between rebuilds the text lines, the swatches and the panel geometry
     * stand. visprof_draw() reads the history ring every frame, so the graph
     * keeps moving whatever the cadence is. */
    if (visprof_g.visible && vp_text_due(now)) {
        visprof_g.last_build_ns = now;
        visprof_g.text_forced = 0;
        visprof_g.display_self_ms = vp_take_self_mean();
        vp_build_display();
    }

    visprof_g.carry_self_ns = vp_elapsed_ns(now, vp_now_ns());
}

/* Each of these changes what the panel says, so the next frame end rebuilds
 * the text whatever the refresh clock says. */
void visprof_set_visible(int on) {
    if (on != 0 && !visprof_g.visible)
        visprof_g.text_forced = 1;
    visprof_g.visible = (on != 0);
}
int  visprof_visible(void) { return visprof_g.visible; }
void visprof_set_mode(visprof_mode_t mode) {
    visprof_g.cfg.mode = (mode == VISPROF_EXCLUSIVE) ? mode : VISPROF_INCLUSIVE;
    visprof_g.text_forced = 1;
}
visprof_mode_t visprof_get_mode(void) { return visprof_g.cfg.mode; }
void visprof_set_anchor(visprof_anchor_t anchor) {
    visprof_g.cfg.anchor = ((uint32_t)anchor > VISPROF_BOTTOM_RIGHT) ? VISPROF_TOP_LEFT : anchor;
    visprof_g.text_forced = 1;
}
visprof_anchor_t visprof_get_anchor(void) { return visprof_g.cfg.anchor; }
float visprof_self_ms(void) { return visprof_g.self_ms; }
uint32_t visprof_glyphs_drawn(void) { return visprof_g.glyphs_drawn; }
uint32_t visprof_rects_drawn(void) { return visprof_g.rects_drawn; }

static const vp_sample_t *vp_last_sample(void) {
    return &visprof_g.hist[(visprof_g.index + visprof_g.hist_len - 1) % visprof_g.hist_len];
}

float visprof_last_frame_ms(void) {
    return visprof_g.inited ? vp_last_sample()->frame_ms : 0.0f;
}

float visprof_last_phase_ms(visprof_phase_t p) {
    if (!visprof_g.inited || p >= VISPROF_MAX_PHASES)
        return 0.0f;
    return vp_last_sample()->phase_ms[p];
}

static int vp_panic_line(uint32_t y, const char *fmt, ...) {
    char line[VP_LINE_CHARS];
    const uint32_t width = (vid_mode != NULL) ? vid_mode->width : 0;
    const uint32_t height = (vid_mode != NULL) ? vid_mode->height : 0;
    const int max_chars = (width > 8u) ? (int)((width - 8u) / 12u) : 0;
    if (max_chars <= 0 || y + BFONT_HEIGHT > height)
        return 0;
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    /* BIOS string drawing expands tabs, newlines and multibyte characters. */
    for (size_t i = 0; line[i] != '\0'; ++i) {
        if ((unsigned char)line[i] < 32u || (unsigned char)line[i] > 126u)
            line[i] = '?';
    }
    bfont_draw_str_vram_fmt(8, y, true, "%.*s", max_chars, line);
    return 1;
}

void visprof_panic_dump(void) {
    if (!visprof_g.inited)
        return;

    const vp_sample_t *s = vp_last_sample();
    uint32_t y = 8;
    if (!vp_panic_line(y, "visprof frame %.2f ms", s->frame_ms))
        return;
    y += BFONT_HEIGHT;
    for (uint32_t p = 0; p < visprof_g.phase_count; ++p, y += BFONT_HEIGHT) {
        if (!vp_panic_line(y, "%s %.2f ms", visprof_g.phase_name[p] ? visprof_g.phase_name[p] : "?", s->phase_ms[p]))
            return;
    }
    for (uint32_t i = 0; i < visprof_g.top_count; ++i, y += BFONT_HEIGHT) {
        if (!vp_panic_line(y, "%s %.2f ms", visprof_g.top[i].name ? visprof_g.top[i].name : "?", visprof_g.top[i].ms))
            return;
    }
}

#endif
