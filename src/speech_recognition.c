#include "speech_recognition.h"

#include <stdlib.h>

#include "app_config.h"
#include "audio_input.h"
#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_log.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "model_path.h"
#include "scorekeeper.h"

static const char *TAG = "DDZ_SR";

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
} speech_context_t;

static speech_context_t s_speech;

static bool configure_chinese_commands(multinet_model_t *model)
{
    ESP_ERROR_CHECK(esp_mn_commands_alloc(model->iface, model->data));

    bool ok = scorekeeper_register_commands();
    esp_mn_error_t *errors = ok ? esp_mn_commands_update() : NULL;
    if (errors != NULL) {
        ESP_LOGE(TAG, "Command update failed; check pinyin command words");
        ok = false;
    }

    esp_mn_commands_free();
    return ok;
}

static bool init_chinese_multinet(srmodel_list_t *models, multinet_model_t *model)
{
    char *model_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_CHINESE);
    if (model_name == NULL) {
        ESP_LOGE(TAG, "Chinese MultiNet model is missing");
        return false;
    }

    model->language = "Chinese";
    model->iface = esp_mn_handle_from_name(model_name);
    model->data = model->iface != NULL ? model->iface->create(model_name, APP_COMMAND_TIMEOUT_MS) : NULL;
    if (model->data == NULL) {
        ESP_LOGE(TAG, "Failed to create Chinese MultiNet: %s", model_name);
        return false;
    }

    if (!configure_chinese_commands(model)) {
        model->iface->destroy(model->data);
        model->data = NULL;
        return false;
    }

    ESP_LOGI(TAG, "Chinese commands ready: %s", model_name);
    return true;
}

bool speech_recognition_init(void)
{
    s_speech.models = esp_srmodel_init("model");
    if (s_speech.models == NULL) {
        ESP_LOGE(TAG, "Failed to load model partition");
        return false;
    }

    afe_config_t *afe_config = afe_config_init("M", s_speech.models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (afe_config == NULL) {
        ESP_LOGE(TAG, "Failed to create AFE config");
        return false;
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
        !init_chinese_multinet(s_speech.models, &s_speech.chinese)) {
        ESP_LOGE(TAG, "Speech recognition initialization failed");
        return false;
    }

    return true;
}

void speech_recognition_print_pipeline(void)
{
    if (s_speech.afe_iface != NULL && s_speech.afe_data != NULL) {
        s_speech.afe_iface->print_pipeline(s_speech.afe_data);
    }
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

    int32_t *raw = (int32_t *)audio_input_alloc((size_t)chunk * sizeof(*raw));
    int16_t *pcm = (int16_t *)audio_input_alloc((size_t)chunk * sizeof(*pcm));
    if (raw == NULL || pcm == NULL) {
        ESP_LOGE(TAG, "Failed to allocate AFE feed buffers");
        free(raw);
        free(pcm);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "AFE feed started: %d samples", chunk);
    while (true) {
        esp_err_t err = audio_input_read_pcm_chunk(raw, pcm, chunk);
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

static void detect_task(void *arg)
{
    speech_context_t *speech = (speech_context_t *)arg;
    const int afe_chunk = speech->afe_iface->get_fetch_chunksize(speech->afe_data);
    const int mn_chunk = speech->chinese.iface->get_samp_chunksize(speech->chinese.data);

    if (afe_chunk != mn_chunk) {
        ESP_LOGE(TAG, "Chunk mismatch: AFE=%d MN=%d", afe_chunk, mn_chunk);
        vTaskDelete(NULL);
        return;
    }

    bool command_session = false;
    bool timed_out = false;
    ESP_LOGI(TAG, "Say Hi ESP first, then say a Dou Dizhu scoring command");

    while (true) {
        afe_fetch_result_t *result = speech->afe_iface->fetch(speech->afe_data);
        if (result == NULL || result->ret_value == ESP_FAIL) {
            ESP_LOGW(TAG, "AFE fetch failed");
            continue;
        }

        if (result->wakeup_state == WAKENET_DETECTED) {
            speech->chinese.iface->clean(speech->chinese.data);
            speech->afe_iface->disable_wakenet(speech->afe_data);
            command_session = true;
            timed_out = false;
            ESP_LOGI(TAG, "Wake word detected; listening for command");
        }

        if (!command_session) {
            continue;
        }

        int command = detect_command(&speech->chinese, result->data, &timed_out);
        if (command >= 0) {
            scorekeeper_apply_command(command);
            speech->chinese.iface->clean(speech->chinese.data);
            speech->afe_iface->enable_wakenet(speech->afe_data);
            command_session = false;
        } else if (timed_out) {
            speech->afe_iface->enable_wakenet(speech->afe_data);
            command_session = false;
            ESP_LOGI(TAG, "Command timeout; waiting for wake word");
        }
    }
}

bool speech_recognition_start(void)
{
    return xTaskCreatePinnedToCore(feed_task, "afe_feed", 8192, &s_speech, 5, NULL, 0) == pdPASS &&
           xTaskCreatePinnedToCore(detect_task, "sr_detect", 12288, &s_speech, 5, NULL, 1) == pdPASS;
}

