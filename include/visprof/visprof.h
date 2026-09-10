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
#ifndef VISPROF_VISPROF_H
#define VISPROF_VISPROF_H

#include <stdint.h>

/* Set consistently for the library and its callers. KOS defines NDEBUG
 * in normal builds, so it does not control this switch. */
#ifndef VISPROF_ENABLED
#  define VISPROF_ENABLED 1
#endif

#define VISPROF_VERSION_MAJOR 0
#define VISPROF_VERSION_MINOR 1
#define VISPROF_VERSION_PATCH 0
#define VISPROF_VERSION_STRING "0.1.0"

#define VISPROF_MAX_PHASES     4
#define VISPROF_MAX_ZONES     32
#define VISPROF_MAX_COUNTERS  16
#define VISPROF_MAX_HISTORY  256
#define VISPROF_TOP_ZONES      8

#define VISPROF_INVALID_ZONE    0xFFFFFFFFu
#define VISPROF_INVALID_PHASE   0xFFFFFFFFu
#define VISPROF_INVALID_COUNTER 0xFFFFFFFFu

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t visprof_phase_t;
typedef uint32_t visprof_zone_t;
typedef uint32_t visprof_counter_t;

/* These modes include or subtract measured profiler overhead from frames
 * and phases. Neither changes zone times or excludes child calls. */
typedef enum {
    VISPROF_INCLUSIVE = 0,
    VISPROF_EXCLUSIVE = 1
} visprof_mode_t;

/* Ordered by row. The zone box is below top anchors and above the others. */
typedef enum {
    VISPROF_TOP_LEFT = 0,  VISPROF_TOP_CENTER,    VISPROF_TOP_RIGHT,
    VISPROF_MID_LEFT,      VISPROF_MID_CENTER,    VISPROF_MID_RIGHT,
    VISPROF_BOTTOM_LEFT,   VISPROF_BOTTOM_CENTER, VISPROF_BOTTOM_RIGHT
} visprof_anchor_t;

typedef struct {
    uint32_t history_len; /* Ring slots and graph width. Default 180, range 2..256. */
    uint32_t panel_argb;  /* Opaque panel and glyph background. Default 0xFF181818. */
    float    x, y;        /* Pixel offset from the anchor. Positive is right/down. */
    float    text_scale;  /* 0 < value < 1 selects 6x12 text. Otherwise 12x24. */
    float    px_per_ms;   /* Pixels per millisecond. Default 3.0 for values <= 0. */
    int      thief_scan;  /* Optional thread CPU scan. Default off. */
    int      spike_log;   /* Optional frame logging through dbglog. Default off. */
    visprof_mode_t mode;  /* Default VISPROF_INCLUSIVE. */
    visprof_anchor_t anchor; /* Default VISPROF_TOP_LEFT. */
} visprof_config_t;

#if VISPROF_ENABLED

/* Call all functions from the frame thread. */

/* Call after PVR initialization. NULL selects defaults. Returns 0 on
 * success, -1 for a usable graph without text, or 1 if already initialized. */
int  visprof_init(const visprof_config_t *cfg);

/* Call after PVR work has finished using the atlas. */
void visprof_shutdown(void);

/* Register before the first frame. Name storage must remain valid and
 * unchanged until shutdown. Zero argb selects a default phase color.
 * Full tables or NULL arguments return the corresponding INVALID constant. */
visprof_phase_t   visprof_phase_register(const char *name, uint32_t argb);

/* The caller owns and resets host_cell. Keep it valid until shutdown.
 * The display shows the value sampled at the last frame_end. */
visprof_counter_t visprof_counter_register(const char *name, const uint32_t *host_cell);

/* Call once each per frame. Close all zones and PVR lists before frame_end.
 * Later frames include time since the preceding frame_end timestamp. */
void visprof_frame_begin(void);
void visprof_frame_end(void);

/* Phases must not overlap or nest. Use one begin/end pair per phase per frame.
 * Repeated pairs overwrite that phase's value. Invalid handles are ignored. */
void visprof_phase_begin(visprof_phase_t p);
void visprof_phase_end(visprof_phase_t p);

/* Each zone call is a separate entry. Nested zones include their children.
 * Name storage must remain valid and unchanged until shutdown. NULL uses "?".
 * Close scopes before frame_end. Do not reuse handles across frames.
 * Table overflow returns VISPROF_INVALID_ZONE. */
visprof_zone_t visprof_zone_begin(const char *literal_name);
void           visprof_zone_end(visprof_zone_t z);

/* Add an elapsed duration measured by the caller, in milliseconds. */
void visprof_zone_add_ms(const char *literal_name, float ms);

/* Call inside an open PVR_LIST_TR_POLY list using direct rendering and bins.
 * Host depth must be below 1e9. In presort mode, submit the overlay last.
 * Draws prepared data without allocation or string formatting. */
void visprof_draw(void);

/* Starts hidden. Recording continues while hidden. */
void visprof_set_visible(int on);
int  visprof_visible(void);

/* Mode changes leave existing history samples unchanged. */
void           visprof_set_mode(visprof_mode_t mode);
visprof_mode_t visprof_get_mode(void);
void             visprof_set_anchor(visprof_anchor_t anchor);
visprof_anchor_t visprof_get_anchor(void);

float visprof_last_frame_ms(void);
float visprof_last_phase_ms(visprof_phase_t p);

/* Measured elapsed profiler overhead for the last frame, in either mode.
 * Excludes phase/zone timer-call overhead and GPU rendering time. */
float visprof_self_ms(void);
uint32_t visprof_glyphs_drawn(void);
uint32_t visprof_rects_drawn(void);

/* Emergency front-buffer text. A display flip can erase it. */
void visprof_panic_dump(void);

#else

static inline int  visprof_init(const visprof_config_t *cfg) { (void)cfg; return 0; }
static inline void visprof_shutdown(void) {}
static inline visprof_phase_t visprof_phase_register(const char *n, uint32_t a) { (void)n; (void)a; return 0; }
static inline visprof_counter_t visprof_counter_register(const char *n, const uint32_t *c) { (void)n; (void)c; return 0; }
static inline void visprof_frame_begin(void) {}
static inline void visprof_frame_end(void) {}
static inline void visprof_phase_begin(visprof_phase_t p) { (void)p; }
static inline void visprof_phase_end(visprof_phase_t p) { (void)p; }
static inline visprof_zone_t visprof_zone_begin(const char *n) { (void)n; return VISPROF_INVALID_ZONE; }
static inline void visprof_zone_end(visprof_zone_t z) { (void)z; }
static inline void visprof_zone_add_ms(const char *n, float ms) { (void)n; (void)ms; }
static inline void visprof_draw(void) {}
static inline void visprof_set_visible(int on) { (void)on; }
static inline int  visprof_visible(void) { return 0; }
static inline void visprof_set_mode(visprof_mode_t mode) { (void)mode; }
static inline visprof_mode_t visprof_get_mode(void) { return VISPROF_INCLUSIVE; }
static inline void visprof_set_anchor(visprof_anchor_t anchor) { (void)anchor; }
static inline visprof_anchor_t visprof_get_anchor(void) { return VISPROF_TOP_LEFT; }
static inline float visprof_last_frame_ms(void) { return 0.0f; }
static inline float visprof_last_phase_ms(visprof_phase_t p) { (void)p; return 0.0f; }
static inline float visprof_self_ms(void) { return 0.0f; }
static inline uint32_t visprof_glyphs_drawn(void) { return 0; }
static inline uint32_t visprof_rects_drawn(void) { return 0; }
static inline void visprof_panic_dump(void) {}

#endif

#ifdef __cplusplus
}
#endif


#define VISPROF_CAT_(a, b) a##b
#define VISPROF_CAT(a, b) VISPROF_CAT_(a, b)

/* C uses GNU cleanup. C++ uses a scope destructor. */
#if !VISPROF_ENABLED
#  define VISPROF_SCOPE(name) ((void)0)
#elif defined(__cplusplus)
class VisprofScope {
public:
    explicit VisprofScope(const char *name) : m_zone(visprof_zone_begin(name)) {}
    ~VisprofScope() { visprof_zone_end(m_zone); }
    VisprofScope(const VisprofScope &) = delete;
    VisprofScope &operator=(const VisprofScope &) = delete;
private:
    visprof_zone_t m_zone;
};
#  define VISPROF_SCOPE(name) VisprofScope VISPROF_CAT(visprof_scope_, __LINE__)(name)
#else
static inline void visprof_scope_end_(const visprof_zone_t *z) { visprof_zone_end(*z); }
#  define VISPROF_SCOPE(name)                                   \
    visprof_zone_t VISPROF_CAT(visprof_scope_, __LINE__)        \
        __attribute__((cleanup(visprof_scope_end_))) = visprof_zone_begin(name)
#endif

#endif
