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

#pragma once

#include "lvgl.h"
#include "muse_board.h"

/* The desktop board profile and the SDL display it creates: the SenseCAP
 * Watcher's round 412 px screen, or "cores3" for the M5Stack CoreS3's
 * 320x240 one. */
bool sim_board_select(const char *name);
const muse_board_t *sim_board_get(void);
/* What the CoreS3 profile's accelerometer reads, in g: x right, y up. */
void sim_board_set_tilt(float x, float y);
/* A scripted touch at x, y in screen pixels, or its release. */
void sim_board_touch(int x, int y, bool down);
lv_display_t *sim_board_display(void);
