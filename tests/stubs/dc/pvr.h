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
#ifndef VP_STUB_PVR_H
#define VP_STUB_PVR_H
#include <stddef.h>
#include <stdint.h>

/* Enough of the KOS PVR interface for the host tests. The layouts match the
 * hardware record sizes the library relies on: 32 bytes each. The test file
 * implements every function declared here. */

typedef void *pvr_ptr_t;
typedef struct { uint32_t words[8]; } pvr_poly_hdr_t;

typedef struct {
    uint32_t flags;
    float    x, y, z;
    float    u, v;
    uint32_t argb;
    uint32_t oargb;
} pvr_vertex_t;

typedef struct {
    int list_type;
    struct { int src, dst; } blend;
    struct { int comparison; } depth;
    struct { int env, format, width, height, filter; pvr_ptr_t base; } txr;
} pvr_poly_cxt_t;

#define PVR_LIST_TR_POLY        2

#define PVR_CMD_VERTEX          0xE0000000u
#define PVR_CMD_VERTEX_EOL      0xF0000000u

#define PVR_BLEND_ONE           1
#define PVR_BLEND_ZERO          0
#define PVR_DEPTHCMP_ALWAYS     7
#define PVR_TXRFMT_ARGB1555     0x02000000
#define PVR_TXRFMT_NONTWIDDLED  0x04000000
#define PVR_FILTER_NONE         0
#define PVR_TXRENV_REPLACE      0

void  pvr_poly_cxt_col(pvr_poly_cxt_t *cxt, int list);
void  pvr_poly_cxt_txr(pvr_poly_cxt_t *cxt, int list, int format,
                       int width, int height, pvr_ptr_t base, int filter);
void  pvr_poly_compile(pvr_poly_hdr_t *hdr, pvr_poly_cxt_t *cxt);

pvr_ptr_t pvr_mem_malloc(size_t bytes);
void      pvr_mem_free(pvr_ptr_t block);
void      pvr_txr_load(const void *src, pvr_ptr_t dst, uint32_t count);

/* Direct rendering. The test counts every committed record. */
void *pvr_dr_target(void);
void  pvr_dr_commit(void *record);

/* Display registers and video memory, for the capture tests.
 *
 * On the console PVR_GET reads 0xa05f8000 + offset and PVR_RAM_BASE is
 * 0xa5000000, whose low 24 bits are zero. Here the registers are an array
 * indexed by the same offset and video memory is an ordinary array, so the
 * library's address arithmetic (base + masked offset) runs unchanged. The
 * test file defines both arrays and drives them. */
#define PVR_FB_CFG_1  0x0044
#define PVR_FB_ADDR   0x0050

#define VP_TEST_REG_WORDS   64
#define VP_TEST_VRAM_BYTES  (128 * 1024)

extern uint32_t vp_test_reg[VP_TEST_REG_WORDS];
extern uint8_t  vp_test_vram[VP_TEST_VRAM_BYTES];

#define PVR_GET(REG)        (vp_test_reg[(REG) >> 2])
#define PVR_SET(REG, VALUE) (PVR_GET(REG) = (VALUE))
#define PVR_RAM_BASE        ((uintptr_t)vp_test_vram)

#endif
