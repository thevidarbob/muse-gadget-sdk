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

/* Runs Muse Jam's engine (muse_jam_synth.c) for test_muse_jam.py, one
 * command a line, each answered with a line of JSON:
 *   tempo <pattern> <bpm>  the tempo the listener hears in 10 s of the pattern
 *   pitch <preset> <pad>   a held pad's level at its note and a semitone either side
 *   sync <pattern> <bpm>   SYNC after 8 s of the pattern as the room: how far
 *                          the beats that follow land from the room's, in ms
 *   mix                    every sound at once for 8 s: peak, RMS, clipped samples */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "muse_jam_synth.h"

static void *s_mem;

static void fresh(void)
{
    jam_init(s_mem);
    jam_seq_pattern(JAM_PATTERNS - 1);   /* empty */
}

static double goertzel(const int16_t *x, int n, double hz)
{
    double w = 2.0 * M_PI * hz / JAM_RATE, c = 2.0 * cos(w), s1 = 0.0, s2 = 0.0;
    for (int i = 0; i < n; i++) {
        double s = x[i] + c * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    return sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / n;
}

static void tempo(int pattern, float bpm)
{
    fresh();
    jam_seq_pattern(pattern);
    jam_seq_set_bpm(bpm);
    jam_seq_set_playing(true);
    int16_t buf[320];
    for (int i = 0; i < 50 * 10; i++) {
        jam_render(buf, 320);
        jam_listen(buf, 320);
    }
    printf("{\"tempo\": %.2f}\n", (double)jam_tempo());
}

static void pitch(int preset, int pad)
{
    fresh();
    jam_set_preset(preset);
    jam_set_echo(false);
    jam_pad_down(pad);
    static int16_t buf[8000];
    jam_render(buf, 800);
    jam_render(buf, 4000);
    double hz = 440.0 * pow(2.0, (jam_pad_midi(pad) - 69) / 12.0), semi = pow(2.0, 1.0 / 12.0);
    printf("{\"hz\": %.2f, \"at\": %.3f, \"below\": %.3f, \"above\": %.3f}\n", hz, goertzel(buf, 4000, hz),
           goertzel(buf, 4000, hz / semi), goertzel(buf, 4000, hz * semi));
}

static void sync(int pattern, float bpm)
{
    int n = JAM_RATE * 8;
    int16_t *room = malloc(n * sizeof(int16_t));
    fresh();
    jam_seq_pattern(pattern);
    jam_seq_set_bpm(bpm);
    jam_seq_set_playing(true);
    for (int i = 0; i < n; i += 320) {
        jam_render(room + i, 320);
    }
    fresh();
    for (int i = 0; i < n; i += 320) {
        jam_listen(room + i, 320);
    }
    float heard = jam_tempo();
    if (heard > 0.0f) {
        jam_seq_set_bpm(heard);
        jam_seq_align(jam_next_beat());
    }
    /* The room's beats fall on whole periods from its first sample. */
    double period = JAM_RATE * 60.0 / bpm, worst = 0.0;
    int16_t s;
    jam_render(&s, 0);   /* takes the align, so the step it sets isn't counted as one sounding */
    int last = jam_seq_step(), beats = 0;
    for (int i = 0; i < JAM_RATE * 10 && beats < 12; i++) {
        jam_render(&s, 1);
        int step = jam_seq_step();
        if (step != last && step % 4 == 0) {
            double ph = fmod((double)(n + i), period);
            double e = fabs(ph > period / 2 ? ph - period : ph) * 1000.0 / JAM_RATE;
            worst = e > worst ? e : worst;
            beats++;
        }
        last = step;
    }
    printf("{\"tempo\": %.2f, \"beats\": %d, \"worst_ms\": %.2f}\n", (double)heard, beats, worst);
    free(room);
}

static void mix(void)
{
    fresh();
    jam_seq_pattern(0);
    jam_seq_set_playing(true);
    jam_set_chord(true);
    int16_t buf[320];
    double acc = 0.0;
    int peak = 0, clipped = 0, n = 0;
    for (int f = 0; f < 50 * 8; f++) {
        if (f % 25 == 0) {
            jam_set_preset(f / 100);
            jam_pad_down((f / 25) % JAM_PADS);
            jam_set_expression((float)((f / 25) % 3) - 1.0f, 1.0f);
        }
        jam_render(buf, 320);
        for (int i = 0; i < 320; i++) {
            int a = abs(buf[i]);
            peak = a > peak ? a : peak;
            clipped += a >= 29990;
            acc += (double)buf[i] * buf[i];
            n++;
        }
    }
    printf("{\"peak\": %d, \"rms_db\": %.2f, \"clipped\": %.5f}\n", peak,
           10.0 * log10(acc / n / (32768.0 * 32768.0)), (double)clipped / n);
}

int main(void)
{
    s_mem = calloc(1, jam_state_size());
    char line[128];
    while (fgets(line, sizeof(line), stdin)) {
        int a, b;
        float f;
        if (sscanf(line, "tempo %d %f", &a, &f) == 2) {
            tempo(a, f);
        } else if (sscanf(line, "pitch %d %d", &a, &b) == 2) {
            pitch(a, b);
        } else if (sscanf(line, "sync %d %f", &a, &f) == 2) {
            sync(a, f);
        } else if (!strncmp(line, "mix", 3)) {
            mix();
        } else {
            fprintf(stderr, "bad command: %s", line);
            return 2;
        }
    }
    free(s_mem);
    return 0;
}
