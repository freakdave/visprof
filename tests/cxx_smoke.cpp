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
#include <visprof/visprof.h>
#include <kos.h>
static uint32_t g_c;
int main(int, char **) {
    visprof_config_t cfg = {};
    cfg.mode = VISPROF_EXCLUSIVE;
    visprof_init(&cfg);
    visprof_phase_t p = visprof_phase_register("Sim", 0);
    visprof_counter_register("c", &g_c);
    visprof_frame_begin();
    visprof_phase_begin(p);
    { VISPROF_SCOPE("scoped"); ++g_c; }
    visprof_phase_end(p);
    visprof_frame_end();
    visprof_set_mode(VISPROF_INCLUSIVE);
    printf("%.2f %.2f\n", (double)visprof_last_frame_ms(), (double)visprof_self_ms());
    visprof_shutdown();
    return 0;
}
