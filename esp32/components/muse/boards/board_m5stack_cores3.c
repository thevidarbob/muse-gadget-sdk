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
 * M5Stack CoreS3: ESP32-S3 (16 MB flash, 8 MB quad PSRAM), 2" 320x240
 * ILI9342C IPS LCD on SPI with an FT6336U touch panel, AW88298 speaker amp
 * and ES7210 dual mic on one I2S bus, AXP2101 PMU, AW9523B GPIO expander and
 * a BMI270 IMU, all on one I2C bus. The only button Muse can use is the side
 * PWR key, which reaches the AXP2101 alone, so it's polled over I2C.
 *
 * Pins and init sequences are from M5Stack's own drivers: M5Unified
 * (src/M5Unified.inl for the I2S pins and the AW88298 / ES7210 setup,
 * src/utility/Power_Class.inl for the AXP2101 rails and the AW9523B boost)
 * and M5GFX (src/M5GFX.cpp, Panel_M5StackCoreS3 / Touch_M5StackCoreS3 /
 * Light_M5StackCoreS3 for the LCD, touch and backlight). xiaozhi-esp32's
 * boards/m5stack/core-s3 is a second ESP-IDF reference for the same parts.
 */
#include <math.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bmi270_config.h"
#include "muse_audio.h"
#include "muse_board.h"
#include "muse_lcd_bands.h"
#include "muse_mem.h"
#include "muse_pmu.h"

static const char *TAG = "board";

#define LCD_W 320
#define LCD_H 240
#define LCD_HOST SPI3_HOST
#define LCD_SCLK GPIO_NUM_36
#define LCD_MOSI GPIO_NUM_37
#define LCD_DC GPIO_NUM_35          /* the SD card's MISO too; Muse leaves the card idle */
#define LCD_CS GPIO_NUM_3
#define SD_CS GPIO_NUM_4            /* held high so the card never drives GPIO35 */
#define DRAW_BUF_LINES 60           /* four bands to the screen (muse_lcd_bands.h) */
#define LCD_CHUNK_BYTES (LCD_W * 8 * 2)

#define I2C_SDA GPIO_NUM_12
#define I2C_SCL GPIO_NUM_11
#define I2S_MCLK GPIO_NUM_0
#define I2S_BCLK GPIO_NUM_34
#define I2S_WS GPIO_NUM_33
#define I2S_DOUT GPIO_NUM_13        /* to the AW88298 */
#define I2S_DIN GPIO_NUM_14         /* from the ES7210 */

/* AW9523B: port 0 and port 1 outputs, directions, mode. */
#define AW9523_ADDR 0x58
#define AW_IN_P0 0x00
#define AW_IN_P1 0x01
#define AW_OUT_P0 0x02
#define AW_OUT_P1 0x03
#define AW_DIR_P0 0x04              /* 1 = input */
#define AW_DIR_P1 0x05
#define AW_GCR 0x11                 /* bit 4: port 0 push-pull */
#define AW_MODE_P0 0x12             /* 1 = GPIO, not LED current sink */
#define AW_MODE_P1 0x13
#define AW_P0_TOUCH_RST BIT(0)
#define AW_P0_SPK_RST BIT(2)        /* AW88298 reset, low = held */
#define AW_P1_CAM_RST BIT(0)
#define AW_P1_LCD_RST BIT(1)
#define AW_P1_BOOST_EN BIT(7)       /* SY7088 5 V boost, which M5Unified always enables */
#define SPK_RESET_PIN 2             /* the AW88298 driver's reset_pin: AW9523B P0_2 */

/* AXP2101 registers M5Unified sets on this board. */
#define AXP_DCDC_UVP 0x23           /* bits 0-4: power off on a DCDC dip */
#define AXP_LDO_ON0 0x90
#define AXP_ALDO1_V 0x92            /* 0.5 V + 0.1 V per step */
#define AXP_ALDO2_V 0x93
#define AXP_ALDO3_V 0x94
#define AXP_ALDO4_V 0x95
#define AXP_DLDO1_V 0x99            /* the LCD backlight */
#define AXP_CHGLED 0x69
#define AXP_LDO_DLDO1 BIT(7)
#define AXP_V(mv) (((mv) - 500) / 100)

/* BMI270, on its 0x69 address. */
#define BMI_ADDR 0x69
#define BMI_CHIP_ID 0x00
#define BMI_ACC_X 0x0C
#define BMI_INTERNAL_STATUS 0x21
#define BMI_ACC_CONF 0x40
#define BMI_ACC_RANGE 0x41
#define BMI_INIT_CTRL 0x59
#define BMI_INIT_ADDR_0 0x5B
#define BMI_INIT_DATA 0x5E
#define BMI_PWR_CONF 0x7C
#define BMI_PWR_CTRL 0x7D
#define BMI_CMD 0x7E
#define BMI_CHUNK 128

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_aw, s_imu;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_touch_handle_t s_tp;
static bool s_talk_down;
static bool s_imu_ok;

static esp_err_t i2c_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    const uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(dev, buf, sizeof(buf), 50);
}

static esp_err_t i2c_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(dev, &reg, 1, buf, n, 50);
}

static esp_err_t add_device(uint8_t addr, uint32_t hz, i2c_master_dev_handle_t *out)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = hz,
    };
    return i2c_master_bus_add_device(s_i2c, &cfg, out);
}

static esp_err_t aw_update(uint8_t reg, uint8_t mask, bool on)
{
    uint8_t v;
    ESP_RETURN_ON_ERROR(i2c_read(s_aw, reg, &v, 1), TAG, "aw9523 read %02x", reg);
    uint8_t want = on ? v | mask : v & ~mask;
    return want == v ? ESP_OK : i2c_write(s_aw, reg, want);
}

/* The AW88298 driver drives its reset line through this: it's on the expander. */
static int aw_gpio_setup(int16_t gpio, audio_gpio_dir_t dir, audio_gpio_mode_t mode)
{
    (void)gpio;
    (void)dir;
    (void)mode;
    return ESP_CODEC_DEV_OK;
}

static int aw_gpio_set(int16_t gpio, bool high)
{
    (void)gpio;
    return aw_update(AW_OUT_P0, AW_P0_SPK_RST, high) == ESP_OK ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_WRITE_FAIL;
}

static bool aw_gpio_get(int16_t gpio)
{
    (void)gpio;
    uint8_t v = 0;
    i2c_read(s_aw, AW_OUT_P0, &v, 1);
    return v & AW_P0_SPK_RST;
}

static const audio_codec_gpio_if_t s_aw_gpio = {
    .setup = aw_gpio_setup,
    .set = aw_gpio_set,
    .get = aw_gpio_get,
};

/* M5GFX's CoreS3 setup: touch, speaker, camera and LCD out of reset, the 5 V
 * boost on, the inputs (touch and amp interrupts, SD detect) as inputs. */
static esp_err_t expander_init(void)
{
    ESP_RETURN_ON_ERROR(i2c_write(s_aw, AW_OUT_P0, AW_P0_TOUCH_RST | AW_P0_SPK_RST), TAG, "aw9523 not responding");
    ESP_RETURN_ON_ERROR(i2c_write(s_aw, AW_OUT_P1, AW_P1_BOOST_EN | AW_P1_LCD_RST | AW_P1_CAM_RST), TAG, "aw p1");
    ESP_RETURN_ON_ERROR(i2c_write(s_aw, AW_DIR_P0, 0x18), TAG, "aw dir p0");
    ESP_RETURN_ON_ERROR(i2c_write(s_aw, AW_DIR_P1, 0x0C), TAG, "aw dir p1");
    ESP_RETURN_ON_ERROR(i2c_write(s_aw, AW_GCR, 0x10), TAG, "aw gcr");
    ESP_RETURN_ON_ERROR(i2c_write(s_aw, AW_MODE_P0, 0xFF), TAG, "aw mode p0");
    ESP_RETURN_ON_ERROR(i2c_write(s_aw, AW_MODE_P1, 0xFF), TAG, "aw mode p1");
    return ESP_OK;
}

/* The rails M5Unified sets: ALDO1 1.8 V for the amp, ALDO2 3.3 V for the
 * mics, ALDO3 and ALDO4 3.3 V for the camera and SD card, BLDO1-2 and DLDO1
 * (the backlight) on. A DCDC dip on a weak USB supply browns the chip out
 * rather than having the PMU switch everything off. */
static esp_err_t pmu_rails(void)
{
    uint8_t v;
    ESP_RETURN_ON_ERROR(muse_pmu_read(AXP_DCDC_UVP, &v), TAG, "uvp");
    ESP_RETURN_ON_ERROR(muse_pmu_write(AXP_DCDC_UVP, v & ~0x1F), TAG, "uvp off");
    ESP_RETURN_ON_ERROR(muse_pmu_write(AXP_LDO_ON0, 0xBF), TAG, "ldo on");
    ESP_RETURN_ON_ERROR(muse_pmu_write(AXP_ALDO1_V, AXP_V(1800)), TAG, "aldo1");
    ESP_RETURN_ON_ERROR(muse_pmu_write(AXP_ALDO2_V, AXP_V(3300)), TAG, "aldo2");
    ESP_RETURN_ON_ERROR(muse_pmu_write(AXP_ALDO3_V, AXP_V(3300)), TAG, "aldo3");
    ESP_RETURN_ON_ERROR(muse_pmu_write(AXP_ALDO4_V, AXP_V(3300)), TAG, "aldo4");
    ESP_RETURN_ON_ERROR(muse_pmu_write(AXP_CHGLED, 0x11), TAG, "charge led");
    return ESP_OK;
}

static esp_err_t imu_upload(void)
{
    for (size_t i = 0; i < bmi270_config_size; i += BMI_CHUNK) {
        /* INIT_ADDR counts 16-bit words: low nibble, then the rest. */
        uint8_t addr[3] = { BMI_INIT_ADDR_0, (uint8_t)((i >> 1) & 0x0F), (uint8_t)(i >> 5) };
        ESP_RETURN_ON_ERROR(i2c_master_transmit(s_imu, addr, sizeof(addr), 50), TAG, "init addr");
        uint8_t buf[1 + BMI_CHUNK];
        size_t n = bmi270_config_size - i < BMI_CHUNK ? bmi270_config_size - i : BMI_CHUNK;
        buf[0] = BMI_INIT_DATA;
        memcpy(buf + 1, bmi270_config_file + i, n);
        ESP_RETURN_ON_ERROR(i2c_master_transmit(s_imu, buf, n + 1, 100), TAG, "init data");
    }
    return ESP_OK;
}

/* Soft reset, firmware upload, then the accelerometer alone at 100 Hz, +-4 g,
 * as M5Unified brings it up (less the gyro and the BMM150, unused here). */
static esp_err_t imu_init(void)
{
    ESP_RETURN_ON_ERROR(add_device(BMI_ADDR, 400000, &s_imu), TAG, "imu");
    uint8_t id = 0;
    ESP_RETURN_ON_ERROR(i2c_read(s_imu, BMI_CHIP_ID, &id, 1), TAG, "BMI270 not responding");
    ESP_RETURN_ON_FALSE(id == 0x24, ESP_ERR_NOT_FOUND, TAG, "chip id %02x, not a BMI270", id);
    i2c_write(s_imu, BMI_CMD, 0xB6);
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(i2c_write(s_imu, BMI_PWR_CONF, 0x00), TAG, "power save off");
    vTaskDelay(pdMS_TO_TICKS(1));
    ESP_RETURN_ON_ERROR(i2c_write(s_imu, BMI_INIT_CTRL, 0x00), TAG, "init start");
    ESP_RETURN_ON_ERROR(imu_upload(), TAG, "config upload");
    ESP_RETURN_ON_ERROR(i2c_write(s_imu, BMI_INIT_CTRL, 0x01), TAG, "init done");
    uint8_t st = 0;
    for (int i = 0; i < 20 && !(st & 0x01); i++) {
        vTaskDelay(pdMS_TO_TICKS(5));
        i2c_read(s_imu, BMI_INTERNAL_STATUS, &st, 1);
    }
    ESP_RETURN_ON_FALSE(st & 0x01, ESP_FAIL, TAG, "BMI270 firmware didn't start (%02x)", st);
    ESP_RETURN_ON_ERROR(i2c_write(s_imu, BMI_ACC_CONF, 0xA8), TAG, "acc conf");
    ESP_RETURN_ON_ERROR(i2c_write(s_imu, BMI_ACC_RANGE, 0x01), TAG, "acc range");
    ESP_RETURN_ON_ERROR(i2c_write(s_imu, BMI_PWR_CTRL, 0x04), TAG, "acc on");
    return ESP_OK;
}

static esp_err_t init(void)
{
    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c), TAG, "i2c");
    ESP_RETURN_ON_ERROR(add_device(AW9523_ADDR, 400000, &s_aw), TAG, "aw9523");
    ESP_RETURN_ON_ERROR(expander_init(), TAG, "expander");

    /* The SD card shares the LCD's SPI pins and GPIO35; keep it deselected. */
    const gpio_config_t sd_cs = { .pin_bit_mask = BIT64(SD_CS), .mode = GPIO_MODE_OUTPUT };
    gpio_config(&sd_cs);
    gpio_set_level(SD_CS, 1);

    /* Key IRQs latch the PWR key's edges for poll_buttons(); its hold-off
     * time goes to 10 s, past the longest voice note. */
    ESP_RETURN_ON_ERROR(muse_pmu_init(s_i2c, true), TAG, "AXP2101");
    ESP_RETURN_ON_ERROR(pmu_rails(), TAG, "rails");
    vTaskDelay(pdMS_TO_TICKS(20));

    esp_err_t err = imu_init();
    s_imu_ok = err == ESP_OK;
    if (!s_imu_ok) {
        ESP_LOGW(TAG, "IMU unavailable (%s): Jam won't respond to tilt", esp_err_to_name(err));
    }
    return ESP_OK;
}

static void lcd_reset(void)
{
    aw_update(AW_OUT_P1, AW_P1_LCD_RST, false);
    vTaskDelay(pdMS_TO_TICKS(20));
    aw_update(AW_OUT_P1, AW_P1_LCD_RST, true);
    vTaskDelay(pdMS_TO_TICKS(120));
}

static void touch_reset(void)
{
    aw_update(AW_OUT_P0, AW_P0_TOUCH_RST, false);
    vTaskDelay(pdMS_TO_TICKS(10));
    aw_update(AW_OUT_P0, AW_P0_TOUCH_RST, true);
    vTaskDelay(pdMS_TO_TICKS(50));
}

/*
 * The ILI9342C takes the standard MIPI DCS set, so IDF's ST7789 driver runs
 * it: landscape as wired (M5GFX's rotation 1 on its offset 3 cancels out),
 * BGR, inverted. Its vendor power and gamma lists differ between the C and
 * the later E revision and are left at the panel's own defaults.
 */
static lv_display_t *display_start(lv_indev_t **touch)
{
    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_core_id = MUSE_UI_CORE;
    adapter_cfg.task_priority = MUSE_UI_PRIORITY;
    if (esp_lv_adapter_init(&adapter_cfg) != ESP_OK) {
        return NULL;
    }

    const spi_bus_config_t bus = {
        .sclk_io_num = LCD_SCLK,
        .mosi_io_num = LCD_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_CHUNK_BYTES,
    };
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        return NULL;
    }
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = LCD_CS,
        .dc_gpio_num = LCD_DC,
        .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    if (esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &s_io) != ESP_OK) {
        return NULL;
    }
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,      /* on the expander */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    if (esp_lcd_new_panel_st7789(s_io, &panel_cfg, &s_panel) != ESP_OK) {
        return NULL;
    }
    lcd_reset();
    esp_lcd_panel_reset(s_panel);           /* SWRESET */
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, true);
    esp_lcd_panel_disp_on_off(s_panel, true);

    const esp_lv_adapter_display_config_t disp_cfg = {
        .panel = s_panel,
        .panel_io = s_io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = LCD_W,
            .ver_res = LCD_H,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = muse_lcd_bands_register(disp_cfg, DRAW_BUF_LINES, LCD_CHUNK_BYTES);
    if (!disp) {
        return NULL;
    }

    /* FT6336U, polled: its interrupt reaches the chip only through the
     * expander, which latches it until read. */
    touch_reset();
    esp_lcd_panel_io_handle_t tp_io;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    tp_io_cfg.scl_speed_hz = 400000;
    if (esp_lcd_new_panel_io_i2c(s_i2c, &tp_io_cfg, &tp_io) != ESP_OK) {
        return NULL;
    }
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_W,
        .y_max = LCD_H,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
    };
    if (esp_lcd_touch_new_i2c_ft5x06(tp_io, &tp_cfg, &s_tp) != ESP_OK) {
        return NULL;
    }
    const esp_lv_adapter_touch_config_t touch_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, s_tp);
    *touch = esp_lv_adapter_register_touch(&touch_cfg);
    if (!*touch || esp_lv_adapter_start() != ESP_OK) {
        return NULL;
    }
    return disp;
}

static bool display_lock(int timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

/* The backlight is DLDO1, 2.5 V (dim) to 3.3 V (full), as M5GFX sets it. */
static void set_brightness(int pct)
{
    uint8_t ldo;
    if (muse_pmu_read(AXP_LDO_ON0, &ldo) != ESP_OK) {
        return;
    }
    if (pct <= 0) {
        muse_pmu_write(AXP_LDO_ON0, ldo & ~AXP_LDO_DLDO1);
        return;
    }
    muse_pmu_write(AXP_DLDO1_V, (uint8_t)(AXP_V(2500) + (pct * 8 + 50) / 100));
    if (!(ldo & AXP_LDO_DLDO1)) {
        muse_pmu_write(AXP_LDO_ON0, ldo | AXP_LDO_DLDO1);
    }
}

static void send_sleep(void *sleep)
{
    esp_lcd_panel_disp_sleep(s_panel, *(bool *)sleep);   /* SLPIN/SLPOUT; GRAM is kept */
}

static void panel_sleep(bool sleep)
{
    muse_lcd_bands_run(send_sleep, &sleep);
    vTaskDelay(pdMS_TO_TICKS(120));   /* SLPOUT needs this before the next command */
}

/* Screen off: LVGL stops and the FT6336U goes to hibernate (0xA5 = 3), which
 * only its reset line ends. */
static void display_pause(bool pause)
{
    if (pause) {
        esp_lv_adapter_pause(-1);
        esp_lcd_panel_io_tx_param(s_tp->io, 0xA5, (uint8_t[]){ 0x03 }, 1);
    } else {
        touch_reset();
        esp_lv_adapter_resume();
    }
}

/* One duplex I2S bus, clocked from MCLK: the AW88298 plays the left slot, the
 * ES7210 sends MIC1 and MIC2 in standard (not TDM) mode. */
static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    /* 60 ms of queue rather than the default 90: Muse Jam's pads answer a
     * finger sooner. Replies are read from the decoder's own buffer either way. */
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 160;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, &rx), TAG, "i2s channel");
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK,
            .bclk = I2S_BCLK,
            .ws = I2S_WS,
            .dout = I2S_DOUT,
            .din = I2S_DIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std_cfg), TAG, "i2s tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &std_cfg), TAG, "i2s rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "i2s tx on");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), TAG, "i2s rx on");

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t spk_i2c = { .port = I2C_NUM_0, .addr = AW88298_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c };
    audio_codec_i2c_cfg_t mic_i2c = { .port = I2C_NUM_0, .addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c };
    const audio_codec_ctrl_if_t *spk_ctrl = audio_codec_new_i2c_ctrl(&spk_i2c);
    const audio_codec_ctrl_if_t *mic_ctrl = audio_codec_new_i2c_ctrl(&mic_i2c);
    ESP_RETURN_ON_FALSE(data_if && spk_ctrl && mic_ctrl, ESP_ERR_NO_MEM, TAG, "codec interfaces");

    aw88298_codec_cfg_t aw_cfg = {
        .ctrl_if = spk_ctrl,
        .gpio_if = &s_aw_gpio,
        .reset_pin = SPK_RESET_PIN,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3, .pa_gain = 1 },
    };
    const audio_codec_if_t *spk_codec = aw88298_codec_new(&aw_cfg);
    ESP_RETURN_ON_FALSE(spk_codec, ESP_FAIL, TAG, "AW88298 not responding");
    es7210_codec_cfg_t es_cfg = {
        .ctrl_if = mic_ctrl,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
    };
    const audio_codec_if_t *mic_codec = es7210_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(mic_codec, ESP_FAIL, TAG, "ES7210 not responding");

    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = spk_codec, .data_if = data_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = mic_codec, .data_if = data_if };
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

static void set_mic_gain(esp_codec_dev_handle_t mic, int db)
{
    /* ES7210 PGA steps are 3 dB; snap so the UI shows what's applied. */
    db = (db / 3) * 3;
    /* esp_codec_dev rounds 33 dB down to 30; the next real step up is 34.5. */
    esp_codec_dev_set_in_gain(mic, db == 33 ? 34.5f : (float)db);
}

/* The PWR key reaches the AXP2101 only. Its latched press and release edges
 * come back as the talk button's; a click with no edges (both inside one
 * poll) still counts as a press and release. */
static unsigned poll_buttons(void)
{
    unsigned key = muse_pmu_poll_key();
    unsigned ev = 0;
    if (key & MUSE_PMU_KEY_PRESS) {
        ev |= MUSE_BTN_TALK_PRESS;
        s_talk_down = true;
    }
    if (key & MUSE_PMU_KEY_RELEASE) {
        ev |= MUSE_BTN_TALK_RELEASE;
        s_talk_down = false;
    }
    if ((key & MUSE_PMU_KEY_CLICK) && !ev) {
        ev = MUSE_BTN_TALK_PRESS | MUSE_BTN_TALK_RELEASE;
    }
    return ev;
}

static esp_err_t read_accel(float g[3])
{
    if (!s_imu_ok) {
        return ESP_ERR_INVALID_STATE;
    }
    int16_t raw[3];
    ESP_RETURN_ON_ERROR(i2c_read(s_imu, BMI_ACC_X, (uint8_t *)raw, sizeof(raw)), TAG, "accel");
    const float scale = 4.0f / 32768.0f;   /* +-4 g */
    /* The BMI270 sits with its x along the screen and its y down it. */
    g[0] = raw[0] * scale;
    g[1] = -raw[1] * scale;
    g[2] = raw[2] * scale;
    return ESP_OK;
}

static const muse_board_t s_board = {
    .name = "M5Stack CoreS3",
    .width = LCD_W,
    .height = LCD_H,
    .round = false,
    .touch = true,
    .diagonal_in = 2.0f,
    .talk_button = "power",
    /* The PWR key is on the left edge, near the top. */
    .talk_hint = { LV_ALIGN_LEFT_MID, 14, 24 },   /* the power key, on the left side */
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .display_pause = display_pause,
    .audio_init = audio_init,
    .mic_slot = -1,
    .set_mic_gain = set_mic_gain,
    .poll_buttons = poll_buttons,
    .read_power = muse_pmu_read_power,
    .power_off = muse_pmu_power_off,
    .read_accel = read_accel,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
