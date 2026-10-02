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

#include "muse_jam_synth.h"

#include <math.h>
#include <string.h>

#define VOICES 8
#define KS_LEN 512            /* plucked strings down to 32 Hz */
#define ECHO_LEN 12288        /* three steps at 80 BPM */
#define CTL 16                /* samples per control update */
#define SIN_LEN 1024
#define EVENTS 32

#define FFT_N 512
#define HOP 320               /* one 20 ms read */
#define FRAME_HZ 50.0f
#define ONSETS 320            /* 6.4 s of onset strength */
#define LAG_MAX 46            /* frames in a beat at 65 BPM */

#define TWO_PI 6.28318531f
#define AGC_MIN 12.0f         /* dB: quieter than this is a quiet room */
#define RANGE_DB 40.0f
#define LIMIT 0.8f            /* the limiter's ceiling, before the soft clip */
#define HEARD_LATE 0.022f    /* s: an onset shows once it's well into the window */

enum { EV_DOWN, EV_MOVE, EV_UP, EV_HIT, EV_ALIGN, EV_PANIC };
enum { ATTACK, DECAY, RELEASE };

typedef struct {
    uint8_t type, arg;
} event_t;

typedef struct {
    bool on, held;
    int stage;
    uint32_t age;
    float vel;
    float hz, hz_target;
    float ph1, ph2, ph3;
    float env, fenv;
    float t;
    float ic1, ic2, a1, a2, a3, k;   /* state-variable filter */
    float lp;
    /* plucked string */
    float ks[KS_LEN];
    int ks_w, excite;
    float ks_d, ks_g, ks_lp, ks_s;
} voice_t;

typedef struct {
    float t, amp, amp2, ph, lp, lp2;
    float ic1, ic2, a1, a2, a3, k;
    float gate, hz;
} drum_t;

typedef struct {
    /* settings, written by the UI */
    volatile int preset, scale, root;
    volatile bool chord, echo, playing, swing;
    volatile float bpm, bend, bright;
    volatile uint8_t grid[JAM_ROWS][JAM_STEPS];
    volatile float align_s;

    event_t ev[EVENTS];
    volatile uint32_t ev_w, ev_r;

    /* audio */
    float sin_tab[SIN_LEN + 1];
    uint32_t noise;
    voice_t v[VOICES];
    uint32_t age;
    int held_pad;
    float bend_s, bright_s, ratio, bright01;
    unsigned ctl;
    drum_t drum[JAM_ROWS];
    float clap_a1, clap_a2, clap_a3, clap_k;
    int bass_midi;
    float seq_t;
    volatile int step;
    float echo_buf[ECHO_LEN];
    int echo_w;
    float echo_lp;
    volatile float level;
    float limit;

    /* listening */
    float ring[FFT_N];
    int hop_fill;
    uint16_t band_bin[JAM_BANDS + 1];
    float win[FFT_N];
    float re[FFT_N], im[FFT_N];
    float tw_re[FFT_N / 2], tw_im[FFT_N / 2];
    uint16_t rev[FFT_N];
    float prev_db[FFT_N / 2 + 1];
    float agc, flux_mean;
    float onset[ONSETS];
    float work[ONSETS], ac[4 * LAG_MAX + 6];
    int onset_w, onset_n;
    unsigned frames;
    float lag, beat_in;
    volatile float tempo, next_beat;
    volatile uint32_t beats;
    volatile float bands[JAM_BANDS];
} jam_t;

static jam_t *J;

static const uint8_t SCALES[JAM_SCALES][8] = {
    { 5, 0, 3, 5, 7, 10 },          /* minor pentatonic */
    { 5, 0, 2, 4, 7, 9 },           /* major pentatonic */
    { 7, 0, 2, 3, 5, 7, 9, 10 },    /* dorian */
    { 6, 0, 3, 5, 6, 7, 10 },       /* blues */
    { 7, 0, 2, 4, 5, 7, 9, 11 },    /* major */
    { 5, 0, 1, 5, 7, 8 },           /* in (Japanese) */
};
static const char *const SCALE_NAMES[JAM_SCALES] = { "MIN PENT", "MAJ PENT", "DORIAN", "BLUES", "MAJOR", "IN SEN" };
static const char *const PRESET_NAMES[JAM_PRESETS] = { "PLUCK", "NEON", "GLASS", "CHIP" };
static const char *const NOTE_NAMES[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

/* Attack, decay, sustain and release (seconds, level), and loudness. */
static const float ENV[JAM_PRESETS][5] = {
    { 0.001f, 1.0f, 1.0f, 1.4f, 0.80f },    /* the string decays by itself */
    { 0.010f, 0.5f, 0.6f, 0.35f, 0.30f },
    { 0.002f, 1.6f, 0.0f, 0.9f, 0.50f },
    { 0.002f, 0.2f, 0.6f, 0.07f, 0.24f },
};

static const struct {
    const char *name;
    float bpm;
    const char *rows[JAM_ROWS];
} PATTERNS[JAM_PATTERNS] = {
    { "HOUSE", 122, { "x...x...x...x...", "................", "..x...x...x...x.", "....x.......x...", "..x..x....x..x.x" } },
    { "BOOM BAP", 90, { "x......x..x.....", "....x.......x...", "x.x.x.x.x.x.x.x.", "............x...", "x......x..x....." } },
    { "TRAP", 140, { "x.....x...x.....", "................", "x.x.x.xxx.x.x.xx", "........x.......", "x.....x...x...x." } },
    { "BREAK", 104, { "x.x.......xx....", "....x..x.x..x..x", "x.x.x.x.x.x.x.x.", "................", "x.........x....." } },
    { "EMPTY", 0, { "................", "................", "................", "................", "................" } },
};

static inline float sin1(float phase)   /* sin(2 pi phase), phase in [0, 1) */
{
    float x = phase * SIN_LEN;
    int i = (int)x;
    float f = x - (float)i;
    i &= SIN_LEN - 1;
    return J->sin_tab[i] + (J->sin_tab[i + 1] - J->sin_tab[i]) * f;
}

static inline float noise(void)
{
    uint32_t x = J->noise;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    J->noise = x;
    return (float)(int32_t)x * (1.0f / 2147483648.0f);
}

static inline float wrap(float p)
{
    return p >= 1.0f ? p - (float)(int)p : p;
}

/* A band-limited sawtooth step: removes the aliasing at the wrap. */
static inline float blep(float t, float dt)
{
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

static inline float saw(float t, float dt)
{
    return 2.0f * t - 1.0f - blep(t, dt);
}

static inline float midi_hz(float m)
{
    return 440.0f * powf(2.0f, (m - 69.0f) / 12.0f);
}

static inline float coef(float seconds)   /* per-sample decay to 1/e in `seconds` */
{
    return expf(-1.0f / (seconds * JAM_RATE));
}

static void svf_set(float hz, float q, float *a1, float *a2, float *a3, float *k)
{
    float limit = 0.45f * JAM_RATE;
    hz = hz < 20.0f ? 20.0f : hz > limit ? limit : hz;
    float g = tanf(3.14159265f * hz / JAM_RATE);
    *k = 1.0f / q;
    *a1 = 1.0f / (1.0f + g * (g + *k));
    *a2 = g * *a1;
    *a3 = g * *a2;
}

/* Returns the low pass; `band` gets the band pass. */
static inline float svf(float x, float a1, float a2, float a3, float *ic1, float *ic2, float *band)
{
    float v3 = x - *ic2;
    float v1 = a1 * *ic1 + a2 * v3;
    float v2 = *ic2 + a2 * *ic1 + a3 * v3;
    *ic1 = 2.0f * v1 - *ic1;
    *ic2 = 2.0f * v2 - *ic2;
    if (band) {
        *band = v1;
    }
    return v2;
}

static float step_samples(void)
{
    float bpm = J->bpm;
    bpm = bpm < 50.0f ? 50.0f : bpm > 200.0f ? 200.0f : bpm;
    return JAM_RATE * 60.0f / bpm / 4.0f;
}

/* -- settings, from the UI ---------------------------------------------- */

static void post(int type, int arg)
{
    if (!J || J->ev_w - J->ev_r >= EVENTS) {
        return;
    }
    J->ev[J->ev_w % EVENTS] = (event_t){ (uint8_t)type, (uint8_t)arg };
    __sync_synchronize();
    J->ev_w++;
}

size_t jam_state_size(void)
{
    return sizeof(jam_t);
}

void jam_init(void *mem)
{
    J = mem;
    memset(J, 0, sizeof(*J));
    for (int i = 0; i <= SIN_LEN; i++) {
        J->sin_tab[i] = sinf(TWO_PI * (float)i / SIN_LEN);
    }
    J->noise = 0x9e3779b9u;
    J->preset = JAM_PLUCK;
    J->scale = 0;
    J->root = 9;   /* A minor pentatonic: no wrong notes */
    J->echo = true;
    J->bpm = 122;
    J->step = -1;
    J->held_pad = -1;
    J->bass_midi = 45;
    svf_set(1300.0f, 1.6f, &J->clap_a1, &J->clap_a2, &J->clap_a3, &J->clap_k);
    for (int r = 0; r < JAM_ROWS; r++) {
        J->drum[r].t = -1.0f;
    }

    for (int i = 0; i < FFT_N; i++) {
        J->win[i] = 0.5f - 0.5f * cosf(TWO_PI * (float)i / FFT_N);
        int r = 0;
        for (int b = 0; (1 << b) < FFT_N; b++) {
            r |= ((i >> b) & 1) << (8 - b);
        }
        J->rev[i] = (uint16_t)r;
    }
    for (int i = 0; i < FFT_N / 2; i++) {
        J->tw_re[i] = cosf(TWO_PI * (float)i / FFT_N);
        J->tw_im[i] = -sinf(TWO_PI * (float)i / FFT_N);
    }
    for (int i = 0; i <= FFT_N / 2; i++) {
        J->prev_db[i] = -100.0f;
    }
    int prev = 1;
    for (int b = 0; b <= JAM_BANDS; b++) {
        float hz = 70.0f * powf(7500.0f / 70.0f, (float)b / JAM_BANDS);
        int bin = (int)(hz * FFT_N / JAM_RATE + 0.5f);
        bin = bin <= prev ? prev + 1 : bin;
        J->band_bin[b] = (uint16_t)(bin > FFT_N / 2 ? FFT_N / 2 : bin);
        prev = bin;
    }
    J->agc = AGC_MIN;
    J->ratio = 1.0f;
    J->bright01 = 0.5f;
    jam_seq_pattern(0);
}

void jam_panic(void)
{
    post(EV_PANIC, 0);
}

static int scale_len(void)
{
    return SCALES[J->scale % JAM_SCALES][0];
}

/* The `degree`th note of the scale up from the pads' lowest root. */
static int degree_midi(int degree)
{
    int n = scale_len();
    return 60 + (J->root % 12 > 4 ? J->root % 12 - 12 : J->root % 12) + 12 * (degree / n) +
           SCALES[J->scale % JAM_SCALES][1 + degree % n];
}

int jam_pad_midi(int pad)
{
    return J ? degree_midi(pad) : 60;
}

bool jam_pad_is_root(int pad)
{
    return J && pad % scale_len() == 0;
}

const char *jam_note_name(int midi)
{
    return NOTE_NAMES[((midi % 12) + 12) % 12];
}

void jam_pad_down(int pad)
{
    post(EV_DOWN, pad);
}

void jam_pad_move(int pad)
{
    post(EV_MOVE, pad);
}

void jam_pad_up(void)
{
    post(EV_UP, 0);
}

void jam_hit(int row)
{
    post(EV_HIT, row);
}

void jam_set_preset(int preset)
{
    J->preset = ((preset % JAM_PRESETS) + JAM_PRESETS) % JAM_PRESETS;
}

int jam_preset(void)
{
    return J ? J->preset : 0;
}

const char *jam_preset_name(int preset)
{
    return PRESET_NAMES[((preset % JAM_PRESETS) + JAM_PRESETS) % JAM_PRESETS];
}

void jam_set_scale(int scale)
{
    J->scale = ((scale % JAM_SCALES) + JAM_SCALES) % JAM_SCALES;
}

int jam_scale(void)
{
    return J ? J->scale : 0;
}

const char *jam_scale_name(int scale)
{
    return SCALE_NAMES[((scale % JAM_SCALES) + JAM_SCALES) % JAM_SCALES];
}

void jam_set_root(int semitone)
{
    J->root = ((semitone % 12) + 12) % 12;
}

int jam_root(void)
{
    return J ? J->root : 0;
}

void jam_set_chord(bool on)
{
    J->chord = on;
}

bool jam_chord(void)
{
    return J && J->chord;
}

void jam_set_echo(bool on)
{
    J->echo = on;
}

bool jam_echo(void)
{
    return J && J->echo;
}

void jam_set_expression(float bend, float bright)
{
    J->bend = bend;
    J->bright = bright < -1.0f ? -1.0f : bright > 1.0f ? 1.0f : bright;
}

void jam_seq_set_playing(bool on)
{
    if (on && !J->playing) {
        J->seq_t = 1e9f;   /* the first step sounds at once */
        J->step = JAM_STEPS - 1;
    }
    J->playing = on;
    if (!on) {
        J->step = -1;
    }
}

bool jam_seq_playing(void)
{
    return J && J->playing;
}

void jam_seq_set_bpm(float bpm)
{
    J->bpm = bpm < 60.0f ? 60.0f : bpm > 180.0f ? 180.0f : bpm;
}

float jam_seq_bpm(void)
{
    return J ? J->bpm : 120.0f;
}

void jam_seq_set_swing(bool on)
{
    J->swing = on;
}

bool jam_seq_swing(void)
{
    return J && J->swing;
}

bool jam_seq_get(int row, int step)
{
    return J && J->grid[row][step];
}

void jam_seq_set(int row, int step, bool on)
{
    J->grid[row][step] = on;
}

int jam_seq_step(void)
{
    return J ? J->step : -1;
}

void jam_seq_pattern(int pattern)
{
    pattern = ((pattern % JAM_PATTERNS) + JAM_PATTERNS) % JAM_PATTERNS;
    for (int r = 0; r < JAM_ROWS; r++) {
        for (int s = 0; s < JAM_STEPS; s++) {
            J->grid[r][s] = PATTERNS[pattern].rows[r][s] == 'x';
        }
    }
    if (PATTERNS[pattern].bpm > 0) {
        J->bpm = PATTERNS[pattern].bpm;
    }
}

const char *jam_seq_pattern_name(int pattern)
{
    return PATTERNS[((pattern % JAM_PATTERNS) + JAM_PATTERNS) % JAM_PATTERNS].name;
}

/* A shake: the kick keeps the downbeat, everything else gets reshuffled
 * around the beat, more often where a groove would put it. */
void jam_seq_remix(uint32_t seed)
{
    uint32_t x = seed | 1;
#define RND() (x ^= x << 13, x ^= x >> 17, x ^= x << 5, (float)(x % 1000) / 1000.0f)
    for (int s = 0; s < JAM_STEPS; s++) {
        bool beat = s % 4 == 0, off = s % 2 == 0;
        J->grid[JAM_ROW_KICK][s] = s == 0 || (beat && RND() < 0.55f) || (!beat && RND() < 0.14f);
        J->grid[JAM_ROW_SNARE][s] = (s == 4 || s == 12) ? RND() < 0.85f : RND() < 0.07f;
        J->grid[JAM_ROW_HAT][s] = off ? RND() < 0.9f : RND() < 0.35f;
        J->grid[JAM_ROW_CLAP][s] = (s == 4 || s == 12) ? RND() < 0.4f : RND() < 0.04f;
        J->grid[JAM_ROW_BASS][s] = J->grid[JAM_ROW_KICK][s] ? RND() < 0.8f : RND() < 0.22f;
    }
#undef RND
}

void jam_seq_align(float seconds)
{
    J->align_s = seconds;
    post(EV_ALIGN, 0);
}

/* -- voices ------------------------------------------------------------- */

static voice_t *voice_alloc(void)
{
    voice_t *best = &J->v[0];
    for (int i = 0; i < VOICES; i++) {
        voice_t *v = &J->v[i];
        if (!v->on) {
            return v;
        }
        /* otherwise the oldest released voice, else the oldest */
        bool better = (!v->held && best->held) || (v->held == best->held && v->age < best->age);
        if (better) {
            best = v;
        }
    }
    return best;
}

static void voice_start(voice_t *v, int midi, float vel)
{
    int preset = J->preset;
    float hz = midi_hz((float)midi);
    bool keep_ks = v->on && preset == JAM_PLUCK;   /* restrike a ringing string without a click */
    v->on = true;
    v->held = true;
    v->stage = ATTACK;
    v->age = ++J->age;
    v->vel = vel;
    v->hz = v->hz_target = hz;
    v->t = 0.0f;
    v->fenv = 1.0f;
    if (!keep_ks) {
        v->env = 0.0f;
        v->ph1 = v->ph2 = v->ph3 = 0.0f;
        v->ic1 = v->ic2 = v->lp = 0.0f;
        memset(v->ks, 0, sizeof(v->ks));
        v->ks_lp = 0.0f;
    }
    if (preset == JAM_PLUCK) {
        v->env = 1.0f;
        v->stage = DECAY;
        v->excite = (int)(JAM_RATE / hz);
        v->ks_lp = 0.0f;
    }
}

static void voice_release(voice_t *v)
{
    v->held = false;
    v->stage = RELEASE;
}

static void pad_notes(int pad, int *notes, int *n)
{
    *n = 0;
    notes[(*n)++] = degree_midi(pad);
    if (J->chord) {
        notes[(*n)++] = degree_midi(pad + 2);
        notes[(*n)++] = degree_midi(pad + 4);
    }
}

static void bass_follow(int midi)
{
    int b = midi;
    while (b > 51) {   /* as low as a small speaker still says the note */
        b -= 12;
    }
    while (b < 40) {
        b += 12;
    }
    J->bass_midi = b;
}

static void pad_event(int type, int pad)
{
    bool strum = J->preset == JAM_PLUCK || J->preset == JAM_GLASS;   /* sliding re-strikes */
    int notes[3], n;
    if (type == EV_UP || (type == EV_MOVE && strum)) {
        for (int i = 0; i < VOICES; i++) {
            if (J->v[i].held) {
                voice_release(&J->v[i]);
            }
        }
        J->held_pad = -1;
        if (type == EV_UP) {
            return;
        }
        type = EV_DOWN;
    }
    if (pad < 0 || pad >= JAM_PADS) {
        return;
    }
    pad_notes(pad, notes, &n);
    bass_follow(notes[0]);
    if (type == EV_MOVE && J->held_pad >= 0) {
        /* A sustained sound glides to the new pad. */
        int k = 0;
        for (int i = 0; i < VOICES && k < n; i++) {
            if (J->v[i].held) {
                J->v[i].hz_target = midi_hz((float)notes[k++]);
            }
        }
        J->held_pad = pad;
        return;
    }
    for (int i = 0; i < VOICES; i++) {
        if (J->v[i].held) {
            voice_release(&J->v[i]);
        }
    }
    for (int k = 0; k < n; k++) {
        voice_start(voice_alloc(), notes[k], k ? 0.7f : 1.0f);
    }
    J->held_pad = pad;
}

static void voice_control(voice_t *v, float ratio, float bright01)
{
    v->hz += (v->hz_target - v->hz) * 0.12f;   /* a glide of about 8 ms */
    float hz = v->hz * ratio;
    switch (J->preset) {
    case JAM_PLUCK:
        /* The loop's two-tap average dulls high strings fast: lean it
         * towards one tap as the note rises. */
        v->ks_s = 150.0f / hz;
        v->ks_s = v->ks_s > 0.5f ? 0.5f : v->ks_s < 0.06f ? 0.06f : v->ks_s;
        v->ks_d = JAM_RATE / hz - v->ks_s;
        if (v->ks_d > KS_LEN - 4) {
            v->ks_d = KS_LEN - 4;
        }
        /* The loop loses this once a period: set from how long the string
         * rings, longer when brighter, short once let go. */
        float t60 = v->held ? 2.5f + 3.0f * bright01 : 0.5f + 0.6f * bright01;
        v->ks_g = expf(-6.9f / (t60 * hz));
        break;
    case JAM_NEON: {
        float cut = (280.0f + 2600.0f * bright01) * (1.0f + 2.5f * v->fenv);
        svf_set(cut, 1.1f, &v->a1, &v->a2, &v->a3, &v->k);
        break;
    }
    default:
        break;
    }
}

static float voice_sample(voice_t *v, float ratio, float bright01)
{
    int preset = J->preset;
    const float *e = ENV[preset];
    float hz = v->hz * ratio;
    float dt = hz / JAM_RATE;
    float out = 0.0f;

    switch (preset) {
    case JAM_PLUCK: {
        float rp = (float)v->ks_w - v->ks_d;
        int i0 = (int)floorf(rp);
        float f = rp - (float)i0;
        float a = v->ks[i0 & (KS_LEN - 1)], b = v->ks[(i0 - 1) & (KS_LEN - 1)], c = v->ks[(i0 + 1) & (KS_LEN - 1)];
        float y0 = a + (c - a) * f, y1 = b + (a - b) * f;   /* two taps, half a sample apart */
        float x = 0.0f;
        if (v->excite > 0) {
            v->ks_lp += (noise() - v->ks_lp) * (0.25f + 0.65f * bright01);
            x = v->ks_lp * v->vel * 2.5f;
            v->excite--;
        }
        float y = x + v->ks_g * ((1.0f - v->ks_s) * y0 + v->ks_s * y1);
        v->ks[v->ks_w & (KS_LEN - 1)] = y;
        v->ks_w++;
        out = y * 1.3f;
        break;
    }
    case JAM_NEON: {
        float s = saw(v->ph1, dt * 0.996f) + saw(v->ph2, dt * 1.004f) + 0.8f * sin1(v->ph3);
        v->ph1 = wrap(v->ph1 + dt * 0.996f);
        v->ph2 = wrap(v->ph2 + dt * 1.004f);
        v->ph3 = wrap(v->ph3 + dt * 0.5f);
        out = svf(s, v->a1, v->a2, v->a3, &v->ic1, &v->ic2, NULL);
        v->fenv *= 0.99985f;
        break;
    }
    case JAM_GLASS: {
        /* Less modulation up high, where the sidebands would fold back down. */
        float tame = 700.0f / hz;
        tame = tame > 1.0f ? 1.0f : tame;
        float index = ((0.5f + 2.6f * bright01) * expf(-v->t * 1.8f) + 0.25f) * tame;
        float mod = sin1(v->ph2);
        v->ph2 = wrap(v->ph2 + dt * 3.0f);
        out = sin1(wrap(v->ph1 + index * mod * 0.159f + 1.0f)) + 0.25f * sin1(v->ph3);
        v->ph1 = wrap(v->ph1 + dt);
        v->ph3 = wrap(v->ph3 + dt * 2.0f);
        break;
    }
    default: {   /* CHIP: a quarter-duty pulse with a late vibrato */
        float vib = v->t > 0.25f ? 1.0f + 0.006f * sin1(wrap(v->t * 5.5f)) : 1.0f;
        float d = dt * vib;
        float p2 = wrap(v->ph1 + 0.75f);
        out = saw(v->ph1, d) - saw(p2, d);
        v->ph1 = wrap(v->ph1 + d);
        v->lp += (out - v->lp) * (0.35f + 0.6f * bright01);
        out = v->lp;
        break;
    }
    }

    /* envelope */
    switch (v->stage) {
    case ATTACK:
        v->env += 1.0f / (e[0] * JAM_RATE);
        if (v->env >= 1.0f) {
            v->env = 1.0f;
            v->stage = DECAY;
        }
        break;
    case DECAY:
        if (preset != JAM_PLUCK) {
            float dc = 1.0f - 1.0f / (e[1] * JAM_RATE);
            v->env = e[2] + (v->env - e[2]) * dc;
        }
        break;
    default:
        v->env *= 1.0f - 1.0f / (e[3] * JAM_RATE);
        break;
    }
    v->t += 1.0f / JAM_RATE;
    if (v->env < 0.0005f && v->stage != ATTACK) {
        v->on = v->held = false;
    }
    if (preset == JAM_PLUCK && v->t > 8.0f) {
        voice_release(v);
    }
    return out * v->env * v->vel * e[4];
}

/* -- drums -------------------------------------------------------------- */

static void drum_trigger(int row, float vel)
{
    drum_t *d = &J->drum[row];
    d->t = 0.0f;
    d->amp = vel;
    d->amp2 = vel;
    if (row == JAM_ROW_KICK) {
        d->ph = 0.0f;
    } else if (row == JAM_ROW_BASS) {
        d->hz = midi_hz((float)J->bass_midi);
        d->gate = step_samples() * 0.85f / JAM_RATE;
        d->lp = 1.0f;   /* filter envelope */
        d->amp2 = 0.0f;
    }
}

static float drums_sample(void)
{
    float out = 0.0f;
    drum_t *d = &J->drum[JAM_ROW_KICK];
    if (d->t >= 0.0f) {
        float f = 52.0f + 140.0f * expf(-d->t * 30.0f);
        /* Driven into a soft clip: the overtones are what a small speaker plays. */
        float k = 2.5f * sin1(d->ph) * d->amp;
        out += k / (1.0f + (k < 0.0f ? -k : k)) * 1.1f;
        if (d->t < 0.003f) {
            out += noise() * 0.25f * d->amp;   /* the beater */
        }
        d->ph = wrap(d->ph + f / JAM_RATE);
        d->amp *= 0.99978f;   /* ~0.28 s */
        d->t += 1.0f / JAM_RATE;
        if (d->amp < 0.001f) {
            d->t = -1.0f;
        }
    }
    d = &J->drum[JAM_ROW_SNARE];
    if (d->t >= 0.0f) {
        float n = noise();
        d->lp += (n - d->lp) * 0.3f;
        out += (n - d->lp) * 0.55f * d->amp + sin1(d->ph) * 0.4f * d->amp2;
        d->ph = wrap(d->ph + 190.0f / JAM_RATE);
        d->amp *= 0.99955f;   /* ~0.14 s */
        d->amp2 *= 0.9990f;
        d->t += 1.0f / JAM_RATE;
        if (d->amp < 0.001f) {
            d->t = -1.0f;
        }
    }
    d = &J->drum[JAM_ROW_HAT];
    if (d->t >= 0.0f) {
        float n = noise();
        float h1 = n - d->lp;
        d->lp += (n - d->lp) * 0.55f;
        float h2 = h1 - d->lp2;
        d->lp2 += (h1 - d->lp2) * 0.55f;
        out += h2 * 0.32f * d->amp;
        d->amp *= 0.9982f;   /* ~35 ms */
        d->t += 1.0f / JAM_RATE;
        if (d->amp < 0.001f) {
            d->t = -1.0f;
        }
    }
    d = &J->drum[JAM_ROW_CLAP];
    if (d->t >= 0.0f) {
        float band;
        svf(noise(), J->clap_a1, J->clap_a2, J->clap_a3, &d->ic1, &d->ic2, &band);
        float burst = d->t < 0.031f ? expf(-fmodf(d->t, 0.0104f) * 420.0f) : 0.0f;
        out += band * 1.6f * (burst * d->amp2 + (d->t >= 0.021f ? d->amp : 0.0f));
        if (d->t >= 0.021f) {
            d->amp *= 0.99945f;   /* ~0.11 s tail */
        }
        d->t += 1.0f / JAM_RATE;
        if (d->amp < 0.001f) {
            d->t = -1.0f;
        }
    }
    d = &J->drum[JAM_ROW_BASS];
    if (d->t >= 0.0f) {
        float dt = d->hz / JAM_RATE;
        float s = 0.7f * saw(d->ph, dt) + sin1(d->ph);
        d->ph = wrap(d->ph + dt);
        float y = svf(s, d->a1, d->a2, d->a3, &d->ic1, &d->ic2, NULL);
        if (d->t < d->gate) {
            d->amp2 += (d->amp - d->amp2) * 0.02f;
        } else {
            d->amp2 *= 0.9975f;
        }
        d->lp *= 0.99948f;   /* the filter closes over ~0.12 s */
        out += y * 0.6f * d->amp2;
        d->t += 1.0f / JAM_RATE;
        if (d->t > d->gate && d->amp2 < 0.001f) {
            d->t = -1.0f;
        }
    }
    return out;
}

/* -- sequencer and mix -------------------------------------------------- */

static void seq_sample(void)
{
    if (!J->playing) {
        return;
    }
    float len = step_samples();
    int step = J->step;
    /* Swing: the even steps hold a little longer, so the odd ones land late. */
    float dur = J->swing ? len * (step % 2 ? 0.8f : 1.2f) : len;
    J->seq_t += 1.0f;
    if (J->seq_t < dur) {
        return;
    }
    J->seq_t -= dur;
    if (J->seq_t > len) {
        J->seq_t = 0.0f;
    }
    step = (step + 1) % JAM_STEPS;
    J->step = step;
    for (int r = 0; r < JAM_ROWS; r++) {
        if (!J->grid[r][step]) {
            continue;
        }
        float vel = 1.0f;
        if (r == JAM_ROW_HAT) {
            vel = step % 4 == 0 ? 1.0f : step % 2 == 0 ? 0.7f : 0.5f;
        }
        drum_trigger(r, vel);
    }
}

static void handle_event(event_t e)
{
    switch (e.type) {
    case EV_DOWN:
    case EV_MOVE:
    case EV_UP:
        pad_event(e.type, e.arg);
        break;
    case EV_HIT:
        if (e.arg < JAM_ROWS) {
            drum_trigger(e.arg, 1.0f);
        }
        break;
    case EV_ALIGN: {
        float len = step_samples();
        float total = J->align_s * JAM_RATE;
        int n = (int)ceilf(total / len);
        n = n < 1 ? 1 : n > JAM_STEPS ? JAM_STEPS : n;
        J->step = JAM_STEPS - n;
        J->seq_t = (float)n * len - total;
        J->seq_t = J->seq_t < 0.0f ? 0.0f : J->seq_t;
        J->playing = true;
        break;
    }
    case EV_PANIC:
        for (int i = 0; i < VOICES; i++) {
            J->v[i].on = J->v[i].held = false;
        }
        for (int r = 0; r < JAM_ROWS; r++) {
            J->drum[r].t = -1.0f;
        }
        memset(J->echo_buf, 0, sizeof(J->echo_buf));
        J->held_pad = -1;
        break;
    default:
        break;
    }
}

static void control(void)
{
    J->bend_s += (J->bend - J->bend_s) * 0.15f;
    J->bright_s += (J->bright - J->bright_s) * 0.15f;
    J->ratio = powf(2.0f, J->bend_s / 12.0f);
    J->bright01 = 0.5f + 0.5f * J->bright_s;
    for (int i = 0; i < VOICES; i++) {
        if (J->v[i].on) {
            voice_control(&J->v[i], J->ratio, J->bright01);
        }
    }
    drum_t *b = &J->drum[JAM_ROW_BASS];
    if (b->t >= 0.0f) {
        svf_set(140.0f + 1700.0f * b->lp * (0.5f + 0.5f * J->bright01), 1.5f, &b->a1, &b->a2, &b->a3, &b->k);
    }
}

void jam_render(int16_t *out, size_t n)
{
    if (!J) {
        memset(out, 0, n * sizeof(*out));
        return;
    }
    while (J->ev_r != J->ev_w) {
        __sync_synchronize();
        event_t e = J->ev[J->ev_r % EVENTS];
        J->ev_r++;
        handle_event(e);
    }
    int echo_d = (int)(3.0f * step_samples());   /* a dotted eighth */
    echo_d = echo_d >= ECHO_LEN ? ECHO_LEN - 1 : echo_d;
    float level = J->level;
    for (size_t i = 0; i < n; i++) {
        if (J->ctl++ % CTL == 0) {
            control();
        }
        float synth = 0.0f;
        for (int k = 0; k < VOICES; k++) {
            if (J->v[k].on) {
                synth += voice_sample(&J->v[k], J->ratio, J->bright01);
            }
        }
        seq_sample();
        float drums = drums_sample();

        int r = J->echo_w - echo_d;
        float wet = J->echo_buf[r < 0 ? r + ECHO_LEN : r];
        J->echo_lp += (wet - J->echo_lp) * 0.4f;
        J->echo_buf[J->echo_w] = (J->echo ? synth : 0.0f) + J->echo_lp * 0.45f;
        J->echo_w = (J->echo_w + 1) % ECHO_LEN;

        float x = (synth + wet * 0.4f + drums * 0.85f) * 0.55f;
        /* A limiter, then a soft clip: loud chords over the drums squash
         * instead of cracking. It lets go over about 60 ms. */
        float a = x < 0.0f ? -x : x;
        J->limit = a > J->limit ? a : J->limit * 0.999f;
        if (J->limit > LIMIT) {
            x *= LIMIT / J->limit;
        }
        x = x > 1.0f ? 1.0f : x < -1.0f ? -1.0f : 1.5f * x - 0.5f * x * x * x;
        a = x < 0.0f ? -x : x;
        level = a > level ? a : level * 0.9997f;
        out[i] = (int16_t)(x * 30000.0f);
    }
    J->level = level;
}

/* -- listening ---------------------------------------------------------- */

static void fft(void)
{
    float *re = J->re, *im = J->im;
    for (int len = 2; len <= FFT_N; len <<= 1) {
        int half = len >> 1, stride = FFT_N / len;
        for (int i = 0; i < FFT_N; i += len) {
            for (int j = 0; j < half; j++) {
                float wr = J->tw_re[j * stride], wi = J->tw_im[j * stride];
                int a = i + j, b = a + half;
                float xr = re[b] * wr - im[b] * wi;
                float xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr;
                im[b] = im[a] - xi;
                re[a] += xr;
                im[a] += xi;
            }
        }
    }
}

static float onset_at(int t, int n)   /* t from 0, the oldest of the last n */
{
    return J->onset[(J->onset_w - n + t + 2 * ONSETS) % ONSETS];
}

/* Tempo by autocorrelating the onsets: a beat's period also lines up at two
 * and four beats, and a lean towards 120 BPM keeps it from being heard at
 * half or double speed. Then where the beats fall. */
static void estimate_tempo(void)
{
    float *o = J->work, *ac = J->ac;
    int n = J->onset_n < ONSETS ? J->onset_n : ONSETS;
    /* Blurred a little: a beat between two frames shows in both. */
    float mean = 0.0f;
    for (int t = 0; t < n; t++) {
        float sum = 0.0f, w = 0.0f;
        for (int d = -2; d <= 2; d++) {
            if (t + d >= 0 && t + d < n) {
                float k = d == 0 ? 0.4f : d == 1 || d == -1 ? 0.2f : 0.1f;
                sum += k * onset_at(t + d, n);
                w += k;
            }
        }
        o[t] = sum / w;
        mean += o[t];
    }
    mean /= (float)n;
    float e0 = 0.0f;
    for (int t = 0; t < n; t++) {
        o[t] -= mean;
        e0 += o[t] * o[t];
    }
    e0 /= (float)n;
    if (e0 < 1e-6f) {
        J->tempo = 0.0f;
        J->lag = 0.0f;
        return;
    }
    int max_lag = 4 * LAG_MAX + 4;
    max_lag = max_lag > n - 40 ? n - 40 : max_lag;
    for (int lag = 0; lag <= max_lag; lag++) {
        float sum = 0.0f;
        for (int t = lag; t < n; t++) {
            sum += o[t] * o[t - lag];
        }
        ac[lag] = sum / (float)(n - lag) / e0;
    }
    /* Every whole BPM, with the lags between frames interpolated. */
    float best_sc = -1e9f, lag = 0.0f;
    for (int bpm = 66; bpm <= 184; bpm++) {
        float l = FRAME_HZ * 60.0f / (float)bpm, h = 0.0f;
        static const float HARM[3] = { 1.0f, 0.6f, 0.4f };
        for (int k = 0; k < 3; k++) {
            float x = l * (float)(1 << k);
            int i = (int)x;
            if (i + 1 > max_lag) {
                break;
            }
            h += HARM[k] * (ac[i] + (ac[i + 1] - ac[i]) * (x - (float)i));
        }
        float oct = log2f((float)bpm / 120.0f) / 0.6f;
        h *= expf(-0.5f * oct * oct);
        if (h > best_sc) {
            best_sc = h;
            lag = l;
        }
    }
    if (best_sc < 0.25f) {
        J->tempo = 0.0f;
        J->lag = 0.0f;
        return;
    }

    /* Phase: the offset, to a quarter frame, whose comb of beats back in
     * time lands on the most onset. */
    float top = -1e9f, phase = 0.0f;
    for (float p = 0.0f; p < lag; p += 0.25f) {
        float sum = 0.0f;
        for (int k = 0; k < 8; k++) {
            float t = (float)(n - 1) - p - (float)k * lag;
            int i = (int)t;
            if (i < 0) {
                break;
            }
            sum += o[i] + (o[i + 1 < n ? i + 1 : i] - o[i]) * (t - (float)i);
        }
        if (sum > top) {
            top = sum;
            phase = p;
        }
    }
    float tempo = FRAME_HZ * 60.0f / lag;
    float old = J->tempo;
    J->tempo = old > 0.0f && fabsf(tempo - old) < old * 0.04f ? old * 0.6f + tempo * 0.4f : tempo;
    J->lag = FRAME_HZ * 60.0f / J->tempo;
    J->beat_in = lag - phase;
}

static void analyze(void)
{
    for (int i = 0; i < FFT_N; i++) {
        J->re[J->rev[i]] = J->ring[i] * J->win[i];
        J->im[i] = 0.0f;
    }
    fft();

    float db[FFT_N / 2 + 1];
    float frame_max = -100.0f;
    for (int k = 0; k <= FFT_N / 2; k++) {
        db[k] = 10.0f * log10f(J->re[k] * J->re[k] + J->im[k] * J->im[k] + 1e-12f);
        if (k >= 2 && k < 230 && db[k] > frame_max) {
            frame_max = db[k];
        }
    }
    J->agc = frame_max > J->agc ? frame_max : J->agc - 0.08f;
    J->agc = J->agc < AGC_MIN ? AGC_MIN : J->agc;
    float floor_db = J->agc - RANGE_DB;

    for (int b = 0; b < JAM_BANDS; b++) {
        float m = -100.0f;
        for (int k = J->band_bin[b]; k < J->band_bin[b + 1]; k++) {
            m = db[k] > m ? db[k] : m;
        }
        float v = (m - floor_db) / RANGE_DB;
        v = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
        float old = J->bands[b];
        J->bands[b] = v > old ? v : old * 0.82f + v * 0.18f;
    }

    /* Onsets: how much louder the spectrum got since the last frame. */
    /* The lows (kick and bass) count most: they carry the beat. */
    float flux = 0.0f, low = J->agc - 50.0f;
    for (int k = 2; k < 200; k++) {
        float now = db[k] > low ? db[k] : low;
        float was = J->prev_db[k] > low ? J->prev_db[k] : low;
        flux += (now > was ? now - was : 0.0f) * (k < 10 ? 16.0f : 1.0f);
        J->prev_db[k] = db[k];
    }
    flux /= 326.0f;
    J->flux_mean += (flux - J->flux_mean) * 0.04f;
    float o = flux - J->flux_mean;
    J->onset[J->onset_w] = o > 0.0f ? o : 0.0f;
    J->onset_w = (J->onset_w + 1) % ONSETS;
    J->onset_n++;

    if (J->lag > 0.0f) {
        J->beat_in -= 1.0f;
        if (J->beat_in <= 0.0f) {
            J->beat_in += J->lag;
            J->beats++;
        }
        J->next_beat = J->beat_in / FRAME_HZ - HEARD_LATE;
    }
    if (++J->frames % 25 == 0 && J->onset_n >= 150) {
        estimate_tempo();
    }
}

void jam_listen(const int16_t *in, size_t n)
{
    if (!J) {
        return;
    }
    while (n) {
        size_t room = (size_t)(HOP - J->hop_fill);
        size_t k = n > room ? room : n;
        memmove(J->ring, J->ring + k, (FFT_N - k) * sizeof(float));
        for (size_t i = 0; i < k; i++) {
            J->ring[FFT_N - k + i] = (float)in[i] * (1.0f / 32768.0f);
        }
        in += k;
        n -= k;
        J->hop_fill += (int)k;
        if (J->hop_fill >= HOP) {
            J->hop_fill = 0;
            analyze();
        }
    }
}

void jam_bands(float out[JAM_BANDS])
{
    for (int b = 0; b < JAM_BANDS; b++) {
        out[b] = J ? J->bands[b] : 0.0f;
    }
}

float jam_tempo(void)
{
    return J ? J->tempo : 0.0f;
}

float jam_next_beat(void)
{
    return J ? J->next_beat : 0.0f;
}

uint32_t jam_beats(void)
{
    return J ? J->beats : 0;
}

float jam_level(void)
{
    return J ? J->level : 0.0f;
}
