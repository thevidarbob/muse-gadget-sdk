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
 * Muse Jam's sound: a small polyphonic synth played from pads, a 16 step
 * drum machine with a bass line, and a listener that finds the tempo of the
 * music around it. Plain C with no platform calls, so the simulator and the
 * host can render it too.
 *
 * Two threads use it. The UI calls the setters, which only post events or
 * store single words; the audio thread calls jam_render() and jam_listen()
 * every 20 ms. The getters read what either side last wrote.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define JAM_RATE 16000
#define JAM_STEPS 16
#define JAM_ROWS 5
#define JAM_PAD_COLS 5
#define JAM_PAD_ROWS 3
#define JAM_PADS (JAM_PAD_COLS * JAM_PAD_ROWS)
#define JAM_BANDS 24

enum { JAM_ROW_KICK, JAM_ROW_SNARE, JAM_ROW_HAT, JAM_ROW_CLAP, JAM_ROW_BASS };
enum { JAM_PLUCK, JAM_NEON, JAM_GLASS, JAM_CHIP, JAM_PRESETS };
#define JAM_SCALES 6
#define JAM_PATTERNS 5

/* The engine's memory, which the caller allocates (about 90 KB; PSRAM is fine). */
size_t jam_state_size(void);
void jam_init(void *mem);
/* Silences every voice, keeping the settings and the pattern. */
void jam_panic(void);

/* Pads: one finger, so a press, the pads it slides over, and its release. */
void jam_pad_down(int pad);
void jam_pad_move(int pad);
void jam_pad_up(void);
int jam_pad_midi(int pad);
bool jam_pad_is_root(int pad);
const char *jam_note_name(int midi);   /* "C", "F#" */

void jam_set_preset(int preset);
int jam_preset(void);
const char *jam_preset_name(int preset);
void jam_set_scale(int scale);
int jam_scale(void);
const char *jam_scale_name(int scale);
void jam_set_root(int semitone);   /* 0 = C */
int jam_root(void);
void jam_set_chord(bool on);
bool jam_chord(void);
void jam_set_echo(bool on);
bool jam_echo(void);
/* From the tilt: a pitch bend in semitones, and brightness -1..1. */
void jam_set_expression(float bend, float bright);

/* Beat machine. Rows are kick, snare, hat, clap and bass; the bass plays the
 * last pad's note, two octaves down. */
void jam_seq_set_playing(bool on);
bool jam_seq_playing(void);
void jam_seq_set_bpm(float bpm);
float jam_seq_bpm(void);
void jam_seq_set_swing(bool on);
bool jam_seq_swing(void);
bool jam_seq_get(int row, int step);
void jam_seq_set(int row, int step, bool on);
int jam_seq_step(void);   /* the step sounding, -1 when stopped */
void jam_seq_pattern(int pattern);
const char *jam_seq_pattern_name(int pattern);
void jam_seq_remix(uint32_t seed);
/* Starts the pattern over so its next bar begins `seconds` from now. */
void jam_seq_align(float seconds);
void jam_hit(int row);   /* a row's sound, now */

/* Audio thread: the next `n` samples out, and the same span of microphone. */
void jam_render(int16_t *out, size_t n);
void jam_listen(const int16_t *in, size_t n);

/* What the listener hears: band levels 0..1, low to high. */
void jam_bands(float out[JAM_BANDS]);
float jam_tempo(void);           /* BPM, 0 until it's sure */
float jam_next_beat(void);       /* seconds to the next beat it expects */
uint32_t jam_beats(void);        /* counts the beats it hears */
float jam_level(void);           /* the output's envelope, 0..1 */
