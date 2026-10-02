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

#include "sim_board.h"

#include <math.h>
#include <string.h>
#include "sim_platform.h"

#include "src/drivers/sdl/lv_sdl_mouse.h"
#include "src/drivers/sdl/lv_sdl_window.h"

#define WATCHER_RESOLUTION 412

static lv_display_t *s_display;
static const muse_board_t *s_selected;
static float s_tilt[2];
static lv_point_t s_touch;
static bool s_touching;

/* muse_ui.c reads the selected board through this production global. */
const muse_board_t *muse_board;

static esp_err_t sim_init(void)
{
    return ESP_OK;
}

/* A finger a scenario puts down, beside the mouse. */
static void scripted_touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point = s_touch;
    data->state = s_touching ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

void sim_board_touch(int x, int y, bool down)
{
    if (down) {
        s_touch.x = x;
        s_touch.y = y;
    }
    s_touching = down;
}

static lv_display_t *sim_display_start(lv_indev_t **touch)
{
    s_display = lv_sdl_window_create(muse_board->width, muse_board->height);
    if (!s_display) {
        return NULL;
    }
    /* The SDL driver installs SDL_GetTicks. Use the simulator clock instead so
     * scripted runs can advance time without sleeping and render repeatably. */
    lv_tick_set_cb(sim_time_tick_ms);
    lv_sdl_window_set_title(s_display, "Muse Gadget Simulator");
    lv_sdl_window_set_resizeable(s_display, false);
    if (touch) {
        *touch = lv_sdl_mouse_create();
    }
    lv_indev_t *scripted = lv_indev_create();
    lv_indev_set_type(scripted, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(scripted, scripted_touch_read);
    return s_display;
}

static bool sim_display_lock(int timeout_ms)
{
    (void)timeout_ms;
    return true;
}

static void sim_display_unlock(void)
{
}

static void sim_set_brightness(int pct)
{
    (void)pct;
}

static void sim_panel_sleep(bool sleep)
{
    (void)sleep;
}

static esp_err_t sim_power_off(void)
{
    return ESP_FAIL;
}

/* Gravity as the board's IMU would report it, tilted by a scenario. */
static esp_err_t sim_read_accel(float g[3])
{
    g[0] = s_tilt[0];
    g[1] = s_tilt[1];
    float z = 1.0f - g[0] * g[0] - g[1] * g[1];
    g[2] = z > 0.0f ? sqrtf(z) : 0.0f;
    return ESP_OK;
}

void sim_board_set_tilt(float x, float y)
{
    s_tilt[0] = x;
    s_tilt[1] = y;
}

static const muse_board_t s_sim_board = {
    .name = "SenseCAP Watcher Simulator",
    .width = WATCHER_RESOLUTION,
    .height = WATCHER_RESOLUTION,
    .round = true,
    .touch = true,
    .diagonal_in = 1.45f,
    .talk_button = "wheel",
    .aux_button = "scroll",
    .talk_hint = { LV_ALIGN_CENTER, 100, -143 },
    .frame_ms = 40,
    .init = sim_init,
    .display_start = sim_display_start,
    .display_lock = sim_display_lock,
    .display_unlock = sim_display_unlock,
    .set_brightness = sim_set_brightness,
    .panel_sleep = sim_panel_sleep,
    .power_off = sim_power_off,
};

/* The M5Stack CoreS3's 2" landscape LCD, for the wide layout and Muse Jam. */
static const muse_board_t s_sim_cores3 = {
    .name = "M5Stack CoreS3 Simulator",
    .width = 320,
    .height = 240,
    .round = false,
    .touch = true,
    .diagonal_in = 2.0f,
    .talk_button = "power",
    .talk_hint = { LV_ALIGN_LEFT_MID, 14, 24 },
    .frame_ms = 40,
    .init = sim_init,
    .display_start = sim_display_start,
    .display_lock = sim_display_lock,
    .display_unlock = sim_display_unlock,
    .set_brightness = sim_set_brightness,
    .panel_sleep = sim_panel_sleep,
    .power_off = sim_power_off,
    .read_accel = sim_read_accel,
};

bool sim_board_select(const char *name)
{
    if (!strcmp(name, "watcher")) {
        s_selected = &s_sim_board;
    } else if (!strcmp(name, "cores3")) {
        s_selected = &s_sim_cores3;
    } else {
        return false;
    }
    return true;
}

const muse_board_t *sim_board_get(void)
{
    return s_selected ? s_selected : &s_sim_board;
}

lv_display_t *sim_board_display(void)
{
    return s_display;
}
