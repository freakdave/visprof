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

/* Span sink tests. These run the REAL src/visprof_draw.c on the host with a
 * stub PVR, so both submission paths are exercised by the same drawing code. */
#include "visprof_internal.h"
#include <dc/biosfont.h>
#include <dc/video.h>
#include <kos/thread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(expr) do { \
    ++checks; \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
        ++failures; \
    } \
} while (0)

/* ── KOS surface ─────────────────────────────────────────────────────────── */

static vp_vid_mode_t screen = { 640, 480 };
vp_vid_mode_t *vid_mode = &screen;
unsigned test_irq_mask;

static uint64_t now_ns;
static kthread_t main_thread = { 1, "main" };

uint64_t timer_ns_gettime64(void) { return now_ns; }
kthread_t *thd_get_current(void) { return &main_thread; }
uint64_t thd_get_cpu_time(kthread_t *thread) { (void)thread; return 0; }
int thd_each(int (*cb)(kthread_t *, void *), void *data) { return cb(&main_thread, data); }
int dbglog(int level, const char *fmt, ...) { (void)level; (void)fmt; return 0; }
void bfont_draw_str_vram_fmt(uint32_t x, uint32_t y, bool opaque, const char *fmt, ...) {
    (void)x; (void)y; (void)opaque; (void)fmt;
}

/* A framed glyph with interior speckle. Any pattern works; the bake only needs
 * ink so that text is enabled and glyph quads are submitted. */
void bfont_draw_ex(void *buffer, uint32_t stride, uint16_t fg, uint16_t bg,
                   int bpp, int opaque, uint32_t ch, int y0, int x0) {
    uint16_t *out = (uint16_t *)buffer;
    (void)bpp; (void)opaque; (void)y0; (void)x0;
    for (int y = 0; y < 24; ++y) {
        for (int x = 0; x < 12; ++x) {
            const int frame = (x == 0 || x == 11 || y == 0 || y == 23);
            const int speck = (((x + y + (int)ch) % 5) == 0);
            out[(size_t)y * stride + (size_t)x] = (frame || speck) ? fg : bg;
        }
    }
}

/* ── PVR stub with record counting ───────────────────────────────────────── */

#define VP_TEST_HDR_MARK 0xABCD0000u

static uint32_t dr_headers;
static uint32_t dr_vertices;
static uint32_t dr_bad_records;
static uint64_t pvr_scratch[4];   /* 32 bytes, 8-byte aligned */

void pvr_poly_cxt_col(pvr_poly_cxt_t *cxt, int list) {
    memset(cxt, 0, sizeof(*cxt));
    cxt->list_type = list;
}
void pvr_poly_cxt_txr(pvr_poly_cxt_t *cxt, int list, int format,
                      int width, int height, pvr_ptr_t base, int filter) {
    memset(cxt, 0, sizeof(*cxt));
    cxt->list_type = list;
    cxt->txr.format = format;
    cxt->txr.width = width;
    cxt->txr.height = height;
    cxt->txr.base = base;
    cxt->txr.filter = filter;
}
void pvr_poly_compile(pvr_poly_hdr_t *hdr, pvr_poly_cxt_t *cxt) {
    memset(hdr, 0, sizeof(*hdr));
    /* A marker the commit counter can tell apart from a vertex command. */
    hdr->words[0] = VP_TEST_HDR_MARK | (uint32_t)cxt->list_type;
}
pvr_ptr_t pvr_mem_malloc(size_t bytes) { return malloc(bytes); }
void pvr_mem_free(pvr_ptr_t block) { free(block); }
void pvr_txr_load(const void *src, pvr_ptr_t dst, uint32_t count) {
    memcpy(dst, src, count);
}
void *pvr_dr_target(void) { return pvr_scratch; }
void pvr_dr_commit(void *record) {
    const uint32_t first = *(const uint32_t *)record;
    if ((first & 0xFFFF0000u) == VP_TEST_HDR_MARK)
        ++dr_headers;
    else if (first == PVR_CMD_VERTEX || first == PVR_CMD_VERTEX_EOL)
        ++dr_vertices;
    else
        ++dr_bad_records;
}

/* ── Small hashes, so a whole panel can be compared in one check ─────────── */

static uint32_t hash_bytes(uint32_t seed, const void *data, size_t bytes) {
    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < bytes; ++i)
        seed = (seed ^ p[i]) * 16777619u;
    return seed;
}

static uint32_t text_hash(void) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < visprof_g.text_count; ++i)
        h = hash_bytes(h, visprof_g.text[i].s, strlen(visprof_g.text[i].s));
    return h;
}

/* ── Span sink under test ────────────────────────────────────────────────── */

/* The arena wraps: every span is checked at its own commit, so no history is
 * kept. 32-byte aligned, as the contract requires of a reserved span. */
#define VP_ARENA_BYTES 8192u
static uint8_t arena[VP_ARENA_BYTES] __attribute__((aligned(32)));
static uint32_t arena_used;

static uint32_t sink_requests;   /* reserve calls, dropped ones included */
static uint32_t sink_spans;      /* reserve calls that returned a span */
static uint32_t sink_bytes;      /* bytes of those spans */
static uint32_t sink_commits;    /* commit calls */
static uint32_t sink_headers;    /* 32-byte spans committed */
static uint32_t sink_vertices;   /* vertex records inside committed quads */
static uint32_t sink_drops;      /* reserve calls that returned NULL */
static uint32_t sink_dropped_bytes;

static uint32_t sink_geom_hash;  /* over every committed vertex position */

static int sink_open;            /* a span is reserved and not yet committed */
static void *sink_last_span;
static uint32_t sink_last_bytes;
static void *sink_user_seen;
static int user_token;

/* Refuse the request with this index. -1 refuses nothing. */
static int32_t sink_drop_at = -1;

static void *sink_reserve(uint32_t bytes, void *user) {
    CHECK(user == sink_user_seen);
    /* The sink must never be re-entered between a reserve and its commit. */
    CHECK(sink_open == 0);
    CHECK(bytes != 0u && (bytes % 32u) == 0u
          && bytes <= (uint32_t)VISPROF_SINK_MAX_SPAN);

    const int32_t request = (int32_t)sink_requests++;
    if (request == sink_drop_at) {
        ++sink_drops;
        sink_dropped_bytes = bytes;
        return NULL;
    }

    if (arena_used + bytes > VP_ARENA_BYTES)
        arena_used = 0u;
    void *span = &arena[arena_used];
    arena_used += bytes;

    sink_open = 1;
    sink_last_span = span;
    sink_last_bytes = bytes;
    ++sink_spans;
    sink_bytes += bytes;
    return span;
}

static void sink_commit(void *span, uint32_t bytes, void *user) {
    CHECK(user == sink_user_seen);
    CHECK(sink_open == 1);
    CHECK(span == sink_last_span);
    CHECK(bytes == sink_last_bytes);
    CHECK(((uintptr_t)span & 31u) == 0u);
    sink_open = 0;
    ++sink_commits;

    if (bytes == 32u) {
        /* A 32-byte span is one polygon header. */
        CHECK((*(const uint32_t *)span & 0xFFFF0000u) == VP_TEST_HDR_MARK);
        ++sink_headers;
        return;
    }
    CHECK(bytes == (uint32_t)VISPROF_SINK_MAX_SPAN);
    if (bytes != (uint32_t)VISPROF_SINK_MAX_SPAN)
        return;

    /* A 128-byte span is one quad: four vertices, the fourth closing it. */
    const pvr_vertex_t *v = (const pvr_vertex_t *)span;
    for (uint32_t i = 0; i < 4u; ++i) {
        CHECK(v[i].flags == ((i == 3u) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX));
        CHECK(v[i].oargb == 0u);
        sink_geom_hash = hash_bytes(sink_geom_hash, &v[i].x, 3u * sizeof(float));
    }
    sink_vertices += 4u;
}

static void reset_counts(void) {
    dr_headers = 0;
    dr_vertices = 0;
    dr_bad_records = 0;
    sink_requests = 0;
    sink_spans = 0;
    sink_bytes = 0;
    sink_commits = 0;
    sink_headers = 0;
    sink_vertices = 0;
    sink_drops = 0;
    sink_dropped_bytes = 0;
    sink_open = 0;
    sink_geom_hash = 2166136261u;
    arena_used = 0u;
}

/* What must hold after any draw through the sink. Every span was already
 * checked field by field inside sink_reserve and sink_commit. */
static void check_sink_health(void) {
    CHECK(sink_open == 0);
    CHECK(sink_commits == sink_spans);
}

static void start(const visprof_config_t *cfg) {
    visprof_shutdown();
    now_ns = 1000000000;
    test_irq_mask = 0;
    CHECK(visprof_init(cfg) == 0);
    visprof_phase_register("Update", 0);
    visprof_phase_register("Render", 0);
    visprof_set_visible(1);
    /* Two frames: the first fills a history slot, the second lays the panel
     * out. Layout runs at frame end while the overlay is visible. */
    for (int i = 0; i < 2; ++i) {
        visprof_frame_begin();
        now_ns += 5000000;
        visprof_frame_end();
    }
    CHECK(visprof_g.layout_valid == 1);
    CHECK(visprof_g.text_ok == 1);
}

static void configure_sink(visprof_config_t *cfg, void *user) {
    cfg->reserve = sink_reserve;
    cfg->commit = sink_commit;
    cfg->user = user;
    sink_user_seen = user;
}

/* ── Cases ───────────────────────────────────────────────────────────────── */

/* With no sink configured every record goes to the store queues, exactly as
 * before the sink existed. */
static void direct_path(void) {
    visprof_config_t cfg = { 0 };
    cfg.history_len = 8;
    start(&cfg);
    reset_counts();
    visprof_draw();

    CHECK(sink_requests == 0);
    CHECK(sink_spans == 0);
    CHECK(sink_commits == 0);
    CHECK(dr_bad_records == 0);
    CHECK(dr_headers == 2);   /* untextured panel header + text header */
    CHECK(dr_vertices == 4 * (visprof_rects_drawn() + visprof_glyphs_drawn()));
    CHECK(dr_vertices > 0);
}

/* With a sink configured no store queue is touched, every span carries the
 * records the direct path would have written, and the sizes obey the
 * contract. */
static void span_path(void) {
    visprof_config_t cfg = { 0 };
    cfg.history_len = 8;
    configure_sink(&cfg, &user_token);
    start(&cfg);
    reset_counts();
    visprof_draw();

    const uint32_t quads = visprof_rects_drawn() + visprof_glyphs_drawn();

    CHECK(dr_headers == 0);
    CHECK(dr_vertices == 0);
    CHECK(dr_bad_records == 0);
    check_sink_health();
    CHECK(sink_drops == 0);
    CHECK(sink_headers == 2);
    CHECK(quads > 0);
    CHECK(sink_spans == 2 + quads);
    CHECK(sink_bytes == 2 * 32u + 128u * quads);
    CHECK(sink_vertices == 4 * quads);

    printf("draw sink: %u spans, %u bytes, %u records per draw\n",
           (unsigned)sink_spans, (unsigned)sink_bytes,
           (unsigned)(sink_headers + sink_vertices));
}

/* Same panel, same record count, whichever path carries it. */
static void paths_agree(void) {
    visprof_config_t cfg = { 0 };
    cfg.history_len = 8;
    start(&cfg);
    reset_counts();
    visprof_draw();
    const uint32_t direct_records = dr_headers + dr_vertices;

    configure_sink(&cfg, NULL);
    start(&cfg);
    reset_counts();
    visprof_draw();

    check_sink_health();
    CHECK(sink_headers + sink_vertices == direct_records);
}

/* A refused span costs exactly one quad: no commit for it, no partial strip,
 * and every later span is still offered. */
static void refused_span_drops_one_quad(void) {
    visprof_config_t cfg = { 0 };
    cfg.history_len = 8;
    configure_sink(&cfg, NULL);
    start(&cfg);

    sink_drop_at = -1;
    reset_counts();
    visprof_draw();
    const uint32_t all_requests = sink_requests;
    const uint32_t all_spans = sink_spans;
    const uint32_t all_vertices = sink_vertices;
    const uint32_t all_quads = visprof_rects_drawn() + visprof_glyphs_drawn();

    /* Request 0 is the untextured header, so request 1 is the panel quad. */
    sink_drop_at = 1;
    reset_counts();
    visprof_draw();
    sink_drop_at = -1;

    check_sink_health();
    CHECK(sink_drops == 1);
    CHECK(sink_dropped_bytes == (uint32_t)VISPROF_SINK_MAX_SPAN);
    CHECK(sink_requests == all_requests);          /* the draw continued */
    CHECK(sink_spans == all_spans - 1);
    CHECK(sink_commits == all_spans - 1);          /* no commit for the drop */
    CHECK(sink_vertices == all_vertices - 4);
    CHECK(visprof_rects_drawn() + visprof_glyphs_drawn() == all_quads - 1);
    CHECK(dr_headers == 0);                        /* nothing leaked to the SQ */
    CHECK(dr_vertices == 0);
}

/* Half a sink would interleave two submission paths in one list. */
static void incomplete_sink_falls_back(void) {
    visprof_config_t cfg = { 0 };
    cfg.history_len = 8;
    cfg.reserve = sink_reserve;
    cfg.commit = NULL;
    sink_user_seen = NULL;
    start(&cfg);
    CHECK(visprof_g.cfg.reserve == NULL);
    CHECK(visprof_g.cfg.commit == NULL);
    reset_counts();
    visprof_draw();
    CHECK(sink_requests == 0);
    CHECK(sink_commits == 0);
    CHECK(dr_vertices > 0);
}

/* A hidden overlay submits nothing through either path. */
static void hidden_draws_nothing(void) {
    visprof_config_t cfg = { 0 };
    cfg.history_len = 8;
    configure_sink(&cfg, NULL);
    start(&cfg);
    visprof_set_visible(0);
    reset_counts();
    visprof_draw();
    CHECK(sink_requests == 0);
    CHECK(sink_commits == 0);
    CHECK(dr_headers == 0);
    CHECK(dr_vertices == 0);
}

/* The text is rebuilt a few times a second, but the graph is read from the
 * history ring at every draw, so the bars must move while the text stands. */
static void bars_move_while_text_stands(void) {
    visprof_config_t cfg = { 0 };
    cfg.history_len = 32;
    cfg.text_refresh_hz = 4;      /* 250 ms between text rebuilds */
    configure_sink(&cfg, NULL);
    start(&cfg);

    reset_counts();
    visprof_draw();
    const uint32_t geometry = sink_geom_hash;
    const uint32_t text = text_hash();
    const uint32_t lines = visprof_g.text_count;

    /* One more frame of the same length: 5 ms, far inside the period, and not
     * a new peak, so nothing rebuilds the text. The history ring advanced. */
    visprof_frame_begin();
    now_ns += 5000000;
    visprof_frame_end();
    reset_counts();
    visprof_draw();

    CHECK(visprof_g.text_count == lines);
    CHECK(text_hash() == text);
    CHECK(sink_geom_hash != geometry);
    check_sink_health();
}

/* The table replaces four divisions per glyph. It must hold exactly what
 * those divisions produced, at both text sizes. */
static void uv_table_matches_divides(void) {
    for (int half = 0; half < 2; ++half) {
        visprof_config_t cfg = { 0 };
        cfg.history_len = 8;
        cfg.text_scale = half ? 0.5f : 1.0f;
        start(&cfg);
        CHECK(visprof_g.glyph_w == (half ? 6.0f : 12.0f));
        for (int ch = VP_FIRST_CH; ch <= VP_LAST_CH; ++ch) {
            const int idx = ch - VP_FIRST_CH;
            const float cx = (float)((idx % VP_COLS) * VP_CELL_W);
            const float cy = (float)((idx / VP_COLS) * VP_CELL_H);
            CHECK(visprof_uv[idx].u0 == cx / (float)VP_ATLAS_W);
            CHECK(visprof_uv[idx].v0 == cy / (float)VP_ATLAS_H);
            CHECK(visprof_uv[idx].u1 == (cx + visprof_g.glyph_w) / (float)VP_ATLAS_W);
            CHECK(visprof_uv[idx].v1 == (cy + visprof_g.glyph_h) / (float)VP_ATLAS_H);
        }
    }
}

int main(void) {
    direct_path();
    span_path();
    paths_agree();
    refused_span_drops_one_quad();
    incomplete_sink_falls_back();
    hidden_draws_nothing();
    bars_move_while_text_stands();
    uv_table_matches_divides();
    visprof_shutdown();

    printf("draw sink: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
