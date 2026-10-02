/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Muse Jam's audio side for the simulator: the engine runs on the simulated
 * clock, hears its own output as the room, and can be written to a WAV file.
 */
#include "sim_jam.h"

#include <stdio.h>
#include <stdlib.h>

#include "muse_jam.h"
#include "muse_jam_synth.h"

static void *s_mem;
static bool s_active;
static FILE *s_wav;
static uint32_t s_wav_frames;
static uint32_t s_owed;   /* samples due, in 1/1000 of a sample */

bool muse_jam_start(void)
{
    if (!s_mem) {
        s_mem = calloc(1, jam_state_size());
        if (!s_mem) {
            return false;
        }
        jam_init(s_mem);
    } else {
        jam_panic();
    }
    s_active = true;
    return true;
}

void muse_jam_stop(void)
{
    s_active = false;
}

bool muse_jam_active(void)
{
    return s_active;
}

float muse_jam_latency(void)
{
    return 0.0f;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void wav_header(uint32_t frames)
{
    uint8_t h[44] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0,
                      0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0 };
    put32(h + 4, 36 + frames * 2);
    put32(h + 24, JAM_RATE);
    put32(h + 28, JAM_RATE * 2);
    put32(h + 40, frames * 2);
    fseek(s_wav, 0, SEEK_SET);
    fwrite(h, 1, sizeof(h), s_wav);
    fseek(s_wav, 0, SEEK_END);
}

bool sim_jam_record(const char *path)
{
    s_wav = fopen(path, "wb");
    if (!s_wav) {
        return false;
    }
    wav_header(0);
    return true;
}

void sim_jam_finish(void)
{
    if (s_wav) {
        wav_header(s_wav_frames);
        fclose(s_wav);
        s_wav = NULL;
    }
}

void sim_jam_advance_ms(uint32_t ms)
{
    if (!s_active) {
        return;
    }
    s_owed += ms * (JAM_RATE / 1000) * 1000;
    int16_t buf[320];
    while (s_owed >= 1000) {
        uint32_t n = s_owed / 1000;
        n = n > 320 ? 320 : n;
        jam_render(buf, n);
        jam_listen(buf, n);   /* the room is what Muse plays */
        if (s_wav) {
            s_wav_frames += (uint32_t)fwrite(buf, sizeof(int16_t), n, s_wav);
        }
        s_owed -= n * 1000;
    }
}
