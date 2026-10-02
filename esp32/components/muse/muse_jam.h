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
 * Muse Jam: a full-screen instrument behind a button on Muse's face. PLAY is
 * a grid of pads locked to a scale, bent and brightened by tilting the
 * gadget; BEATS is a 16 step drum machine that a shake remixes; LISTEN shows
 * the room's music as a spectrum, finds its tempo, and syncs the beat to it.
 * A talk press, or the screen going to sleep, ends it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

enum { MUSE_JAM_PLAY, MUSE_JAM_BEATS, MUSE_JAM_LISTEN, MUSE_JAM_TABS };

/* Audio side. muse_jam_start() makes the engine (once) and hands the voice
 * task's idle loop to muse_jam_audio_tick() until muse_jam_stop(). */
bool muse_jam_start(void);
void muse_jam_stop(void);
bool muse_jam_active(void);
/* 20 ms of the jam out to the speaker, and the same of the microphone into
 * `mic` and the listener. In place of the idle loop's read, which it paces
 * the same way. */
void muse_jam_audio_tick(int16_t *mic);
/* Seconds from rendering a sample to hearing it, plus hearing to analysing:
 * what a beat synced to the room has to start early by. */
float muse_jam_latency(void);

/* UI side, from the LVGL task. The face's button that opens it, centred
 * `x`, `y` off the face's centre; NULL on a screen without touch or too
 * small for the pads. */
lv_obj_t *muse_jam_ui_add_button(lv_obj_t *face, int x, int y);
void muse_jam_ui_open(int tab);
void muse_jam_ui_close(void);
bool muse_jam_ui_is_open(void);
