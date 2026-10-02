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

#include "muse_jam.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_timer.h"

#include "muse_board.h"
#include "muse_jam_synth.h"
#include "muse_settings.h"
#include "muse_state.h"

/* Laid out for 320x240, centred on anything bigger: a 412 px circle fits it. */
#define W 320
#define H 240
#define TOP 30
#define PAGE_H (H - TOP)
#define TICK_MS 33

#define COLOR_BG 0x07060d
#define COLOR_PANEL 0x17142b
#define COLOR_CELL 0x1d1a33
#define COLOR_CELL_BEAT 0x2b2748
#define COLOR_DIM 0x7a76a0
#define COLOR_TEXT 0xf2f0ff

static const uint32_t PRESET_COLOR[JAM_PRESETS] = { 0xffb02e, 0xff3dcb, 0x3de0ff, 0x9dff3d };
static const uint32_t ROW_COLOR[JAM_ROWS] = { 0xff4d6d, 0xffb02e, 0x3de0ff, 0xb388ff, 0x9dff3d };
static const char *const ROW_NAME[JAM_ROWS] = { "KICK", "SNARE", "HAT", "CLAP", "BASS" };
static const char *const TAB_NAME[MUSE_JAM_TABS] = { "PLAY", "BEATS", "LISTEN" };
static const uint32_t TAB_COLOR[MUSE_JAM_TABS] = { 0, 0xff4d6d, 0x3de0ff };   /* PLAY takes the preset's */

static lv_obj_t *s_root;
static lv_obj_t *s_tabs[MUSE_JAM_TABS], *s_pages[MUSE_JAM_TABS];
static lv_timer_t *s_timer;
static int s_tab;
static lv_obj_t *s_toast;
static float s_toast_until;
static lv_obj_t *s_tilt_box, *s_tilt_dot, *s_muted;

/* PLAY */
static lv_obj_t *s_pad_area, *s_pads[JAM_PADS], *s_pad_lbl[JAM_PADS];
static float s_glow[JAM_PADS];
static uint8_t s_drawn[JAM_PADS];
static int s_pad_held = -1;
static lv_obj_t *s_btn_preset, *s_btn_root, *s_btn_scale, *s_btn_chord, *s_btn_echo;

/* BEATS */
static lv_obj_t *s_grid_area, *s_cells[JAM_ROWS][JAM_STEPS], *s_row_lbl[JAM_ROWS];
static float s_row_glow[JAM_ROWS];
static int s_paint = -1, s_last_cell = -1, s_shown_step = -1;
static lv_obj_t *s_btn_play, *s_bpm_lbl, *s_btn_pattern, *s_btn_swing;
static int s_pattern;

/* LISTEN */
static lv_obj_t *s_bars[JAM_BANDS], *s_tempo_lbl, *s_hint_lbl, *s_beat_dot, *s_btn_sync;
static int s_bar_h[JAM_BANDS];
static uint32_t s_seen_beats;
static float s_beat_glow;
static int s_shown_tempo = -1;

/* tilt */
static float s_rest[3];
static bool s_have_rest;
static float s_bend, s_bright, s_last_shake;

static float now_s(void)
{
    return (float)esp_timer_get_time() / 1e6f;
}

static lv_color_t shade(uint32_t color, float amount)   /* the panel colour towards `color` */
{
    amount = amount < 0.0f ? 0.0f : amount > 1.0f ? 1.0f : amount;
    return lv_color_mix(lv_color_hex(color), lv_color_hex(COLOR_PANEL), (uint8_t)(amount * 255.0f));
}

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_radius(o, radius, 0);
    return o;
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *s)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, s);
    return l;
}

static lv_obj_t *button(lv_obj_t *parent, int x, int y, int w, int h, const char *s, lv_event_cb_t cb, intptr_t arg)
{
    lv_obj_t *b = box(parent, x, y, w, h, COLOR_PANEL, 8);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(b, 2);
    lv_obj_add_event_cb(b, cb, LV_EVENT_ALL, (void *)arg);
    lv_obj_t *l = text(b, &lv_font_montserrat_14, COLOR_TEXT, s);
    lv_obj_center(l);
    return b;
}

static void button_set(lv_obj_t *b, const char *s, uint32_t lit)   /* lit 0: dark */
{
    lv_obj_t *l = lv_obj_get_child(b, 0);
    if (s && strcmp(lv_label_get_text(l), s) != 0) {
        lv_label_set_text(l, s);
    }
    lv_obj_set_style_bg_color(b, lit ? lv_color_hex(lit) : lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(lit ? 0x000000 : COLOR_TEXT), 0);
}

/* Pressed buttons dim a moment, like the rest of Muse's touch targets. */
static bool clicked(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *b = lv_event_get_current_target(e);
    if (code == LV_EVENT_PRESSED) {
        lv_obj_set_style_opa(b, LV_OPA_70, 0);
        muse_state_poke();
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        lv_obj_set_style_opa(b, LV_OPA_COVER, 0);
    }
    return code == LV_EVENT_CLICKED;
}

static void toast(const char *s)
{
    lv_label_set_text(s_toast, s);
    lv_obj_align(s_toast, LV_ALIGN_CENTER, 0, -8);
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_toast);
    s_toast_until = now_s() + 1.3f;
}

/* -- PLAY --------------------------------------------------------------- */

#define PAD_X 4
#define PAD_Y 2
#define PAD_GAP 4
#define PAD_W 59
#define PAD_H 51

static void restyle_pads(void)
{
    for (int i = 0; i < JAM_PADS; i++) {
        int midi = jam_pad_midi(i);
        lv_label_set_text_fmt(s_pad_lbl[i], "%s%d", jam_note_name(midi), midi / 12 - 1);
        s_drawn[i] = 0xff;
    }
}

static void draw_pads(void)
{
    uint32_t c = PRESET_COLOR[jam_preset()];
    for (int i = 0; i < JAM_PADS; i++) {
        float base = jam_pad_is_root(i) ? 0.34f : 0.16f;
        float g = s_glow[i];
        uint8_t level = (uint8_t)((base + (1.0f - base) * g) * 40.0f);
        if (level == s_drawn[i]) {
            continue;
        }
        s_drawn[i] = level;
        lv_obj_set_style_bg_color(s_pads[i], shade(c, (float)level / 40.0f), 0);
        lv_obj_set_style_text_color(s_pad_lbl[i], lv_color_hex(g > 0.5f ? 0x000000 : (jam_pad_is_root(i) ? c : COLOR_DIM)), 0);
    }
}

static int pad_at(void)
{
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    lv_area_t a;
    lv_obj_get_coords(s_pad_area, &a);
    int x = p.x - a.x1, y = p.y - a.y1;
    int w = lv_area_get_width(&a), h = lv_area_get_height(&a);
    if (x < 0 || y < 0 || x >= w || y >= h) {
        return -1;
    }
    int col = x * JAM_PAD_COLS / w, row = y * JAM_PAD_ROWS / h;
    return (JAM_PAD_ROWS - 1 - row) * JAM_PAD_COLS + col;   /* low notes at the bottom */
}

static void on_pads(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        int pad = pad_at();
        if (pad < 0 || pad == s_pad_held) {
            return;
        }
        if (s_pad_held < 0) {
            jam_pad_down(pad);
            muse_state_poke();
        } else {
            jam_pad_move(pad);
        }
        s_pad_held = pad;
        s_glow[pad] = 1.0f;
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (s_pad_held >= 0) {
            jam_pad_up();
        }
        s_pad_held = -1;
    }
}

static void show_play_buttons(void)
{
    uint32_t c = PRESET_COLOR[jam_preset()];
    button_set(s_btn_preset, jam_preset_name(jam_preset()), c);
    button_set(s_btn_root, jam_note_name(jam_root()), 0);
    button_set(s_btn_scale, jam_scale_name(jam_scale()), 0);
    button_set(s_btn_chord, "CHORD", jam_chord() ? c : 0);
    button_set(s_btn_echo, "ECHO", jam_echo() ? c : 0);
    if (s_tab == MUSE_JAM_PLAY) {
        button_set(s_tabs[MUSE_JAM_PLAY], NULL, c);
    }
}

static void on_play_button(lv_event_t *e)
{
    if (!clicked(e)) {
        return;
    }
    lv_obj_t *b = lv_event_get_current_target(e);
    if (b == s_btn_preset) {
        jam_set_preset(jam_preset() + 1);
        memset(s_drawn, 0xff, sizeof(s_drawn));
    } else if (b == s_btn_root) {
        jam_set_root(jam_root() + 1);
        restyle_pads();
    } else if (b == s_btn_scale) {
        jam_set_scale(jam_scale() + 1);
        restyle_pads();
    } else if (b == s_btn_chord) {
        jam_set_chord(!jam_chord());
    } else if (b == s_btn_echo) {
        jam_set_echo(!jam_echo());
    }
    show_play_buttons();
}

static void build_play(lv_obj_t *page)
{
    s_pad_area = box(page, PAD_X, PAD_Y, JAM_PAD_COLS * (PAD_W + PAD_GAP) - PAD_GAP,
                     JAM_PAD_ROWS * (PAD_H + PAD_GAP) - PAD_GAP, COLOR_BG, 0);
    lv_obj_add_flag(s_pad_area, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_pad_area, on_pads, LV_EVENT_ALL, NULL);
    for (int i = 0; i < JAM_PADS; i++) {
        int col = i % JAM_PAD_COLS, row = JAM_PAD_ROWS - 1 - i / JAM_PAD_COLS;
        s_pads[i] = box(s_pad_area, col * (PAD_W + PAD_GAP), row * (PAD_H + PAD_GAP), PAD_W, PAD_H, COLOR_PANEL, 10);
        s_pad_lbl[i] = text(s_pads[i], &lv_font_montserrat_14, COLOR_DIM, "");
        lv_obj_align(s_pad_lbl[i], LV_ALIGN_BOTTOM_LEFT, 7, -5);
    }
    restyle_pads();

    int y = PAGE_H - 40;
    s_btn_preset = button(page, 4, y, 68, 36, "", on_play_button, 0);
    s_btn_root = button(page, 74, y, 34, 36, "", on_play_button, 0);
    s_btn_scale = button(page, 110, y, 86, 36, "", on_play_button, 0);
    s_btn_chord = button(page, 198, y, 62, 36, "", on_play_button, 0);
    s_btn_echo = button(page, 262, y, 54, 36, "", on_play_button, 0);
}

/* -- BEATS -------------------------------------------------------------- */

#define GRID_X 46
#define GRID_W 270
#define ROW_PITCH 32
#define CELL_H 28

static void style_cell(int r, int s)
{
    bool on = jam_seq_get(r, s), head = s == s_shown_step;
    lv_color_t c;
    if (on) {
        c = head ? lv_color_mix(lv_color_white(), lv_color_hex(ROW_COLOR[r]), 150) : lv_color_hex(ROW_COLOR[r]);
    } else {
        c = lv_color_hex(head ? 0x3d3866 : s % 4 == 0 ? COLOR_CELL_BEAT : COLOR_CELL);
    }
    lv_obj_set_style_bg_color(s_cells[r][s], c, 0);
}

static void style_grid(void)
{
    for (int r = 0; r < JAM_ROWS; r++) {
        for (int s = 0; s < JAM_STEPS; s++) {
            style_cell(r, s);
        }
    }
}

static int cell_at(void)
{
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    lv_area_t a;
    lv_obj_get_coords(s_grid_area, &a);
    int x = p.x - a.x1, y = p.y - a.y1;
    if (x < 0 || y < 0 || x >= lv_area_get_width(&a) || y >= lv_area_get_height(&a)) {
        return -1;
    }
    return (y / ROW_PITCH) * JAM_STEPS + x * JAM_STEPS / GRID_W;
}

/* A press toggles its cell; dragging paints the same over the cells it crosses. */
static void on_grid(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        int c = cell_at();
        if (c < 0 || c == s_last_cell || c >= JAM_ROWS * JAM_STEPS) {
            return;
        }
        int r = c / JAM_STEPS, s = c % JAM_STEPS;
        if (s_paint < 0) {
            s_paint = !jam_seq_get(r, s);
            muse_state_poke();
        }
        jam_seq_set(r, s, s_paint);
        if (s_paint && !jam_seq_playing()) {
            jam_hit(r);   /* hear what was drawn */
        }
        style_cell(r, s);
        s_last_cell = c;
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s_paint = -1;
        s_last_cell = -1;
    }
}

/* The row names are drum pads too. */
static void on_row(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_PRESSED) {
        return;
    }
    int r = (int)(intptr_t)lv_event_get_user_data(e);
    jam_hit(r);
    s_row_glow[r] = 1.0f;
    muse_state_poke();
}

static void show_bpm(void)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", (int)(jam_seq_bpm() + 0.5f));
    lv_label_set_text(s_bpm_lbl, buf);
}

static void show_beats_buttons(void)
{
    button_set(s_btn_play, jam_seq_playing() ? LV_SYMBOL_STOP : LV_SYMBOL_PLAY, jam_seq_playing() ? TAB_COLOR[1] : 0);
    button_set(s_btn_pattern, jam_seq_pattern_name(s_pattern), 0);
    button_set(s_btn_swing, "SWING", jam_seq_swing() ? TAB_COLOR[1] : 0);
    show_bpm();
}

static void on_beats_button(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *b = lv_event_get_current_target(e);
    int nudge = (int)(intptr_t)lv_event_get_user_data(e);   /* the BPM buttons' step */
    if (nudge && code == LV_EVENT_LONG_PRESSED_REPEAT) {
        jam_seq_set_bpm(jam_seq_bpm() + (float)(nudge * 4));
        show_bpm();
        return;
    }
    if (!clicked(e)) {
        return;
    }
    if (b == s_btn_play) {
        jam_seq_set_playing(!jam_seq_playing());
    } else if (b == s_btn_pattern) {
        s_pattern = (s_pattern + 1) % JAM_PATTERNS;
        jam_seq_pattern(s_pattern);
        style_grid();
    } else if (b == s_btn_swing) {
        jam_seq_set_swing(!jam_seq_swing());
    } else {
        jam_seq_set_bpm(jam_seq_bpm() + (float)nudge);
    }
    show_beats_buttons();
}

static void build_beats(lv_obj_t *page)
{
    for (int r = 0; r < JAM_ROWS; r++) {
        lv_obj_t *l = box(page, 4, PAD_Y + r * ROW_PITCH, 38, CELL_H, COLOR_BG, 6);
        lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(l, on_row, LV_EVENT_PRESSED, (void *)(intptr_t)r);
        lv_obj_t *t = text(l, &lv_font_unscii_8, ROW_COLOR[r], ROW_NAME[r]);
        lv_obj_center(t);
        s_row_lbl[r] = l;
    }
    s_grid_area = box(page, GRID_X, PAD_Y, GRID_W, JAM_ROWS * ROW_PITCH, COLOR_BG, 0);
    lv_obj_add_flag(s_grid_area, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_grid_area, on_grid, LV_EVENT_ALL, NULL);
    for (int r = 0; r < JAM_ROWS; r++) {
        for (int s = 0; s < JAM_STEPS; s++) {
            int x0 = s * GRID_W / JAM_STEPS, x1 = (s + 1) * GRID_W / JAM_STEPS;
            s_cells[r][s] = box(s_grid_area, x0 + 1, r * ROW_PITCH, x1 - x0 - 2, CELL_H, COLOR_CELL, 4);
        }
    }
    style_grid();

    int y = PAGE_H - 40;
    s_btn_play = button(page, 4, y, 44, 36, LV_SYMBOL_PLAY, on_beats_button, 0);
    button(page, 50, y, 34, 36, LV_SYMBOL_MINUS, on_beats_button, -1);
    lv_obj_t *bpm = box(page, 86, y, 60, 36, COLOR_BG, 8);
    s_bpm_lbl = text(bpm, &lv_font_montserrat_20, COLOR_TEXT, "");
    lv_obj_align(s_bpm_lbl, LV_ALIGN_CENTER, 0, -5);
    lv_obj_t *unit = text(bpm, &lv_font_unscii_8, COLOR_DIM, "BPM");
    lv_obj_align(unit, LV_ALIGN_BOTTOM_MID, 0, 0);
    button(page, 148, y, 34, 36, LV_SYMBOL_PLUS, on_beats_button, 1);
    s_btn_pattern = button(page, 184, y, 70, 36, "", on_beats_button, 0);
    s_btn_swing = button(page, 256, y, 60, 36, "", on_beats_button, 0);
    show_beats_buttons();
}

/* -- LISTEN ------------------------------------------------------------- */

#define BAR_BASE (PAGE_H - 50)
#define BAR_MAX 104

static lv_color_t band_color(int b)
{
    static const uint32_t STOPS[] = { 0xff4d6d, 0xffb02e, 0x9dff3d, 0x3de0ff, 0xb388ff };
    float x = (float)b / (JAM_BANDS - 1) * 4.0f;
    int i = (int)x;
    i = i > 3 ? 3 : i;
    return lv_color_mix(lv_color_hex(STOPS[i + 1]), lv_color_hex(STOPS[i]), (uint8_t)((x - (float)i) * 255.0f));
}

static void on_sync(lv_event_t *e)
{
    if (!clicked(e)) {
        return;
    }
    float tempo = jam_tempo();
    if (tempo <= 0.0f) {
        toast("NO BEAT YET");
        return;
    }
    /* Start early by the trip through the speaker, so ours lands on theirs. */
    float beat = 60.0f / tempo, at = jam_next_beat() - muse_jam_latency();
    while (at < 0.05f) {
        at += beat;
    }
    jam_seq_set_bpm(tempo);
    jam_seq_align(at);
    char buf[32];
    snprintf(buf, sizeof(buf), "LOCKED: %d BPM", (int)(tempo + 0.5f));
    toast(buf);
    show_beats_buttons();
}

static void build_listen(lv_obj_t *page)
{
    s_tempo_lbl = text(page, &lv_font_montserrat_28, COLOR_TEXT, "");
    lv_obj_set_pos(s_tempo_lbl, 10, 4);
    s_hint_lbl = text(page, &lv_font_montserrat_14, COLOR_DIM, "");
    lv_obj_set_pos(s_hint_lbl, 12, 38);
    s_beat_dot = box(page, W - 50, 6, 40, 40, COLOR_PANEL, LV_RADIUS_CIRCLE);
    for (int b = 0; b < JAM_BANDS; b++) {
        int x0 = 8 + b * (W - 16) / JAM_BANDS, x1 = 8 + (b + 1) * (W - 16) / JAM_BANDS;
        s_bars[b] = box(page, x0, BAR_BASE - 2, x1 - x0 - 3, 2, 0, 2);
        lv_obj_set_style_bg_color(s_bars[b], band_color(b), 0);
        s_bar_h[b] = 2;
    }
    s_btn_sync = button(page, 4, PAGE_H - 40, W - 8, 36, LV_SYMBOL_LOOP "  SYNC THE BEAT TO THE ROOM", on_sync, 0);
}

static void tick_listen(void)
{
    float bands[JAM_BANDS];
    jam_bands(bands);
    for (int b = 0; b < JAM_BANDS; b++) {
        int h = 2 + (int)(bands[b] * BAR_MAX);
        if (h != s_bar_h[b]) {
            s_bar_h[b] = h;
            lv_obj_set_height(s_bars[b], h);
            lv_obj_set_y(s_bars[b], BAR_BASE - h);
        }
    }
    int tempo = (int)(jam_tempo() + 0.5f);
    if (tempo != s_shown_tempo) {
        s_shown_tempo = tempo;
        char buf[24];
        if (tempo > 0) {
            snprintf(buf, sizeof(buf), "%d BPM", tempo);
        } else {
            strlcpy(buf, "--- BPM", sizeof(buf));
        }
        lv_label_set_text(s_tempo_lbl, buf);
        lv_label_set_text(s_hint_lbl, tempo > 0 ? "Hearing a beat. Tap SYNC to jam along."
                                                : "Play some music near Muse...");
        button_set(s_btn_sync, NULL, tempo > 0 ? TAB_COLOR[MUSE_JAM_LISTEN] : 0);
    }
    uint32_t beats = jam_beats();
    if (beats != s_seen_beats) {
        s_seen_beats = beats;
        s_beat_glow = tempo > 0 ? 1.0f : 0.0f;
    }
    s_beat_glow *= 0.72f;
    lv_obj_set_style_bg_color(s_beat_dot, shade(TAB_COLOR[MUSE_JAM_LISTEN], 0.15f + 0.85f * s_beat_glow), 0);
}

/* -- shared ------------------------------------------------------------- */

static void show_tab(int tab)
{
    s_tab = tab;
    for (int i = 0; i < MUSE_JAM_TABS; i++) {
        lv_obj_set_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN, i != tab);
        uint32_t c = i == MUSE_JAM_PLAY ? PRESET_COLOR[jam_preset()] : TAB_COLOR[i];
        button_set(s_tabs[i], NULL, i == tab ? c : 0);
    }
    if (tab == MUSE_JAM_BEATS) {
        style_grid();
        show_beats_buttons();
    }
    if (tab != MUSE_JAM_PLAY) {
        jam_set_expression(0.0f, 0.0f);
    }
    s_shown_tempo = -1;
}

static void on_tab(lv_event_t *e)
{
    if (clicked(e)) {
        show_tab((int)(intptr_t)lv_event_get_user_data(e));
    }
}

static void on_close(lv_event_t *e)
{
    if (clicked(e)) {
        muse_jam_ui_close();
    }
}

static float deadzone(float x, float dz)
{
    return x > dz ? x - dz : x < -dz ? x + dz : 0.0f;
}

/* Tilt left and right bends the pitch, towards and away brightens it, both
 * from however it was held when the jam opened. A shake remixes the beat. */
static void tick_motion(float now)
{
    float g[3];
    if (!muse_board->read_accel || muse_board->read_accel(g) != ESP_OK) {
        return;
    }
    if (!s_have_rest) {
        memcpy(s_rest, g, sizeof(s_rest));
        s_have_rest = true;
    }
    float mag = sqrtf(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
    if (fabsf(mag - 1.0f) > 0.9f && now - s_last_shake > 1.5f) {
        s_last_shake = now;
        if (!jam_seq_playing()) {
            jam_seq_set_playing(true);
        }
        jam_seq_remix((uint32_t)(now * 1000.0f));
        if (s_tab == MUSE_JAM_BEATS) {
            style_grid();
            show_beats_buttons();
        }
        toast(LV_SYMBOL_SHUFFLE "  REMIX!");
        return;
    }
    if (mag > 1.3f || mag < 0.7f) {
        return;   /* moving, not tilted */
    }
    float bend = deadzone(g[0] - s_rest[0], 0.08f) * 5.0f;
    float bright = deadzone(g[1] - s_rest[1], 0.08f) * 2.5f;
    bend = bend > 2.0f ? 2.0f : bend < -2.0f ? -2.0f : bend;
    bright = bright > 1.0f ? 1.0f : bright < -1.0f ? -1.0f : bright;
    s_bend += (bend - s_bend) * 0.5f;
    s_bright += (bright - s_bright) * 0.5f;
    if (s_tab == MUSE_JAM_PLAY) {
        jam_set_expression(s_bend, s_bright);
        lv_obj_set_pos(s_tilt_dot, 17 + (int)(s_bend * 8.0f), 7 - (int)(s_bright * 6.0f));
    }
}

static void on_tick(lv_timer_t *t)
{
    (void)t;
    float now = now_s();
    if (jam_seq_playing() || s_tab == MUSE_JAM_LISTEN) {
        muse_state_poke();   /* keep the screen up while it's making or hearing music */
    }
    tick_motion(now);
    lv_obj_set_flag(s_muted, LV_OBJ_FLAG_HIDDEN, muse_settings_speaker_on());
    if (s_toast_until && now > s_toast_until) {
        s_toast_until = 0.0f;
        lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    }

    switch (s_tab) {
    case MUSE_JAM_PLAY:
        for (int i = 0; i < JAM_PADS; i++) {
            s_glow[i] = i == s_pad_held ? 0.85f : s_glow[i] * 0.8f;
        }
        draw_pads();
        break;
    case MUSE_JAM_BEATS: {
        int step = jam_seq_step(), was = s_shown_step;
        if (step != was) {
            s_shown_step = step;
            for (int r = 0; r < JAM_ROWS; r++) {
                if (was >= 0) {
                    style_cell(r, was);
                }
                if (step >= 0) {
                    style_cell(r, step);
                    if (jam_seq_get(r, step)) {
                        s_row_glow[r] = 1.0f;
                    }
                }
            }
        }
        for (int r = 0; r < JAM_ROWS; r++) {
            lv_obj_set_style_bg_color(s_row_lbl[r], shade(ROW_COLOR[r], s_row_glow[r] * 0.6f), 0);
            s_row_glow[r] *= 0.7f;
        }
        break;
    }
    default:
        tick_listen();
        break;
    }
}

static void build(void)
{
    lv_obj_t *scr = lv_screen_active();
    s_root = box(scr, 0, 0, muse_board->width, muse_board->height, COLOR_BG, 0);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);   /* nothing under it takes a touch */
    lv_obj_t *stage = box(s_root, (muse_board->width - W) / 2, (muse_board->height - H) / 2, W, H, COLOR_BG, 0);

    for (int i = 0; i < MUSE_JAM_TABS; i++) {
        s_tabs[i] = button(stage, 4 + i * 70, 3, 66, 24, TAB_NAME[i], on_tab, i);
        s_pages[i] = box(stage, 0, TOP, W, PAGE_H, COLOR_BG, 0);
    }
    /* Where the tilt is, when there's an accelerometer to tilt. */
    s_tilt_box = box(stage, 218, 3, 44, 24, COLOR_PANEL, 8);
    s_tilt_dot = box(s_tilt_box, 17, 7, 10, 10, COLOR_TEXT, LV_RADIUS_CIRCLE);
    lv_obj_set_flag(s_tilt_box, LV_OBJ_FLAG_HIDDEN, !muse_board->read_accel);
    s_muted = text(stage, &lv_font_montserrat_16, 0xff4d6d, LV_SYMBOL_MUTE);
    lv_obj_set_pos(s_muted, 268, 6);
    button(stage, W - 34, 3, 30, 24, LV_SYMBOL_CLOSE, on_close, 0);

    build_play(s_pages[MUSE_JAM_PLAY]);
    build_beats(s_pages[MUSE_JAM_BEATS]);
    build_listen(s_pages[MUSE_JAM_LISTEN]);
    show_play_buttons();

    s_toast = text(stage, &lv_font_montserrat_20, COLOR_TEXT, "");
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_90, 0);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_radius(s_toast, 12, 0);
    lv_obj_set_style_pad_hor(s_toast, 16, 0);
    lv_obj_set_style_pad_ver(s_toast, 10, 0);
    lv_obj_set_style_border_width(s_toast, 2, 0);
    lv_obj_set_style_border_color(s_toast, lv_color_hex(0x3de0ff), 0);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);

    s_timer = lv_timer_create(on_tick, TICK_MS, NULL);
    lv_timer_pause(s_timer);
}

void muse_jam_ui_open(int tab)
{
    if (!muse_jam_start()) {
        muse_state_set_caption("NO MEMORY FOR JAM");
        return;
    }
    if (!s_root) {
        build();
    }
    s_have_rest = false;
    s_bend = s_bright = 0.0f;
    s_pad_held = -1;
    s_shown_step = -1;
    memset(s_drawn, 0xff, sizeof(s_drawn));
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_root);
    show_tab(tab >= 0 && tab < MUSE_JAM_TABS ? tab : MUSE_JAM_PLAY);
    show_play_buttons();
    lv_timer_resume(s_timer);
    muse_state_poke();
}

void muse_jam_ui_close(void)
{
    if (!s_root || lv_obj_has_flag(s_root, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    jam_pad_up();
    jam_seq_set_playing(false);
    jam_set_expression(0.0f, 0.0f);
    muse_jam_stop();
    lv_timer_pause(s_timer);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(lv_screen_active());
}

bool muse_jam_ui_is_open(void)
{
    return s_root && !lv_obj_has_flag(s_root, LV_OBJ_FLAG_HIDDEN);
}

static void on_jam_button(lv_event_t *e)
{
    if (clicked(e) && muse_state_mode(NULL) == MUSE_MODE_IDLE) {
        muse_jam_ui_open(MUSE_JAM_PLAY);
    }
}

lv_obj_t *muse_jam_ui_add_button(lv_obj_t *face, int x, int y)
{
    if (!muse_board->touch || muse_board->width < W || muse_board->height < H) {
        return NULL;   /* the jam needs a finger and the room for its pads */
    }
    lv_obj_t *b = box(face, 0, 0, 52, 52, 0x241d45, LV_RADIUS_CIRCLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(b, 10);
    lv_obj_add_event_cb(b, on_jam_button, LV_EVENT_ALL, NULL);
    lv_obj_t *l = text(b, &lv_font_montserrat_28, 0xff3dcb, LV_SYMBOL_AUDIO);
    lv_obj_center(l);
    lv_obj_align(b, LV_ALIGN_CENTER, x, y);
    return b;
}
