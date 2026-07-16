/*
 * ESP32-C3 + INMP441 + ESP-SR WakeNet9s
 *
 * INMP441 wiring:
 *   SCK -> GPIO4, WS -> GPIO5, SD -> GPIO6, L/R -> GND
 * Wake word: "Hi ESP" (wn9s_hiesp)
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "model_path.h"

static const char *TAG = "WAKE";

#define I2S_PORT I2S_NUM_0
#define PIN_BCLK GPIO_NUM_4
#define PIN_WS GPIO_NUM_5
#define PIN_SD GPIO_NUM_6
#define SAMPLE_RATE 16000

#define LED_GPIO GPIO_NUM_10
#define WAKE_LOCKOUT_MS 1000

// INMP441 outputs 24-bit samples left-aligned in a 32-bit I2S slot.
// Shifting by 14 converts to 16-bit PCM while adding modest input gain.
#define MIC_SAMPLE_SHIFT 14

static void i2s_init(void)
{
    i2s_config_t config = {};
    config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
    config.sample_rate = SAMPLE_RATE;
    config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
    config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
    config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    config.dma_desc_num = 4;
    config.dma_frame_num = 256;

    i2s_pin_config_t pins = {};
    pins.bck_io_num = PIN_BCLK;
    pins.ws_io_num = PIN_WS;
    pins.data_out_num = I2S_PIN_NO_CHANGE;
    pins.data_in_num = PIN_SD;
    pins.mck_io_num = I2S_PIN_NO_CHANGE;

    ESP_ERROR_CHECK(i2s_driver_install(I2S_PORT, &config, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_PORT, &pins));
    ESP_ERROR_CHECK(i2s_zero_dma_buffer(I2S_PORT));
}

static int16_t sample_to_pcm16(int32_t sample)
{
    sample >>= MIC_SAMPLE_SHIFT;
    if (sample > INT16_MAX) {
        return INT16_MAX;
    }
    if (sample < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)sample;
}

static esp_err_t read_pcm_chunk(int32_t *i2s_buffer, int16_t *pcm_buffer, int sample_count)
{
    const size_t requested_bytes = (size_t)sample_count * sizeof(*i2s_buffer);
    size_t total_bytes = 0;

    while (total_bytes < requested_bytes) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_read(I2S_PORT,
                                 (uint8_t *)i2s_buffer + total_bytes,
                                 requested_bytes - total_bytes,
                                 &bytes_read,
                                 portMAX_DELAY);
        if (err != ESP_OK) {
            return err;
        }
        if (bytes_read == 0) {
            return ESP_ERR_INVALID_SIZE;
        }
        total_bytes += bytes_read;
    }

    for (int i = 0; i < sample_count; ++i) {
        pcm_buffer[i] = sample_to_pcm16(i2s_buffer[i]);
    }
    return ESP_OK;
}

static void detection_task(void *arg)
{
    (void)arg;

    srmodel_list_t *models = esp_srmodel_init("model");
    if (models == NULL) {
        ESP_LOGE(TAG, "Unable to load models from the 'model' partition");
        vTaskDelete(NULL);
        return;
    }

    char *model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, "hiesp");
    if (model_name == NULL || strstr(model_name, "wn9s_hiesp") == NULL) {
        ESP_LOGE(TAG, "WakeNet9s Hi ESP model is not present in the model partition");
        esp_srmodel_deinit(models);
        vTaskDelete(NULL);
        return;
    }

    const esp_wn_iface_t *wakenet = esp_wn_handle_from_name(model_name);
    if (wakenet == NULL) {
        ESP_LOGE(TAG, "No WakeNet interface for model %s", model_name);
        esp_srmodel_deinit(models);
        vTaskDelete(NULL);
        return;
    }

    model_iface_data_t *model_data = wakenet->create(model_name, DET_MODE_95);
    if (model_data == NULL) {
        ESP_LOGE(TAG, "Failed to create WakeNet model %s", model_name);
        esp_srmodel_deinit(models);
        vTaskDelete(NULL);
        return;
    }

    const int chunk_samples = wakenet->get_samp_chunksize(model_data);
    if (chunk_samples <= 0) {
        ESP_LOGE(TAG, "Invalid WakeNet chunk size: %d", chunk_samples);
        wakenet->destroy(model_data);
        esp_srmodel_deinit(models);
        vTaskDelete(NULL);
        return;
    }

    int16_t *pcm_buffer = (int16_t *)malloc((size_t)chunk_samples * sizeof(*pcm_buffer));
    int32_t *i2s_buffer = (int32_t *)malloc((size_t)chunk_samples * sizeof(*i2s_buffer));
    if (pcm_buffer == NULL || i2s_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate audio buffers (chunk=%d)", chunk_samples);
        free(i2s_buffer);
        free(pcm_buffer);
        wakenet->destroy(model_data);
        esp_srmodel_deinit(models);
        vTaskDelete(NULL);
        return;
    }

    i2s_init();
    ESP_LOGI(TAG, "Model %s ready, chunk=%d samples", model_name, chunk_samples);
    ESP_LOGI(TAG, "Listening for \"Hi ESP\"");

    int64_t last_wake_us = 0;
    while (true) {
        esp_err_t err = read_pcm_chunk(i2s_buffer, pcm_buffer, chunk_samples);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "I2S read failed: %s", esp_err_to_name(err));
            continue;
        }

        wakenet_state_t state = wakenet->detect(model_data, pcm_buffer);
        if (state != WAKENET_DETECTED) {
            continue;
        }

        const int64_t now = esp_timer_get_time();
        if (now - last_wake_us <= WAKE_LOCKOUT_MS * 1000LL) {
            continue;
        }
        last_wake_us = now;

        gpio_set_level(LED_GPIO, 1);
        ESP_LOGI(TAG, "Hi ESP detected");
        vTaskDelay(pdMS_TO_TICKS(150));
        gpio_set_level(LED_GPIO, 0);
    }
}

extern "C" void app_main(void)
{
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO, 0);

    ESP_LOGI(TAG, "ESP32-C3 WakeNet9s direct inference");
    ESP_LOGI(TAG, "INMP441: BCLK=GPIO4 WS=GPIO5 SD=GPIO6");

    BaseType_t created = xTaskCreatePinnedToCore(
        detection_task, "wakenet", 8192, NULL, 5, NULL, 0);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create WakeNet task");
    }
}
