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

/* Profiler integration and rendering demo. */
#include <kos.h>
#include <stdio.h>

#include <visprof/visprof.h>

static uint32_t g_quads;

static pvr_poly_hdr_t g_hdr_op;
static pvr_poly_hdr_t g_hdr_pt;
static pvr_poly_hdr_t g_hdr_tr_always;

static void submit_hdr(const pvr_poly_hdr_t *src) {
    pvr_poly_hdr_t *dst = (pvr_poly_hdr_t *)pvr_dr_target();
    *dst = *src;
    pvr_dr_commit(dst);
}

static void vertex(float x, float y, float z, uint32_t argb, uint32_t flags) {
    pvr_vertex_t *v = (pvr_vertex_t *)pvr_dr_target();
    v->flags = flags;
    v->x = x;
    v->y = y;
    v->z = z;
    v->u = 0.0f;
    v->v = 0.0f;
    v->argb = argb;
    v->oargb = 0;
    pvr_dr_commit(v);
}

static void quad(float x, float y, float w, float h, float z, uint32_t argb) {
    vertex(x,     y + h, z, argb, PVR_CMD_VERTEX);
    vertex(x,     y,     z, argb, PVR_CMD_VERTEX);
    vertex(x + w, y + h, z, argb, PVR_CMD_VERTEX);
    vertex(x + w, y,     z, argb, PVR_CMD_VERTEX_EOL);
    ++g_quads;
}

static void draw_scene(int frame) {
    submit_hdr(&g_hdr_op);
    for (int i = 0; i < 48; ++i) {
        const float t = (float)(frame + i * 7) * 0.02f;
        const float x = 320.0f + 220.0f * __builtin_sinf(t);
        const float y = 240.0f + 180.0f * __builtin_cosf(t * 1.3f);
        quad(x, y, 16.0f, 16.0f, 1.0f, 0xFF3060A0u | (uint32_t)(i << 2));
    }
}

static void draw_hostile_op(void) {
    submit_hdr(&g_hdr_op);
    quad(0.0f, 60.0f, 640.0f, 24.0f, 100000.0f, 0xFFFF8000u);
}

static void draw_hostile_pt(void) {
    submit_hdr(&g_hdr_pt);
    quad(0.0f, 110.0f, 640.0f, 20.0f, 100000.0f, 0xFF00C0FFu);
}

static void draw_hostile_tr(void) {
    submit_hdr(&g_hdr_tr_always);
    quad(0.0f, 150.0f, 640.0f, 40.0f, 1.0f, 0xA0FF0000u);
}

/* Artificial work for the demo. Duration depends on the target. */
static void spin(int iterations) {
    volatile float acc = 0.0f;
    for (int i = 0; i < iterations; ++i)
        acc += (float)i * 0.5f;
    (void)acc;
}

static void broadphase(void) {
    VISPROF_SCOPE("broadphase");
    spin(6000);
}

static void narrowphase(int stall) {
    VISPROF_SCOPE("narrowphase");
    spin(9000 + stall);
}

static void physics(int stall) {
    VISPROF_SCOPE("physics");
    broadphase();
    narrowphase(stall);
}

static void ai_update(int frame) {
    VISPROF_SCOPE("ai_update");
    spin(8000 + ((frame % 120) == 0 ? 500000 : 0));
}

static void audio_mix(void) {
    VISPROF_SCOPE("audio_mix");
    spin(4000);
}

/* One test bar in each PVR list checks overlay depth ordering. */
static void compile_headers(void) {
    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    pvr_poly_compile(&g_hdr_op, &cxt);

    pvr_poly_cxt_col(&cxt, PVR_LIST_PT_POLY);
    pvr_poly_compile(&g_hdr_pt, &cxt);

    pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
    pvr_poly_compile(&g_hdr_tr_always, &cxt);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    pvr_init_params_t params = {
        .opb_sizes = { PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16,
                       PVR_BINSIZE_0, PVR_BINSIZE_16 },
        .vertex_buf_size = 512 * 1024,
        .opb_overflow_count = 3,
    };
    pvr_init(&params);

    visprof_config_t cfg = { 0 };
    cfg.anchor = VISPROF_TOP_LEFT;
    cfg.text_scale = 0.5f;
    cfg.spike_log = 1;
    cfg.thief_scan = 1;
    visprof_init(&cfg);

    const visprof_phase_t sim  = visprof_phase_register("Sim", 0);
    const visprof_phase_t wait = visprof_phase_register("Wait", 0);
    const visprof_phase_t draw = visprof_phase_register("Draw", 0);
    visprof_counter_register("demo quads", &g_quads);
    visprof_set_visible(1);

    compile_headers();

    int frame = 0;
    uint32_t held = 0;
    int hostile = 1;

    for (;;) {
        visprof_frame_begin();

        int stall = 0;
        maple_device_t *pad = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
        if (pad != NULL) {
            const cont_state_t *st = (const cont_state_t *)maple_dev_status(pad);
            if (st != NULL) {
                const uint32_t pressed = st->buttons & ~held;
                held = st->buttons;
                if (st->buttons & CONT_START)
                    break;
                if (st->buttons & CONT_X)
                    stall = 200000;
                if (pressed & CONT_A)
                    visprof_set_visible(!visprof_visible());
                if (pressed & CONT_B)
                    visprof_set_mode(visprof_get_mode() == VISPROF_EXCLUSIVE
                                     ? VISPROF_INCLUSIVE : VISPROF_EXCLUSIVE);
                if (pressed & CONT_Y)
                    hostile = !hostile;
                if (pressed & CONT_DPAD_RIGHT)
                    visprof_set_anchor((visprof_anchor_t)((visprof_get_anchor() + 1) % 9));
            }
        }

        visprof_phase_begin(sim);
        physics(stall);
        ai_update(frame);
        audio_mix();
        visprof_phase_end(sim);

        visprof_phase_begin(wait);
        pvr_wait_ready();
        visprof_phase_end(wait);

        visprof_phase_begin(draw);
        pvr_scene_begin();
        g_quads = 0;

        pvr_list_begin(PVR_LIST_OP_POLY);
        draw_scene(frame);
        if (hostile)
            draw_hostile_op();
        pvr_list_finish();

        pvr_list_begin(PVR_LIST_PT_POLY);
        if (hostile)
            draw_hostile_pt();
        pvr_list_finish();

        pvr_list_begin(PVR_LIST_TR_POLY);
        visprof_draw();
        if (hostile)
            draw_hostile_tr();
        pvr_list_finish();

        pvr_scene_finish();
        visprof_phase_end(draw);

        visprof_frame_end();

        if ((++frame % 120) == 60)
            printf("visprof_demo: glyphs=%u rects=%u self=%.2f mode=%s frame=%.2f\n",
                   (unsigned)visprof_glyphs_drawn(), (unsigned)visprof_rects_drawn(),
                   (double)visprof_self_ms(),
                   visprof_get_mode() == VISPROF_EXCLUSIVE ? "adjusted" : "raw",
                   (double)visprof_last_frame_ms());
    }

    pvr_wait_ready();
    pvr_wait_render_done();
    visprof_shutdown();
    printf("visprof_demo: exit frames=%d\n", frame);
    return 0;
}
