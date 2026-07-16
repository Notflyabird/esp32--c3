#include "app_config.h"
#include "audio_input.h"
#include "esp_log.h"
#include "scorekeeper.h"
#include "speech_recognition.h"

static const char *TAG = "DDZ_APP";

void app_main(void)
{
    audio_input_init();

    if (!speech_recognition_init()) {
        return;
    }

    ESP_LOGI(TAG, "ESP32-S3 ESP-SR Dou Dizhu scorekeeper started");
    ESP_LOGI(TAG, "Single mic INMP441: BCLK=%d WS=%d SD=%d", APP_PIN_BCLK, APP_PIN_WS, APP_PIN_SD);
    scorekeeper_print_scores("Initial");
    speech_recognition_print_pipeline();

    if (!speech_recognition_start()) {
        ESP_LOGE(TAG, "Failed to create speech tasks");
    }
}

