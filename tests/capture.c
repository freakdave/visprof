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

/* Host tests for the screen capture: a stub video memory, stub display
 * registers and a stub filesystem, running the real conversion and the real
 * request state machine. */
#include "visprof_internal.h"
#include <dc/pvr.h>
#include <dc/video.h>
#include <dc/biosfont.h>
#include <kos/fs.h>
#include <kos/thread.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* ---- what the library sees instead of the console -------------------- */

uint32_t vp_test_reg[VP_TEST_REG_WORDS];
uint8_t  vp_test_vram[VP_TEST_VRAM_BYTES];

static vp_vid_mode_t screen = { 640, 480 };
vp_vid_mode_t *vid_mode = &screen;
unsigned test_irq_mask;

static uint64_t now_ns;
static int failures;
static int checks;

#define CHECK(expr) do { \
    ++checks; \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
        ++failures; \
    } \
} while (0)

uint64_t timer_ns_gettime64(void) { return now_ns; }
int dbglog(int level, const char *fmt, ...) { (void)level; (void)fmt; return 0; }
int visprof_atlas_bake(uint32_t color, int half) { (void)color; (void)half; return 0; }
void visprof_atlas_free(void) {}
void bfont_draw_str_vram_fmt(uint32_t x, uint32_t y, bool opaque, const char *fmt, ...) {
    (void)x; (void)y; (void)opaque; (void)fmt;
}
static kthread_t main_thread = { 1, "main" };
kthread_t *thd_get_current(void) { return &main_thread; }
uint64_t thd_get_cpu_time(kthread_t *thread) { (void)thread; return 0; }
int thd_each(int (*cb)(kthread_t *, void *), void *data) { return cb(&main_thread, data); }

/* ---- stub filesystem -------------------------------------------------- */

#define VP_FILES      4
#define VP_FILE_BYTES (64 * 1024)

typedef struct {
    char    name[64];
    uint8_t data[VP_FILE_BYTES];
    size_t  size;
    int     used;
    int     reading;
} vp_file_t;

static vp_file_t files[VP_FILES];
static int   opens_read;
static int   opens_write;
static int   opens_failed;
static int   writes;
static int   step;              /* Monotonic operation counter. */
static int   unlink_step;       /* Step at which the request was deleted. */
static int   open_step;         /* Step at which the picture was opened. */
static const char *open_denied; /* Name whose open must fail. */
static size_t short_write_at;   /* Byte count after which writes go short. */
static size_t written_total;
static uint64_t write_cost_ns = 1000000; /* Each write advances the clock. */
static uint64_t open_cost_ns;

static vp_file_t *find_file(const char *name) {
    for (int i = 0; i < VP_FILES; ++i)
        if (files[i].used && strcmp(files[i].name, name) == 0)
            return &files[i];
    return NULL;
}

file_t fs_open(const char *fn, int mode) {
    ++step;
    now_ns += open_cost_ns;
    if ((mode & O_WRONLY) != 0) {
        if (open_denied != NULL && strcmp(open_denied, fn) == 0) {
            ++opens_failed;
            return FILEHND_INVALID;
        }
        vp_file_t *f = find_file(fn);
        if (f == NULL) {
            for (int i = 0; i < VP_FILES && f == NULL; ++i)
                if (!files[i].used) f = &files[i];
        }
        if (f == NULL)
            return FILEHND_INVALID;
        memset(f, 0, sizeof(*f));
        snprintf(f->name, sizeof(f->name), "%s", fn);
        f->used = 1;
        ++opens_write;
        open_step = step;
        return (file_t)(f - files);
    }
    vp_file_t *f = find_file(fn);
    if (f == NULL) {
        ++opens_failed;
        return FILEHND_INVALID;
    }
    f->reading = 1;
    ++opens_read;
    return (file_t)(f - files);
}

int fs_close(file_t hnd) {
    if (hnd >= 0 && hnd < VP_FILES)
        files[hnd].reading = 0;
    return 0;
}

ssize_t fs_read(file_t hnd, void *buffer, size_t cnt) {
    if (hnd < 0 || hnd >= VP_FILES)
        return -1;
    vp_file_t *f = &files[hnd];
    const size_t take = (f->size < cnt) ? f->size : cnt;
    memcpy(buffer, f->data, take);
    return (ssize_t)take;
}

ssize_t fs_write(file_t hnd, const void *buffer, size_t cnt) {
    ++step;
    ++writes;
    now_ns += write_cost_ns;
    if (hnd < 0 || hnd >= VP_FILES)
        return -1;
    vp_file_t *f = &files[hnd];
    size_t take = cnt;
    if (short_write_at != 0 && written_total + take > short_write_at)
        take = (written_total < short_write_at) ? short_write_at - written_total : 0;
    if (f->size + take > VP_FILE_BYTES)
        take = VP_FILE_BYTES - f->size;
    memcpy(f->data + f->size, buffer, take);
    f->size += take;
    written_total += take;
    return (ssize_t)take;
}

int fs_unlink(const char *fn) {
    ++step;
    vp_file_t *f = find_file(fn);
    if (f == NULL)
        return -1;
    unlink_step = step;
    f->used = 0;
    return 0;
}

static void put_request(const char *name, const char *body) {
    vp_file_t *f = NULL;
    for (int i = 0; i < VP_FILES && f == NULL; ++i)
        if (!files[i].used) f = &files[i];
    if (f == NULL)
        return;
    memset(f, 0, sizeof(*f));
    snprintf(f->name, sizeof(f->name), "%s", name);
    f->size = strlen(body);
    memcpy(f->data, body, f->size);
    f->used = 1;
}

/* ---- helpers ---------------------------------------------------------- */

static void reset(const visprof_config_t *cfg) {
    visprof_config_t use = { 0 };
    if (cfg != NULL)
        use = *cfg;
    visprof_shutdown();
    memset(files, 0, sizeof(files));
    memset(vp_test_reg, 0, sizeof(vp_test_reg));
    memset(vp_test_vram, 0, sizeof(vp_test_vram));
    now_ns = 1000000000;
    screen.width = 640;
    screen.height = 480;
    opens_read = opens_write = opens_failed = writes = 0;
    step = unlink_step = open_step = 0;
    open_denied = NULL;
    short_write_at = 0;
    written_total = 0;
    write_cost_ns = 1000000;
    open_cost_ns = 0;
    CHECK(visprof_init(&use) == 0);
}

static void frame_only(float ms) {
    visprof_frame_begin();
    now_ns += (uint64_t)(ms * 1000000.0f);
    visprof_frame_end();
}

/* Display on, pixel mode in bits 3:2, framebuffer at `offset`. */
static void set_display(uint32_t pixel_mode, uint32_t offset, uint32_t w, uint32_t h) {
    vp_test_reg[PVR_FB_CFG_1 >> 2] = 1u | (pixel_mode << 2);
    vp_test_reg[PVR_FB_ADDR >> 2] = offset;
    screen.width = w;
    screen.height = h;
}

static void put16(uint32_t offset, uint32_t index, uint16_t value) {
    memcpy(vp_test_vram + offset + index * 2u, &value, sizeof(value));
}

static void put32(uint32_t offset, uint32_t index, uint32_t value) {
    memcpy(vp_test_vram + offset + index * 4u, &value, sizeof(value));
}

static const vp_file_t *picture(const char *name) {
    return find_file(name);
}

/* Compares the first `n` bytes of a file with a string. */
static int head_is(const vp_file_t *f, const char *text) {
    const size_t n = strlen(text);
    return f != NULL && f->size >= n && memcmp(f->data, text, n) == 0;
}

/* One pixel of the body, after the header. */
static int pixel_is(const vp_file_t *f, size_t header, uint32_t index,
                    uint8_t r, uint8_t g, uint8_t b) {
    const size_t at = header + (size_t)index * 3u;
    if (f == NULL || f->size < at + 3u)
        return 0;
    return f->data[at] == r && f->data[at + 1] == g && f->data[at + 2] == b;
}

/* ---- cases ------------------------------------------------------------ */

/* The header is exact and the body is width*height*3 bytes. */
static void header_and_length(void) {
    reset(NULL);
    set_display(1u, 0x400u, 4u, 2u);
    CHECK(visprof_capture_now("/ram/shot.ppm") == 0);
    const vp_file_t *f = picture("/ram/shot.ppm");
    CHECK(head_is(f, "P6\n4 2\n255\n"));
    CHECK(f != NULL && f->size == strlen("P6\n4 2\n255\n") + 4u * 2u * 3u);
    /* One write for the header and one per row. */
    CHECK(writes == 3);
}

/* Each pixel format converts the way KOS's own screenshot converts it. */
static void formats(void) {
    const size_t header = strlen("P6\n2 1\n255\n");

    /* 565: red 0xF800, green 0x07E0. */
    reset(NULL);
    set_display(1u, 0x100u, 2u, 1u);
    put32(0x100u, 0u, 0x07E0F800u);
    CHECK(visprof_capture_now("/ram/a.ppm") == 0);
    CHECK(pixel_is(picture("/ram/a.ppm"), header, 0u, 248u, 0u, 0u));
    CHECK(pixel_is(picture("/ram/a.ppm"), header, 1u, 0u, 252u, 0u));

    /* 555: red 0x7C00, blue 0x001F. */
    reset(NULL);
    set_display(0u, 0x100u, 2u, 1u);
    put32(0x100u, 0u, 0x001F7C00u);
    CHECK(visprof_capture_now("/ram/b.ppm") == 0);
    CHECK(pixel_is(picture("/ram/b.ppm"), header, 0u, 248u, 0u, 0u));
    CHECK(pixel_is(picture("/ram/b.ppm"), header, 1u, 0u, 0u, 248u));

    /* 888 packed: stored blue, green, red. */
    reset(NULL);
    set_display(2u, 0x100u, 2u, 1u);
    vp_test_vram[0x100] = 0x11; vp_test_vram[0x101] = 0x22; vp_test_vram[0x102] = 0x33;
    vp_test_vram[0x103] = 0x44; vp_test_vram[0x104] = 0x55; vp_test_vram[0x105] = 0x66;
    CHECK(visprof_capture_now("/ram/c.ppm") == 0);
    CHECK(pixel_is(picture("/ram/c.ppm"), header, 0u, 0x33u, 0x22u, 0x11u));
    CHECK(pixel_is(picture("/ram/c.ppm"), header, 1u, 0x66u, 0x55u, 0x44u));

    /* 0888: one 32-bit word per pixel, top byte unused. */
    reset(NULL);
    set_display(3u, 0x100u, 2u, 1u);
    put32(0x100u, 0u, 0xFF112233u);
    put32(0x100u, 1u, 0x00445566u);
    CHECK(visprof_capture_now("/ram/d.ppm") == 0);
    CHECK(pixel_is(picture("/ram/d.ppm"), header, 0u, 0x11u, 0x22u, 0x33u));
    CHECK(pixel_is(picture("/ram/d.ppm"), header, 1u, 0x44u, 0x55u, 0x66u));
}

/* An odd width ends on a single 16-bit pixel, and rows follow the stride. */
static void odd_width_and_rows(void) {
    const size_t header = strlen("P6\n3 2\n255\n");
    reset(NULL);
    set_display(1u, 0x200u, 3u, 2u);
    /* Row 0: white, black, blue. Row 1 starts one stride (6 bytes) later. */
    put16(0x200u, 0u, 0xFFFFu);
    put16(0x200u, 1u, 0x0000u);
    put16(0x200u, 2u, 0x001Fu);
    put16(0x200u, 3u, 0xF800u);
    put16(0x200u, 4u, 0x07E0u);
    put16(0x200u, 5u, 0xFFFFu);
    CHECK(visprof_capture_now("/ram/e.ppm") == 0);
    const vp_file_t *f = picture("/ram/e.ppm");
    CHECK(f != NULL && f->size == header + 3u * 2u * 3u);
    CHECK(pixel_is(f, header, 0u, 248u, 252u, 248u));
    CHECK(pixel_is(f, header, 1u, 0u, 0u, 0u));
    CHECK(pixel_is(f, header, 2u, 0u, 0u, 248u));   /* the odd tail pixel */
    CHECK(pixel_is(f, header, 3u, 248u, 0u, 0u));   /* first pixel of row 1 */
    CHECK(pixel_is(f, header, 5u, 248u, 252u, 248u));
}

/* The picture follows the DISPLAY REGISTER, not a fixed address. This is the
 * whole point of the module: whichever buffer the driver's double buffering
 * has current is the one that is read. */
static void follows_the_register(void) {
    const size_t header = strlen("P6\n1 1\n255\n");
    reset(NULL);
    set_display(1u, 0x800u, 1u, 1u);
    put16(0x800u, 0u, 0xF800u);          /* buffer A: red */
    put16(0x1000u, 0u, 0x001Fu);         /* buffer B: blue */
    CHECK(visprof_capture_now("/ram/a.ppm") == 0);
    CHECK(pixel_is(picture("/ram/a.ppm"), header, 0u, 248u, 0u, 0u));

    vp_test_reg[PVR_FB_ADDR >> 2] = 0x1000u;   /* the hardware flipped */
    CHECK(visprof_capture_now("/ram/b.ppm") == 0);
    CHECK(pixel_is(picture("/ram/b.ppm"), header, 0u, 0u, 0u, 248u));
}

/* Every refusal has its own reason, and none of them writes a file. */
static void refusals(void) {
    reset(NULL);
    set_display(1u, 0x100u, 4u, 2u);
    CHECK(visprof_capture_now(NULL) == VISPROF_CAPTURE_EARG);
    CHECK(opens_write == 0);

    screen.width = 0;
    CHECK(visprof_capture_now("/ram/x.ppm") == VISPROF_CAPTURE_EMODE);
    CHECK(opens_write == 0);

    screen.width = VISPROF_CAPTURE_MAX_WIDTH + 1u;
    CHECK(visprof_capture_now("/ram/x.ppm") == VISPROF_CAPTURE_EMODE);
    CHECK(opens_write == 0);

    screen.width = 4u;
    vp_test_reg[PVR_FB_CFG_1 >> 2] &= ~1u;     /* display disabled */
    CHECK(visprof_capture_now("/ram/x.ppm") == VISPROF_CAPTURE_EFORMAT);
    CHECK(opens_write == 0);
    CHECK(picture("/ram/x.ppm") == NULL);

    reset(NULL);
    set_display(1u, 0x100u, 4u, 2u);
    open_denied = "/ram/x.ppm";
    CHECK(visprof_capture_now("/ram/x.ppm") == VISPROF_CAPTURE_EOPEN);
    CHECK(writes == 0);
    CHECK(picture("/ram/x.ppm") == NULL);

    /* A short write is reported, and the partial file is closed either way. */
    reset(NULL);
    set_display(1u, 0x100u, 4u, 2u);
    short_write_at = 14;                 /* header plus two body bytes */
    CHECK(visprof_capture_now("/ram/y.ppm") == VISPROF_CAPTURE_EWRITE);
    CHECK(picture("/ram/y.ppm") != NULL);
    CHECK(picture("/ram/y.ppm")->size == 14u);
}

/* The capture leaves no spike behind: its time is out of the frame and out of
 * the phase in BOTH timing modes, and visprof_capture_ms() reports it. */
static void accounting(visprof_mode_t mode) {
    visprof_config_t cfg = { 0 };
    cfg.history_len = 4;
    cfg.mode = mode;
    reset(&cfg);
    set_display(1u, 0x100u, 4u, 2u);
    const visprof_phase_t phase = visprof_phase_register("Sim", 0);

    visprof_frame_begin();
    visprof_phase_begin(phase);
    now_ns += 2000000;                   /* 2 ms of real work */
    CHECK(visprof_capture_now("/ram/f.ppm") == 0);  /* 3 writes = 3 ms */
    now_ns += 1000000;                   /* 1 ms more of real work */
    visprof_phase_end(phase);
    visprof_frame_end();

    CHECK(visprof_capture_ms() > 2.99f && visprof_capture_ms() < 3.01f);
    CHECK(visprof_last_phase_ms(phase) > 2.99f && visprof_last_phase_ms(phase) < 3.01f);
    CHECK(visprof_last_frame_ms() > 2.99f && visprof_last_frame_ms() < 3.01f);
    /* The accumulators are consumed, not carried into the next frame. */
    CHECK(visprof_g.capture_frame_ns == 0);
    CHECK(visprof_g.capture_phase_ns[phase] == 0);
    frame_only(5.0f);
    CHECK(visprof_last_frame_ms() > 4.99f && visprof_last_frame_ms() < 5.01f);

    /* A second begin/end pair for the same phase in one frame overwrites the
     * phase value. That pair did not pay for the capture the first one did, so
     * the subtraction must not happen twice. */
    visprof_frame_begin();
    visprof_phase_begin(phase);
    CHECK(visprof_capture_now("/ram/g.ppm") == 0);   /* 3 writes = 3 ms */
    visprof_phase_end(phase);
    visprof_phase_begin(phase);
    now_ns += 2000000;
    visprof_phase_end(phase);
    visprof_frame_end();
    CHECK(visprof_last_phase_ms(phase) > 1.99f && visprof_last_phase_ms(phase) < 2.01f);
    CHECK(visprof_last_frame_ms() > 1.99f && visprof_last_frame_ms() < 2.01f);
}

/* ---- the request state machine ---------------------------------------- */

static visprof_config_t poll_config(uint32_t hz) {
    visprof_config_t cfg = { 0 };
    cfg.capture_request = "/pc/captures/request.txt";
    cfg.capture_dir = "/pc/captures";
    cfg.capture_poll_hz = hz;
    return cfg;
}

/* No configuration, no cost at all. */
static void poll_off(void) {
    reset(NULL);
    set_display(1u, 0x100u, 4u, 2u);
    for (int i = 0; i < 10; ++i) {
        visprof_capture_poll();
        now_ns += 1000000000ull;
    }
    CHECK(opens_read == 0);
    CHECK(opens_write == 0);
    CHECK(opens_failed == 0);
}

/* Half a configuration is a configuration error: both are cleared. */
static void poll_half_configured(void) {
    visprof_config_t cfg = { 0 };
    cfg.capture_request = "/pc/captures/request.txt";
    reset(&cfg);
    CHECK(visprof_g.cfg.capture_request == NULL);
    CHECK(visprof_g.cfg.capture_dir == NULL);
    visprof_capture_poll();
    CHECK(opens_failed == 0);
}

/* A quiet poll is one failed open per interval, and nothing else. */
static void poll_rate(void) {
    visprof_config_t cfg = poll_config(4u);
    reset(&cfg);
    set_display(1u, 0x100u, 4u, 2u);

    visprof_capture_poll();
    CHECK(opens_failed == 1);
    now_ns += 100000000ull;              /* 100 ms: not due */
    visprof_capture_poll();
    CHECK(opens_failed == 1);
    now_ns += 160000000ull;              /* 260 ms since the first look */
    visprof_capture_poll();
    CHECK(opens_failed == 2);
    CHECK(opens_write == 0);

    /* What a quiet poll costs leaves the frame sample, the way a capture's
     * cost does. A failed open over dc-load can take hundreds of
     * milliseconds, and that is not the game's frame time. */
    open_cost_ns = 500000000ull;         /* half a second, as measured */
    now_ns += 300000000ull;              /* past the 250 ms period: due */
    visprof_frame_begin();
    now_ns += 16000000ull;
    visprof_capture_poll();
    CHECK(opens_failed == 3);
    CHECK(visprof_g.capture_frame_ns >= 500000000ull);
    visprof_frame_end();
    /* This is the first frame of the case, so its period starts at
     * frame_begin, not at a previous frame end: 16 ms of work. The half
     * second the open took is not part of it. */
    CHECK(visprof_last_frame_ms() > 15.9f && visprof_last_frame_ms() < 16.1f);
    CHECK(visprof_self_ms() < 0.01f);
}

/* A request becomes exactly one picture, named by its first line, and the
 * request is deleted BEFORE the picture is opened. */
static void poll_request(void) {
    visprof_config_t cfg = poll_config(1u);
    reset(&cfg);
    set_display(1u, 0x100u, 4u, 2u);
    put_request("/pc/captures/request.txt", "hardware-01\n");

    visprof_capture_poll();
    CHECK(picture("/pc/captures/hardware-01.ppm") != NULL);
    CHECK(head_is(picture("/pc/captures/hardware-01.ppm"), "P6\n4 2\n255\n"));
    CHECK(find_file("/pc/captures/request.txt") == NULL);
    CHECK(unlink_step > 0 && open_step > unlink_step);

    /* The next interval finds nothing: one request, one picture. */
    now_ns += 2000000000ull;
    visprof_capture_poll();
    CHECK(opens_write == 1);
}

/* Anything that is not a plain name becomes "capture". */
static void poll_names(void) {
    static const char *const bad[] = {
        "../../etc/passwd\n",
        "with space\n",
        "\n",
        "",
        "0123456789012345678901234567890123456789X\n"   /* 41 characters */
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        visprof_config_t cfg = poll_config(1u);
        reset(&cfg);
        set_display(1u, 0x100u, 4u, 2u);
        put_request("/pc/captures/request.txt", bad[i]);
        visprof_capture_poll();
        CHECK(picture("/pc/captures/capture.ppm") != NULL);
    }

    /* Exactly forty characters is still a name. */
    visprof_config_t cfg = poll_config(1u);
    reset(&cfg);
    set_display(1u, 0x100u, 4u, 2u);
    put_request("/pc/captures/request.txt", "0123456789012345678901234567890123456789\n");
    visprof_capture_poll();
    CHECK(picture("/pc/captures/0123456789012345678901234567890123456789.ppm") != NULL);
}

int main(void) {
    header_and_length();
    formats();
    odd_width_and_rows();
    follows_the_register();
    refusals();
    accounting(VISPROF_INCLUSIVE);
    accounting(VISPROF_EXCLUSIVE);
    poll_off();
    poll_half_configured();
    poll_rate();
    poll_request();
    poll_names();
    printf("capture: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
