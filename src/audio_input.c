#include "audio_input.h"

#include <limits.h>
#include <stdlib.h>

#include "app_config.h"
#include "driver/i2s.h"
#include "esp_heap_caps.h"

void audio_input_init(void)
{
    i2s_config_t config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_RX,
        .sample_rate = APP_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 6,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0,
    };

    i2s_pin_config_t pins = {
        .mck_io_num = I2S_PIN_NO_CHANGE,
        .bck_io_num = APP_PIN_BCLK,
        .ws_io_num = APP_PIN_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = APP_PIN_SD,
    };

    ESP_ERROR_CHECK(i2s_driver_install(APP_I2S_PORT, &config, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(APP_I2S_PORT, &pins));
    ESP_ERROR_CHECK(i2s_zero_dma_buffer(APP_I2S_PORT));
}

void *audio_input_alloc(size_t size)
{
    void *buffer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return buffer != NULL ? buffer : malloc(size);
}

static int16_t sample_to_pcm16(int32_t sample)
{
    sample >>= APP_MIC_SAMPLE_SHIFT;
    if (sample > INT16_MAX) {
        return INT16_MAX;
    }
    if (sample < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)sample;
}

esp_err_t audio_input_read_pcm_chunk(int32_t *raw, int16_t *pcm, int sample_count)
{
    const size_t wanted = (size_t)sample_count * sizeof(*raw);
    size_t total = 0;

    while (total < wanted) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_read(APP_I2S_PORT, (uint8_t *)raw + total,
                                 wanted - total, &bytes_read, portMAX_DELAY);
        if (err != ESP_OK) {
            return err;
        }
        if (bytes_read == 0) {
            return ESP_ERR_INVALID_SIZE;
        }
        total += bytes_read;
    }

    for (int i = 0; i < sample_count; ++i) {
        pcm[i] = sample_to_pcm16(raw[i]);
    }
    return ESP_OK;
}

