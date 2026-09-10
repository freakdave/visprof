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

/* BIOS font atlas and PVR submission. */
#include "visprof_internal.h"

#if VISPROF_ENABLED

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <dc/biosfont.h>
#include <dc/pvr.h>

#define VP_COL_GRID   0xFF404040u
#define VP_COL_FRAME  0xFFFF4444u
#define VP_COL_60FPS  0xFFFFFFFFu
#define VP_COL_30FPS  0xFFFFE000u
#define VP_COL_TEXT   0xFFFFFFFFu

static pvr_ptr_t      vp_atlas;
static pvr_poly_hdr_t vp_hdr_col;
static pvr_poly_hdr_t vp_hdr_txr;

static uint16_t vp_argb1555(uint32_t argb) {
    return (uint16_t)(0x8000u
        | ((argb >> 9) & 0x7C00u)
        | ((argb >> 6) & 0x03E0u)
        | ((argb >> 3) & 0x001Fu));
}

/* Autosort uses depth. Hosts must keep polygons below VP_Z_PANEL. */
static void vp_on_top(pvr_poly_cxt_t *cxt) {
    cxt->blend.src = PVR_BLEND_ONE;
    cxt->blend.dst = PVR_BLEND_ZERO;
    cxt->depth.comparison = PVR_DEPTHCMP_ALWAYS;
}

#define VP_HALF_W (VP_FONT_W / 2)
#define VP_HALF_H (VP_FONT_H / 2)

static int vp_ink(const uint8_t *mask, int x, int y) {
    return x < VP_FONT_W && y < VP_FONT_H && mask[y * VP_FONT_W + x];
}

/* Combine each 2x2 block so single-pixel strokes survive reduction. */
static void vp_shrink(const uint8_t *mask, int dx, int dy, uint8_t *out) {
    for (int y = 0; y < VP_HALF_H; ++y) {
        for (int x = 0; x < VP_HALF_W; ++x) {
            const int sx = 2 * x + dx;
            const int sy = 2 * y + dy;
            out[y * VP_HALF_W + x] = (uint8_t)(vp_ink(mask, sx, sy) || vp_ink(mask, sx + 1, sy) ||
                                               vp_ink(mask, sx, sy + 1) || vp_ink(mask, sx + 1, sy + 1));
        }
    }
}

static int vp_edges(const uint8_t *img) {
    int n = 0;
    for (int y = 0; y < VP_HALF_H; ++y) {
        for (int x = 0; x < VP_HALF_W; ++x) {
            const uint8_t v = img[y * VP_HALF_W + x];
            if (x + 1 < VP_HALF_W && v != img[y * VP_HALF_W + x + 1]) ++n;
            if (y + 1 < VP_HALF_H && v != img[(y + 1) * VP_HALF_W + x]) ++n;
        }
    }
    return n;
}

static void vp_glyph_mask(int ch, uint8_t *mask) {
    uint16_t full[VP_FONT_W * VP_FONT_H];
    bfont_draw_ex(full, VP_FONT_W, 0xFFFFu, 0x0000u, 16, 1, (uint32_t)ch, 0, 0);
    for (int i = 0; i < VP_FONT_W * VP_FONT_H; ++i)
        mask[i] = (uint8_t)(full[i] != 0);
}

/* Choose the grid offset with most edges. An occupied first column
 * prevents an offset that would discard it. */
static int vp_best_dx(const uint8_t *mask, int dy, int *edges) {
    uint8_t half[VP_HALF_W * VP_HALF_H];
    int col0 = 0;
    for (int y = 0; y < VP_FONT_H; ++y)
        col0 |= mask[y * VP_FONT_W];

    int best = 0;
    *edges = -1;
    for (int dx = 0; dx < (col0 ? 1 : 2); ++dx) {
        vp_shrink(mask, dx, dy, half);
        const int e = vp_edges(half);
        if (e > *edges) {
            *edges = e;
            best = dx;
        }
    }
    return best;
}

/* Use one vertical offset for all glyphs to keep their baselines aligned. */
static int vp_font_dy(void) {
    static int cached = -1;
    if (cached >= 0)
        return cached;

    uint8_t mask[VP_FONT_W * VP_FONT_H];
    int total[2] = { 0, 0 };
    int row0 = 0;
    for (int ch = VP_FIRST_CH; ch <= VP_LAST_CH; ++ch) {
        vp_glyph_mask(ch, mask);
        for (int x = 0; x < VP_FONT_W; ++x)
            row0 |= mask[x];
        for (int dy = 0; dy < 2; ++dy) {
            int edges;
            vp_best_dx(mask, dy, &edges);
            total[dy] += edges;
        }
    }
    cached = (!row0 && total[1] > total[0]) ? 1 : 0;
    return cached;
}

static void vp_bake_half(uint16_t *cell, int ch, uint16_t fg, uint16_t bg) {
    uint8_t mask[VP_FONT_W * VP_FONT_H];
    uint8_t half[VP_HALF_W * VP_HALF_H];
    const int dy = vp_font_dy();
    int edges;

    vp_glyph_mask(ch, mask);
    vp_shrink(mask, vp_best_dx(mask, dy, &edges), dy, half);
    for (int y = 0; y < VP_HALF_H; ++y)
        for (int x = 0; x < VP_HALF_W; ++x)
            cell[y * VP_ATLAS_W + x] = half[y * VP_HALF_W + x] ? fg : bg;
}

int visprof_atlas_bake(uint32_t panel_argb, int half) {
    const size_t pixels = (size_t)VP_ATLAS_W * VP_ATLAS_H;
    const size_t bytes = pixels * 2u;
    pvr_poly_cxt_t cxt;

    /* A valid untextured header is required even if atlas creation fails. */
    pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    vp_on_top(&cxt);
    pvr_poly_compile(&vp_hdr_col, &cxt);

    uint16_t *stage = (uint16_t *)malloc(bytes);
    if (stage == NULL)
        return VP_BAKE_NO_RAM;

    /* Opaque glyph cells use the panel color as their background. */
    const uint16_t bg = vp_argb1555(panel_argb);
    const uint16_t fg = 0xFFFFu;
    for (size_t i = 0; i < pixels; ++i)
        stage[i] = bg;

    for (int ch = VP_FIRST_CH; ch <= VP_LAST_CH; ++ch) {
        const int idx = ch - VP_FIRST_CH;
        const size_t px = (size_t)(idx % VP_COLS) * VP_CELL_W;
        const size_t py = (size_t)(idx / VP_COLS) * VP_CELL_H;
        uint16_t *cell = stage + py * VP_ATLAS_W + px;
        if (half)
            vp_bake_half(cell, ch, fg, bg);
        else

            bfont_draw_ex(cell, VP_ATLAS_W, fg, bg, 16, 1, (uint32_t)ch, 0, 0);
    }

    size_t ink = 0;
    for (size_t i = 0; i < pixels; ++i)
        if (stage[i] != bg) ++ink;

    vp_atlas = pvr_mem_malloc(bytes);
    if (vp_atlas == NULL) {
        free(stage);
        return VP_BAKE_NO_VRAM;
    }

    pvr_txr_load(stage, vp_atlas, (uint32_t)bytes);
    free(stage);

    pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY,
                     PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_NONTWIDDLED,
                     VP_ATLAS_W, VP_ATLAS_H, vp_atlas, PVR_FILTER_NONE);
    vp_on_top(&cxt);

    /* Use atlas colors without vertex color modulation. */
    cxt.txr.env = PVR_TXRENV_REPLACE;
    pvr_poly_compile(&vp_hdr_txr, &cxt);

    return (ink > 0) ? 0 : VP_BAKE_NO_INK;
}

void visprof_atlas_free(void) {
    if (vp_atlas != NULL) {
        pvr_mem_free(vp_atlas);
        vp_atlas = NULL;
    }
}

static void vp_submit_hdr(const pvr_poly_hdr_t *src) {
    pvr_poly_hdr_t *dst = (pvr_poly_hdr_t *)pvr_dr_target();
    *dst = *src;
    pvr_dr_commit(dst);
}

static void vp_vertex(float x, float y, float z, float u, float v,
                      uint32_t argb, uint32_t flags) {
    pvr_vertex_t *dst = (pvr_vertex_t *)pvr_dr_target();
    dst->flags = flags;
    dst->x = x;
    dst->y = y;
    dst->z = z;
    dst->u = u;
    dst->v = v;
    dst->argb = argb;
    dst->oargb = 0;
    pvr_dr_commit(dst);
}

/* Both quad paths use BL, TL, BR, TR winding for PVR_CULLING_CCW. */
static void vp_rect(float x1, float y1, float x2, float y2, uint32_t argb, float z) {
    vp_vertex(x1, y2, z, 0.0f, 0.0f, argb, PVR_CMD_VERTEX);
    vp_vertex(x1, y1, z, 0.0f, 0.0f, argb, PVR_CMD_VERTEX);
    vp_vertex(x2, y2, z, 0.0f, 0.0f, argb, PVR_CMD_VERTEX);
    vp_vertex(x2, y1, z, 0.0f, 0.0f, argb, PVR_CMD_VERTEX_EOL);
    ++visprof_g.rects_drawn;
}

static void vp_glyph(int ch, float x, float y, float w, float h) {
    if (ch < VP_FIRST_CH || ch > VP_LAST_CH)
        return;
    const int idx = ch - VP_FIRST_CH;
    const float cx = (float)((idx % VP_COLS) * VP_CELL_W);
    const float cy = (float)((idx / VP_COLS) * VP_CELL_H);
    const float u0 = cx / (float)VP_ATLAS_W;
    const float v0 = cy / (float)VP_ATLAS_H;
    const float u1 = (cx + visprof_g.glyph_w) / (float)VP_ATLAS_W;
    const float v1 = (cy + visprof_g.glyph_h) / (float)VP_ATLAS_H;

    x = (float)(int)(x + 0.5f);
    y = (float)(int)(y + 0.5f);

    vp_vertex(x,     y + h, VP_Z_TEXT, u0, v1, VP_COL_TEXT, PVR_CMD_VERTEX);
    vp_vertex(x,     y,     VP_Z_TEXT, u0, v0, VP_COL_TEXT, PVR_CMD_VERTEX);
    vp_vertex(x + w, y + h, VP_Z_TEXT, u1, v1, VP_COL_TEXT, PVR_CMD_VERTEX);
    vp_vertex(x + w, y,     VP_Z_TEXT, u1, v0, VP_COL_TEXT, PVR_CMD_VERTEX_EOL);
    ++visprof_g.glyphs_drawn;
}

/* Merge adjacent equal heights to reduce entries in PVR tile lists.
 * Cumulative bands overlap in Y and use depth to expose each phase. */
static void vp_draw_bars(void) {
    const uint32_t tracks = visprof_g.phase_count + 1u;
    const int max_h = (int)visprof_g.graph_h;

    int run_value[VISPROF_MAX_PHASES + 1] = { 0 };
    uint32_t run_start[VISPROF_MAX_PHASES + 1] = { 0 };
    int have_run = 0;

    /* Skip the current write slot. Read completed samples oldest first. */
    for (uint32_t i = 1; i < visprof_g.hist_len; ++i) {
        const vp_sample_t *s = &visprof_g.hist[(visprof_g.index + i) % visprof_g.hist_len];

        int value[VISPROF_MAX_PHASES + 1];
        int total = 0;
        for (uint32_t p = 0; p < visprof_g.phase_count; ++p) {
            int h = (int)(s->phase_ms[p] * visprof_g.cfg.px_per_ms + 0.5f);
            if (h < 0) h = 0;
            if (h > max_h - total) h = max_h - total;
            total += h;
            value[p] = total;
        }
        int frame_h = (int)(s->frame_ms * visprof_g.cfg.px_per_ms + 0.5f);
        if (frame_h < 0) frame_h = 0;
        if (frame_h > max_h) frame_h = max_h;
        value[visprof_g.phase_count] = frame_h;

        if (!have_run) {
            for (uint32_t t = 0; t < tracks; ++t) {
                run_value[t] = value[t];
                run_start[t] = i;
            }
            have_run = 1;
            continue;
        }
        for (uint32_t t = 0; t < tracks; ++t) {
            if (value[t] == run_value[t])
                continue;
            if (run_value[t] > 0) {
                const float x1 = visprof_g.graph_x + (float)run_start[t];
                const float x2 = visprof_g.graph_x + (float)i;
                const float y_top = visprof_g.graph_base_y - (float)run_value[t];
                if (t < visprof_g.phase_count)
                    vp_rect(x1, y_top, x2, visprof_g.graph_base_y, visprof_g.phase_argb[t],
                            VP_Z_BAR + VP_Z_BAR_STEP * (float)(visprof_g.phase_count - 1u - t));
                else
                    vp_rect(x1, y_top, x2, y_top + 1.0f, VP_COL_FRAME, VP_Z_LINE);
            }
            run_value[t] = value[t];
            run_start[t] = i;
        }
    }

    if (!have_run)
        return;
    for (uint32_t t = 0; t < tracks; ++t) {
        if (run_value[t] <= 0)
            continue;
        const float x1 = visprof_g.graph_x + (float)run_start[t];
        const float x2 = visprof_g.graph_x + visprof_g.graph_w;
        const float y_top = visprof_g.graph_base_y - (float)run_value[t];
        if (t < visprof_g.phase_count)
            vp_rect(x1, y_top, x2, visprof_g.graph_base_y, visprof_g.phase_argb[t],
                    VP_Z_BAR + VP_Z_BAR_STEP * (float)(visprof_g.phase_count - 1u - t));
        else
            vp_rect(x1, y_top, x2, y_top + 1.0f, VP_COL_FRAME, VP_Z_LINE);
    }
}

void visprof_draw(void) {
    if (!visprof_g.inited || !visprof_g.visible || !visprof_g.layout_valid)
        return;

    const uint64_t entry_ns = vp_now_ns();

    visprof_g.rects_drawn = 0;
    visprof_g.glyphs_drawn = 0;

    vp_submit_hdr(&vp_hdr_col);

    vp_rect(visprof_g.panel_x, visprof_g.panel_y, visprof_g.panel_x + visprof_g.panel_w,
            visprof_g.panel_y + visprof_g.panel_h, visprof_g.cfg.panel_argb, VP_Z_PANEL);
    if (visprof_g.zbox_h > 0.0f)
        vp_rect(visprof_g.zbox_x, visprof_g.zbox_y, visprof_g.zbox_x + visprof_g.zbox_w,
                visprof_g.zbox_y + visprof_g.zbox_h, visprof_g.cfg.panel_argb, VP_Z_PANEL);

    vp_draw_bars();

    const float gx1 = visprof_g.graph_x;
    const float gx2 = visprof_g.graph_x + visprof_g.graph_w;
    for (float ms = 0.0f; ms <= VP_GRID_MAX_MS; ms += VP_GRID_STEP_MS) {
        const float y = visprof_g.graph_base_y - ms * visprof_g.cfg.px_per_ms;
        vp_rect(gx1, y, gx2, y + 1.0f, VP_COL_GRID, VP_Z_GUIDE);
    }

    const float y60 = visprof_g.graph_base_y - 16.6667f * visprof_g.cfg.px_per_ms;
    const float y30 = visprof_g.graph_base_y - 33.3333f * visprof_g.cfg.px_per_ms;

    vp_rect(gx1, y60 - 1.0f, visprof_g.guide_x2, y60 + 1.0f, VP_COL_60FPS, VP_Z_GUIDE);
    vp_rect(gx1, y30 - 1.0f, visprof_g.guide_x2, y30 + 1.0f, VP_COL_30FPS, VP_Z_GUIDE);

    const float swatch = visprof_g.text_h * 0.5f;
    for (uint32_t i = 0; i < visprof_g.swatch_count; ++i)
        vp_rect(visprof_g.swatch[i].x, visprof_g.swatch[i].y,
                visprof_g.swatch[i].x + swatch, visprof_g.swatch[i].y + swatch,
                visprof_g.swatch[i].argb, VP_Z_LINE);

    if (visprof_g.text_ok) {
        vp_submit_hdr(&vp_hdr_txr);
        for (uint32_t i = 0; i < visprof_g.text_count; ++i) {
            float x = visprof_g.text[i].x;
            for (const char *c = visprof_g.text[i].s; *c != '\0'; ++c, x += visprof_g.char_w) {
                if (*c != ' ')
                    vp_glyph((unsigned char)*c, x, visprof_g.text[i].y, visprof_g.char_w, visprof_g.text_h);
            }
        }
    }

    const uint64_t cost_ns = vp_elapsed_ns(entry_ns, vp_now_ns());
    visprof_g.self_frame_ns += cost_ns;
    if (visprof_g.open_phase < VISPROF_MAX_PHASES)
        visprof_g.self_phase_ns[visprof_g.open_phase] += cost_ns;
}

#endif
