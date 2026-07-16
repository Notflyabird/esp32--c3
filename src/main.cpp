/*
 * ESP32-S3 N16R8 + INMP441 + ESP-SR
 *
 * INMP441: SCK=GPIO4, WS=GPIO5, SD=GPIO6, L/R=GND
 * Output:  GPIO10, active high
 * Flow:    AFE -> "Hi ESP" WakeNet -> Chinese/English MultiNet -> GPIO
 */

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s.h"
#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "model_path.h"

static const char *TAG = "VOICE_LIGHT";

#define I2S_PORT I2S_NUM_0
#define PIN_BCLK GPIO_NUM_4
#define PIN_WS GPIO_NUM_5
#define PIN_SD GPIO_NUM_6
#define LED_GPIO GPIO_NUM_10
#define SAMPLE_RATE 16000
#define MIC_SAMPLE_SHIFT 14
#define COMMAND_TIMEOUT_MS 6000

enum command_id_t {
    COMMAND_LIGHT_ON = 0,
    COMMAND_LIGHT_OFF = 1,
};

typedef struct {
    const char *language;
    const esp_mn_iface_t *iface;
    model_iface_data_t *data;
} multinet_model_t;

typedef struct {
    srmodel_list_t *models;
    const esp_afe_sr_iface_t *afe_iface;
    esp_afe_sr_data_t *afe_data;
    multinet_model_t chinese;
    multinet_model_t english;
} speech_context_t;

static speech_context_t s_speech = {};

static void *audio_alloc(size_t size)
{
    void *buffer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return buffer != NULL ? buffer : malloc(size);
}

static void i2s_init(void)
{
    i2s_config_t config = {};
    config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
    config.sample_rate = SAMPLE_RATE;
    config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
    config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
    config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    config.dma_desc_num = 6;
    config.dma_frame_num = 256;

    i2s_pin_config_t pins = {};
    pins.mck_io_num = I2S_PIN_NO_CHANGE;
    pins.bck_io_num = PIN_BCLK;
    pins.ws_io_num = PIN_WS;
    pins.data_out_num = I2S_PIN_NO_CHANGE;
    pins.data_in_num = PIN_SD;

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

static esp_err_t read_pcm_chunk(int32_t *raw, int16_t *pcm, int sample_count)
{
    const size_t wanted = (size_t)sample_count * sizeof(*raw);
    size_t total = 0;
    while (total < wanted) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_read(I2S_PORT, (uint8_t *)raw + total,
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

static bool add_command(esp_err_t err, const char *name)
{
    if (err == ESP_OK) {
        return true;
    }
    ESP_LOGE(TAG, "Invalid %s command: %s", name, esp_err_to_name(err));
    return false;
}

static bool configure_chinese_commands(multinet_model_t *model)
{
    ESP_ERROR_CHECK(esp_mn_commands_alloc(model->iface, model->data));
    bool ok = add_command(esp_mn_commands_add(COMMAND_LIGHT_ON, "da kai dian deng"), "Chinese light-on") &&
              add_command(esp_mn_commands_add(COMMAND_LIGHT_OFF, "guan bi dian deng"), "Chinese light-off");
    esp_mn_error_t *errors = ok ? esp_mn_commands_update() : NULL;
    ok = ok && errors == NULL;
    esp_mn_commands_free();
    return ok;
}

static bool configure_english_commands(multinet_model_t *model)
{
    ESP_ERROR_CHECK(esp_mn_commands_alloc(model->iface, model->data));
    bool ok = add_command(esp_mn_commands_phoneme_add(
                              COMMAND_LIGHT_ON, "turn on the light", "TkN nN jc LiT"),
                          "English light-on") &&
              add_command(esp_mn_commands_phoneme_add(
                              COMMAND_LIGHT_OFF, "turn off the light", "TkN eF jc LiT"),
                          "English light-off");
    esp_mn_error_t *errors = ok ? esp_mn_commands_update() : NULL;
    ok = ok && errors == NULL;
    esp_mn_commands_free();
    return ok;
}

static bool init_multinet(srmodel_list_t *models, const char *language_filter,
                          const char *language_name, multinet_model_t *model)
{
    char *model_name = esp_srmodel_filter(models, ESP_MN_PREFIX, language_filter);
    if (model_name == NULL) {
        ESP_LOGE(TAG, "%s MultiNet model is missing", language_name);
        return false;
    }

    model->language = language_name;
    model->iface = esp_mn_handle_from_name(model_name);
    model->data = model->iface != NULL ? model->iface->create(model_name, COMMAND_TIMEOUT_MS) : NULL;
    if (model->data == NULL) {
        ESP_LOGE(TAG, "Failed to create %s model %s", language_name, model_name);
        return false;
    }

    bool configured = strcmp(language_filter, ESP_MN_CHINESE) == 0
                          ? configure_chinese_commands(model)
                          : configure_english_commands(model);
    if (!configured) {
        ESP_LOGE(TAG, "Failed to configure %s commands", language_name);
        model->iface->destroy(model->data);
        model->data = NULL;
        return false;
    }

    ESP_LOGI(TAG, "%s commands ready: %s", language_name, model_name);
    return true;
}

static void feed_task(void *arg)
{
    speech_context_t *speech = (speech_context_t *)arg;
    const int chunk = speech->afe_iface->get_feed_chunksize(speech->afe_data);
    const int channels = speech->afe_iface->get_feed_channel_num(speech->afe_data);
    if (chunk <= 0 || channels != 1) {
        ESP_LOGE(TAG, "Unsupported AFE input: chunk=%d channels=%d", chunk, channels);
        vTaskDelete(NULL);
        return;
    }

    int32_t *raw = (int32_t *)audio_alloc((size_t)chunk * sizeof(*raw));
    int16_t *pcm = (int16_t *)audio_alloc((size_t)chunk * sizeof(*pcm));
    if (raw == NULL || pcm == NULL) {
        ESP_LOGE(TAG, "Unable to allocate AFE feed buffers");
        free(raw);
        free(pcm);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "AFE feed started: %d samples", chunk);
    while (true) {
        esp_err_t err = read_pcm_chunk(raw, pcm, chunk);
        if (err == ESP_OK) {
            speech->afe_iface->feed(speech->afe_data, pcm);
        } else {
            ESP_LOGW(TAG, "I2S read failed: %s", esp_err_to_name(err));
        }
    }
}

static int detect_command(multinet_model_t *model, int16_t *audio, bool *timed_out)
{
    esp_mn_state_t state = model->iface->detect(model->data, audio);
    if (state == ESP_MN_STATE_TIMEOUT) {
        *timed_out = true;
        return -1;
    }
    if (state != ESP_MN_STATE_DETECTED) {
        return -1;
    }

    esp_mn_results_t *results = model->iface->get_results(model->data);
    if (results == NULL || results->num <= 0) {
        return -1;
    }
    ESP_LOGI(TAG, "%s command=%d probability=%.3f",
             model->language, results->command_id[0], results->prob[0]);
    return results->command_id[0];
}

static void apply_light_command(int command)
{
    if (command == COMMAND_LIGHT_ON) {
        gpio_set_level(LED_GPIO, 1);
        ESP_LOGI(TAG, "Light ON");
    } else if (command == COMMAND_LIGHT_OFF) {
        gpio_set_level(LED_GPIO, 0);
        ESP_LOGI(TAG, "Light OFF");
    }
}

static void detect_task(void *arg)
{
    speech_context_t *speech = (speech_context_t *)arg;
    const int afe_chunk = speech->afe_iface->get_fetch_chunksize(speech->afe_data);
    const int cn_chunk = speech->chinese.iface->get_samp_chunksize(speech->chinese.data);
    const int en_chunk = speech->english.iface->get_samp_chunksize(speech->english.data);
    if (afe_chunk != cn_chunk || afe_chunk != en_chunk) {
        ESP_LOGE(TAG, "Chunk mismatch: AFE=%d CN=%d EN=%d", afe_chunk, cn_chunk, en_chunk);
        vTaskDelete(NULL);
        return;
    }

    bool command_session = false;
    bool cn_timed_out = false;
    bool en_timed_out = false;
    ESP_LOGI(TAG, "Say Hi ESP, then use a Chinese or English light command");

    while (true) {
        afe_fetch_result_t *result = speech->afe_iface->fetch(speech->afe_data);
        if (result == NULL || result->ret_value == ESP_FAIL) {
            ESP_LOGW(TAG, "AFE fetch failed");
            continue;
        }

        if (result->wakeup_state == WAKENET_DETECTED) {
            speech->chinese.iface->clean(speech->chinese.data);
            speech->english.iface->clean(speech->english.data);
            speech->afe_iface->disable_wakenet(speech->afe_data);
            command_session = true;
            cn_timed_out = false;
            en_timed_out = false;
            ESP_LOGI(TAG, "Wake word detected; listening for command");
        }

        if (!command_session) {
            continue;
        }

        int command = detect_command(&speech->chinese, result->data, &cn_timed_out);
        if (command < 0) {
            command = detect_command(&speech->english, result->data, &en_timed_out);
        }
        if (command >= 0) {
            apply_light_command(command);
            speech->chinese.iface->clean(speech->chinese.data);
            speech->english.iface->clean(speech->english.data);
            speech->afe_iface->enable_wakenet(speech->afe_data);
            command_session = false;
        } else if (cn_timed_out && en_timed_out) {
            speech->afe_iface->enable_wakenet(speech->afe_data);
            command_session = false;
            ESP_LOGI(TAG, "Command timeout; waiting for wake word");
        }
    }
}

extern "C" void app_main(void)
{
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO, 0);
    i2s_init();

    s_speech.models = esp_srmodel_init("model");
    if (s_speech.models == NULL) {
        ESP_LOGE(TAG, "Unable to load the model partition");
        return;
    }

    afe_config_t *afe_config = afe_config_init("M", s_speech.models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (afe_config == NULL) {
        ESP_LOGE(TAG, "Unable to create AFE configuration");
        return;
    }
    afe_config->aec_init = false;
    afe_config->se_init = false;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    s_speech.afe_iface = esp_afe_handle_from_config(afe_config);
    s_speech.afe_data = s_speech.afe_iface != NULL
                            ? s_speech.afe_iface->create_from_config(afe_config)
                            : NULL;
    afe_config_free(afe_config);

    if (s_speech.afe_data == NULL ||
        !init_multinet(s_speech.models, ESP_MN_CHINESE, "Chinese", &s_speech.chinese) ||
        !init_multinet(s_speech.models, ESP_MN_ENGLISH, "English", &s_speech.english)) {
        ESP_LOGE(TAG, "Speech pipeline initialization failed");
        return;
    }

    ESP_LOGI(TAG, "ESP32-S3 multilingual voice light control");
    ESP_LOGI(TAG, "INMP441 BCLK=4 WS=5 SD=6, output GPIO=10");
    s_speech.afe_iface->print_pipeline(s_speech.afe_data);

    if (xTaskCreatePinnedToCore(feed_task, "afe_feed", 8192, &s_speech, 5, NULL, 0) != pdPASS ||
        xTaskCreatePinnedToCore(detect_task, "sr_detect", 12288, &s_speech, 5, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create speech tasks");
    }
}
