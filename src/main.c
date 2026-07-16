/*
 * ESP32-S3 + INMP441 + ESP-SR offline voice scorekeeper for Dou Dizhu.
 *
 * INMP441 wiring:
 *   SCK/BCLK = GPIO4, WS/LRCLK = GPIO5, SD = GPIO6, L/R = GND
 *
 * Flow:
 *   single mic I2S -> AFE -> "Hi ESP" WakeNet -> Chinese MultiNet -> serial log
 */

#include <limits.h>
#include <stdbool.h>
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

static const char *TAG = "DDZ_SCORE";

#define I2S_PORT I2S_NUM_0
#define PIN_BCLK GPIO_NUM_4
#define PIN_WS GPIO_NUM_5
#define PIN_SD GPIO_NUM_6
#define SAMPLE_RATE 16000
#define MIC_SAMPLE_SHIFT 14
#define COMMAND_TIMEOUT_MS 6000

#define CMD_QUERY_SCORE 1
#define CMD_RESET_SCORE 2
#define CMD_SCORE_BASE 100

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

typedef struct {
    const char *spoken;
    int points;
} point_phrase_t;

static speech_context_t s_speech;
static int s_score[3] = {0, 0, 0};

static const char *const PLAYER_PHRASES[3] = {
    "yi hao",
    "er hao",
    "san hao",
};

static const point_phrase_t POINT_PHRASES[] = {
    {"liang fen", 2},
    {"si fen", 4},
    {"liu fen", 6},
    {"ba fen", 8},
    {"shi fen", 10},
    {"shi er fen", 12},
    {"shi si fen", 14},
    {"shi liu fen", 16},
    {"shi ba fen", 18},
    {"er shi fen", 20},
};

static int score_total(void)
{
    return s_score[0] + s_score[1] + s_score[2];
}

static void print_scores(const char *title)
{
    ESP_LOGI(TAG, "%s: P1=%d, P2=%d, P3=%d, total=%d",
             title, s_score[0], s_score[1], s_score[2], score_total());
}

static void reset_scores(void)
{
    s_score[0] = 0;
    s_score[1] = 0;
    s_score[2] = 0;
    ESP_LOGI(TAG, "All scores reset");
    print_scores("After reset");
}

static int make_score_command_id(int player, bool landlord_win, int points)
{
    const int player_index = player - 1;
    const int outcome_index = landlord_win ? 0 : 1;
    const int point_index = points / 2 - 1;
    return CMD_SCORE_BASE + player_index * 20 + outcome_index * 10 + point_index;
}

static bool parse_score_command_id(int command, int *player, bool *landlord_win, int *points)
{
    int value = command - CMD_SCORE_BASE;
    if (value < 0 || value >= 60) {
        return false;
    }

    const int player_index = value / 20;
    value %= 20;
    const int outcome_index = value / 10;
    const int point_index = value % 10;

    *player = player_index + 1;
    *landlord_win = (outcome_index == 0);
    *points = (point_index + 1) * 2;
    return true;
}

static void settle_round(int landlord, bool landlord_win, int points)
{
    const int before[3] = {s_score[0], s_score[1], s_score[2]};
    const int landlord_delta = landlord_win ? points : -points;
    const int farmer_delta = landlord_win ? -(points / 2) : (points / 2);
    const int landlord_index = landlord - 1;

    ESP_LOGI(TAG, "Command: P%d landlord %s %d points",
             landlord, landlord_win ? "wins" : "loses", points);
    ESP_LOGI(TAG, "Before: P1=%d, P2=%d, P3=%d, total=%d",
             before[0], before[1], before[2], before[0] + before[1] + before[2]);

    for (int i = 0; i < 3; ++i) {
        s_score[i] += (i == landlord_index) ? landlord_delta : farmer_delta;
    }

    ESP_LOGI(TAG, "Delta: P1=%+d, P2=%+d, P3=%+d",
             s_score[0] - before[0], s_score[1] - before[1], s_score[2] - before[2]);
    print_scores("After");

    if (score_total() != 0) {
        ESP_LOGE(TAG, "Total check failed");
    }
}

static void apply_score_command(int command)
{
    int player = 0;
    int points = 0;
    bool landlord_win = false;

    if (parse_score_command_id(command, &player, &landlord_win, &points)) {
        settle_round(player, landlord_win, points);
        return;
    }

    switch (command) {
    case CMD_QUERY_SCORE:
        print_scores("Query");
        break;
    case CMD_RESET_SCORE:
        reset_scores();
        break;
    default:
        ESP_LOGW(TAG, "Unknown command id: %d", command);
        break;
    }
}

static void *audio_alloc(size_t size)
{
    void *buffer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return buffer != NULL ? buffer : malloc(size);
}

static void i2s_init(void)
{
    i2s_config_t config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_RX,
        .sample_rate = SAMPLE_RATE,
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
        .bck_io_num = PIN_BCLK,
        .ws_io_num = PIN_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = PIN_SD,
    };

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

static bool add_command_checked(esp_err_t err, int command_id, const char *phrase)
{
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Add command %d: %s", command_id, phrase);
        return true;
    }
    ESP_LOGE(TAG, "Failed to add command [%s]: %s", phrase, esp_err_to_name(err));
    return false;
}

static bool add_score_command(multinet_model_t *model, int player, bool landlord_win,
                              int points, const char *point_phrase)
{
    char command_phrase[64];
    const int command_id = make_score_command_id(player, landlord_win, points);

    snprintf(command_phrase, sizeof(command_phrase), "%s di zhu %s %s",
             PLAYER_PHRASES[player - 1], landlord_win ? "ying" : "shu", point_phrase);

    return add_command_checked(esp_mn_commands_add(command_id, command_phrase),
                               command_id, command_phrase);
}

static bool configure_chinese_commands(multinet_model_t *model)
{
    ESP_ERROR_CHECK(esp_mn_commands_alloc(model->iface, model->data));

    bool ok = true;
    for (int player = 1; player <= 3 && ok; ++player) {
        for (size_t i = 0; i < sizeof(POINT_PHRASES) / sizeof(POINT_PHRASES[0]) && ok; ++i) {
            ok = add_score_command(model, player, true,
                                   POINT_PHRASES[i].points, POINT_PHRASES[i].spoken);
            if (ok) {
                ok = add_score_command(model, player, false,
                                       POINT_PHRASES[i].points, POINT_PHRASES[i].spoken);
            }
        }
    }

    ok = ok &&
         add_command_checked(esp_mn_commands_add(CMD_QUERY_SCORE, "cha xun fen shu"),
                             CMD_QUERY_SCORE, "cha xun fen shu") &&
         add_command_checked(esp_mn_commands_add(CMD_RESET_SCORE, "chong zhi suo you fen shu"),
                             CMD_RESET_SCORE, "chong zhi suo you fen shu");

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
    model->data = model->iface != NULL ? model->iface->create(model_name, COMMAND_TIMEOUT_MS) : NULL;
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
        ESP_LOGE(TAG, "Failed to allocate AFE feed buffers");
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
            apply_score_command(command);
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

void app_main(void)
{
    i2s_init();

    s_speech.models = esp_srmodel_init("model");
    if (s_speech.models == NULL) {
        ESP_LOGE(TAG, "Failed to load model partition");
        return;
    }

    afe_config_t *afe_config = afe_config_init("M", s_speech.models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (afe_config == NULL) {
        ESP_LOGE(TAG, "Failed to create AFE config");
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
        !init_chinese_multinet(s_speech.models, &s_speech.chinese)) {
        ESP_LOGE(TAG, "Speech recognition initialization failed");
        return;
    }

    ESP_LOGI(TAG, "ESP32-S3 ESP-SR Dou Dizhu scorekeeper started");
    ESP_LOGI(TAG, "Single mic INMP441: BCLK=%d WS=%d SD=%d", PIN_BCLK, PIN_WS, PIN_SD);
    print_scores("Initial");
    s_speech.afe_iface->print_pipeline(s_speech.afe_data);

    if (xTaskCreatePinnedToCore(feed_task, "afe_feed", 8192, &s_speech, 5, NULL, 0) != pdPASS ||
        xTaskCreatePinnedToCore(detect_task, "sr_detect", 12288, &s_speech, 5, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create speech tasks");
    }
}
