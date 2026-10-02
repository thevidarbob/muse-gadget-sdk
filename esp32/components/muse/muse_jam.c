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

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_jam_synth.h"
#include "muse_mem.h"

static const char *TAG = "muse_jam";

static void *s_mem;
static volatile bool s_active;

bool muse_jam_start(void)
{
    if (!s_mem) {
        s_mem = heap_caps_calloc(1, jam_state_size(), MUSE_BIG_CAPS);
        if (!s_mem) {
            ESP_LOGE(TAG, "no memory for the jam engine (%u bytes)", (unsigned)jam_state_size());
            return false;
        }
        jam_init(s_mem);
        ESP_LOGI(TAG, "jam engine up: %u bytes", (unsigned)jam_state_size());
    } else {
        jam_panic();   /* no notes left hanging from last time */
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

void muse_jam_audio_tick(int16_t *mic)
{
    static int16_t out[MUSE_AUDIO_CHUNK];
    jam_render(out, MUSE_AUDIO_CHUNK);
    if (muse_audio_write(out, MUSE_AUDIO_CHUNK) != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(20));
        return;
    }
    if (muse_audio_read(mic, MUSE_AUDIO_CHUNK) == ESP_OK) {
        jam_listen(mic, MUSE_AUDIO_CHUNK);
    }
}

float muse_jam_latency(void)
{
    /* Estimated, not measured: the I2S queue out (60 ms on the CoreS3) and
     * about a chunk each way. The listener allows for its own window. */
    return 0.09f;
}
