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

/* Screen capture: the displayed framebuffer as a binary PPM.
 *
 * WHICH buffer. The PVR shows one of two framebuffers and swaps them when a
 * render finishes. KOS keeps a pointer to the current one (`vram_l`), but it
 * writes the display register first and that pointer second, and any caller of
 * vid_set_vram() can move the pointer without moving the display. The register
 * is the hardware's own answer to "what is on the screen", so the start
 * address comes from PVR_FB_ADDR and the pixel format from PVR_FB_CFG_1.
 *
 * WHY it is safe to read for a long time. The view swaps only inside the
 * vertical blank handler, and only when a render has completed
 * (KOS kernel/arch/dreamcast/hardware/pvr/pvr_irq.c:120-131). This function
 * blocks the frame thread, so no new scene is submitted while it runs: at most
 * the render already in flight completes and swaps once. Nothing writes into
 * the buffer being read, so the picture is one whole frame.
 *
 * WHY it does not use KOS's vid_screen_shot(). That function allocates
 * width*height*3 bytes and holds interrupts off for the whole conversion
 * (kernel/arch/dreamcast/util/screenshot.c:39-49 and :54-118). This library
 * allocates nothing and must not stop the vertical blank, the sound stream or
 * the dc-load network for a fifth of a second.
 */
#include "visprof_internal.h"

#if !defined(VISPROF_ENABLED)
#  error "visprof/visprof.h did not define VISPROF_ENABLED - include path is wrong"
#endif

#if VISPROF_ENABLED

#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#include <dc/pvr.h>
#include <dc/video.h>
#include <kos/fs.h>

/* Register fields. PVR_FB_CFG_1 bit 0 enables the display and bits 3:2 hold
 * the pixel mode that vid_set_mode_ex() wrote (KOS hardware/video.c:349). */
#define VP_FB_ENABLE_BIT  0x00000001u
#define VP_FB_PM_SHIFT    2
#define VP_FB_PM_MASK     0x00000003u

#define VP_PM_RGB555      0u   /* 16 bits per pixel, one unused. */
#define VP_PM_RGB565      1u   /* 16 bits per pixel. */
#define VP_PM_RGB888P     2u   /* 24 bits per pixel, packed. */
#define VP_PM_RGB0888     3u   /* 32 bits per pixel, one byte unused. */

/* Video memory offsets never exceed 16 MiB, and the low 24 bits of
 * PVR_RAM_BASE are zero, so an addition reproduces what KOS does with an OR
 * (hardware/video.c:445-446) while staying valid arithmetic on a host test
 * where the base is an ordinary array. */
#define VP_VRAM_OFFSET_MASK 0x00FFFFFFu

static uint32_t vp_bytes_per_pixel(uint32_t pixel_mode) {
    switch (pixel_mode) {
        case VP_PM_RGB555:  return 2u;
        case VP_PM_RGB565:  return 2u;
        case VP_PM_RGB888P: return 3u;
        default:            return 4u;
    }
}

/* Expand one 16-bit pixel. The channel shifts match KOS's own screenshot so
 * that both produce the same picture from the same memory. */
static void vp_put_16(uint8_t *dst, uint32_t pixel, uint32_t pixel_mode) {
    if (pixel_mode == VP_PM_RGB565) {
        dst[0] = (uint8_t)(((pixel >> 11) & 0x1Fu) << 3);
        dst[1] = (uint8_t)(((pixel >> 5) & 0x3Fu) << 2);
        dst[2] = (uint8_t)(((pixel >> 0) & 0x1Fu) << 3);
    } else {
        dst[0] = (uint8_t)(((pixel >> 10) & 0x1Fu) << 3);
        dst[1] = (uint8_t)(((pixel >> 5) & 0x1Fu) << 3);
        dst[2] = (uint8_t)(((pixel >> 0) & 0x1Fu) << 3);
    }
}

/* One row of video memory into one row of red, green and blue bytes.
 * `address` is the integer address of the row; keeping it an integer avoids a
 * byte pointer cast that -Wcast-align rejects. Sixteen and thirty-two bit
 * modes are read 32 bits at a time: video memory is uncached, so half as many
 * reads is half the bus traffic. */
static void vp_convert_row(uintptr_t address, uint8_t *dst, uint32_t width,
                           uint32_t pixel_mode) {
    uint32_t x = 0;

    if (pixel_mode == VP_PM_RGB888P) {
        const volatile uint8_t *src = (const volatile uint8_t *)address;
        for (; x < width; ++x) {
            dst[x * 3 + 0] = src[x * 3 + 2];
            dst[x * 3 + 1] = src[x * 3 + 1];
            dst[x * 3 + 2] = src[x * 3 + 0];
        }
        return;
    }

    if (pixel_mode == VP_PM_RGB0888) {
        const volatile uint32_t *src = (const volatile uint32_t *)address;
        for (; x < width; ++x) {
            const uint32_t pixel = src[x];
            dst[x * 3 + 0] = (uint8_t)((pixel >> 16) & 0xFFu);
            dst[x * 3 + 1] = (uint8_t)((pixel >> 8) & 0xFFu);
            dst[x * 3 + 2] = (uint8_t)((pixel >> 0) & 0xFFu);
        }
        return;
    }

    {
        const volatile uint32_t *src = (const volatile uint32_t *)address;
        const uint32_t pairs = width / 2u;
        for (uint32_t i = 0; i < pairs; ++i) {
            const uint32_t two = src[i];
            vp_put_16(dst + i * 6 + 0, two & 0xFFFFu, pixel_mode);
            vp_put_16(dst + i * 6 + 3, two >> 16, pixel_mode);
        }
        if ((width & 1u) != 0u) {
            const volatile uint16_t *tail = (const volatile uint16_t *)address;
            vp_put_16(dst + (width - 1u) * 3, tail[width - 1u], pixel_mode);
        }
    }
}

/* Adds an elapsed capture to the frame period and, when one is open, to the
 * phase. visprof_frame_end() and visprof_phase_end() subtract these in BOTH
 * timing modes: the profiler's own overhead belongs in a `raw` reading, a
 * screenshot the user asked for does not. */
static void vp_charge_capture(uint64_t ns) {
    visprof_g.capture_frame_ns += ns;
    if (visprof_g.open_phase < VISPROF_MAX_PHASES)
        visprof_g.capture_phase_ns[visprof_g.open_phase] += ns;
}

int visprof_capture_now(const char *path) {
    uint8_t row[VISPROF_CAPTURE_MAX_WIDTH * 3];
    char header[32];
    const uint64_t start_ns = vp_now_ns();

    if (!visprof_g.inited || path == NULL)
        return VISPROF_CAPTURE_EARG;

    const uint32_t width = (vid_mode != NULL) ? (uint32_t)vid_mode->width : 0u;
    const uint32_t height = (vid_mode != NULL) ? (uint32_t)vid_mode->height : 0u;
    if (width == 0u || height == 0u || width > VISPROF_CAPTURE_MAX_WIDTH) {
        printf("visprof: capture %s failed (%d)\n", path, VISPROF_CAPTURE_EMODE);
        return VISPROF_CAPTURE_EMODE;
    }

    const uint32_t config = PVR_GET(PVR_FB_CFG_1);
    if ((config & VP_FB_ENABLE_BIT) == 0u) {
        printf("visprof: capture %s failed (%d)\n", path, VISPROF_CAPTURE_EFORMAT);
        return VISPROF_CAPTURE_EFORMAT;
    }

    const uint32_t pixel_mode = (config >> VP_FB_PM_SHIFT) & VP_FB_PM_MASK;
    const uint32_t stride = width * vp_bytes_per_pixel(pixel_mode);
    const uintptr_t base = (uintptr_t)PVR_RAM_BASE +
                           (PVR_GET(PVR_FB_ADDR) & VP_VRAM_OFFSET_MASK);

    const file_t out = fs_open(path, O_WRONLY | O_TRUNC);
    if (out == FILEHND_INVALID) {
        printf("visprof: capture %s failed (%d)\n", path, VISPROF_CAPTURE_EOPEN);
        vp_charge_capture(vp_elapsed_ns(start_ns, vp_now_ns()));
        return VISPROF_CAPTURE_EOPEN;
    }

    const int header_len = snprintf(header, sizeof(header), "P6\n%u %u\n255\n",
                                    (unsigned)width, (unsigned)height);
    int status = 0;
    if (fs_write(out, header, (size_t)header_len) != (ssize_t)header_len)
        status = VISPROF_CAPTURE_EWRITE;

    const size_t row_bytes = (size_t)width * 3u;
    for (uint32_t y = 0; status == 0 && y < height; ++y) {
        vp_convert_row(base + (uintptr_t)y * stride, row, width, pixel_mode);
        if (fs_write(out, row, row_bytes) != (ssize_t)row_bytes)
            status = VISPROF_CAPTURE_EWRITE;
    }

    fs_close(out);

    const uint64_t spent_ns = vp_elapsed_ns(start_ns, vp_now_ns());
    vp_charge_capture(spent_ns);
    visprof_g.capture_ms = vp_ns_to_ms(spent_ns);

    if (status != 0) {
        printf("visprof: capture %s failed (%d)\n", path, status);
        return status;
    }
    printf("visprof: capture %s %ux%u %.1f ms\n", path, (unsigned)width,
           (unsigned)height, (double)visprof_g.capture_ms);
    return 0;
}

/* Letters, digits, hyphen and underscore only. Anything else, an empty line or
 * a line over VISPROF_CAPTURE_NAME_MAX characters becomes "capture", so a
 * damaged or hostile request file can never build a path of its own. */
static void vp_clean_name(const char *raw, size_t raw_len, char *out) {
    size_t n = 0;
    int ok = 1;

    while (n < raw_len && raw[n] != '\0' && raw[n] != '\n' && raw[n] != '\r') {
        const char c = raw[n];
        const int allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!allowed)
            ok = 0;
        ++n;
    }
    if (!ok || n == 0 || n > VISPROF_CAPTURE_NAME_MAX) {
        memcpy(out, "capture", 8);
        return;
    }
    memcpy(out, raw, n);
    out[n] = '\0';
}

void visprof_capture_poll(void) {
    char request[VISPROF_CAPTURE_NAME_MAX + 24];
    char name[VISPROF_CAPTURE_NAME_MAX + 1];
    char path[160];

    if (!visprof_g.inited || visprof_g.cfg.capture_request == NULL ||
        visprof_g.cfg.capture_dir == NULL)
        return;

    const uint64_t now_ns = vp_now_ns();
    if (visprof_g.capture_next_ns != 0 && now_ns < visprof_g.capture_next_ns)
        return;
    visprof_g.capture_next_ns = now_ns + visprof_g.capture_poll_ns;

    const file_t in = fs_open(visprof_g.cfg.capture_request, O_RDONLY);
    if (in == FILEHND_INVALID) {
        /* The whole cost of a quiet poll: one failed open. It leaves the
         * frame and phase samples, like a capture does.
         *
         * Measured on a Dreamcast over dcload-ip on 2026-09-15: most of these
         * cost a few milliseconds, but about one in six took roughly 400 ms,
         * and that one frame became the panel's `max` and its retained
         * captured frame. A profiler that reports the cost of its own file
         * system probe as the game's worst frame is lying about the game, so
         * the probe is charged where a capture is charged. A host that wants
         * to know what the probe costs turns the feature off and compares. */
        vp_charge_capture(vp_elapsed_ns(now_ns, vp_now_ns()));
        return;
    }

    const ssize_t got = fs_read(in, request, sizeof(request) - 1u);
    fs_close(in);
    vp_clean_name(request, (got > 0) ? (size_t)got : 0u, name);

    /* Delete the request BEFORE capturing. The capture takes long enough for
     * several polls to have fired otherwise, and a request must produce
     * exactly one picture. */
    fs_unlink(visprof_g.cfg.capture_request);

    /* Reading and deleting the request are two more file operations, which
     * over dc-load are two network round trips. They belong to the picture,
     * so they are charged where the capture's own time is charged. */
    vp_charge_capture(vp_elapsed_ns(now_ns, vp_now_ns()));

    snprintf(path, sizeof(path), "%s/%s.ppm", visprof_g.cfg.capture_dir, name);
    visprof_capture_now(path);

    /* The next look starts after the capture, not before it. */
    visprof_g.capture_next_ns = vp_now_ns() + visprof_g.capture_poll_ns;
}

float visprof_capture_ms(void) {
    return visprof_g.capture_ms;
}

#endif
