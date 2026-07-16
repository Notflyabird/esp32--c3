/*
 * ESP32-S3 + INMP441 + ESP-SR offline voice scoring for Dou Dizhu.
 *
 * INMP441 wiring:
 *   SCK/BCLK = GPIO4, WS/LRCLK = GPIO5, SD = GPIO6, L/R = GND
 *
 * Flow:
 *   single mic I2S -> AFE -> "Hi ESP" WakeNet -> Chinese MultiNet -> serial log
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

static const char *TAG = "DDZ_SCORE";

#define I2S_PORT I2S_NUM_0
#define PIN_BCLK GPIO_NUM_4
#define PIN_WS GPIO_NUM_5
#define PIN_SD GPIO_NUM_6
#define SAMPLE_RATE 16000
#define MIC_SAMPLE_SHIFT 14
#define COMMAND_TIMEOUT_MS 6000

typedef enum {
    CMD_P1_LANDLORD_WIN = 1,
    CMD_P2_LANDLORD_WIN,
    CMD_P3_LANDLORD_WIN,
    CMD_P1_LANDLORD_LOSE,
    CMD_P2_LANDLORD_LOSE,
    CMD_P3_LANDLORD_LOSE,
    CMD_QUERY_SCORE,
    CMD_RESET_SCORE,
} command_id_t;

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
static int s_score[3] = {0, 0, 0};

static int score_total(void)
{
    return s_score[0] + s_score[1] + s_score[2];
}

static void print_scores(const char *title)
{
    ESP_LOGI(TAG, "%s: 1号=%d, 2号=%d, 3号=%d, 总分=%d",
             title, s_score[0], s_score[1], s_score[2], score_total());
}

static void reset_scores(void)
{
    s_score[0] = 0;
    s_score[1] = 0;
    s_score[2] = 0;
    ESP_LOGI(TAG, "已重置所有分数");
    print_scores("重置后");
}

static void settle_round(int landlord, bool landlord_win)
{
    const int before[3] = {s_score[0], s_score[1], s_score[2]};
    const int landlord_delta = landlord_win ? 2 : -2;
    const int farmer_delta = landlord_win ? -1 : 1;
    const int landlord_index = landlord - 1;

    ESP_LOGI(TAG, "结算指令: %d号地主%s两分", landlord, landlord_win ? "赢" : "输");
    ESP_LOGI(TAG, "变更前: 1号=%d, 2号=%d, 3号=%d, 总分=%d",
             before[0], before[1], before[2], before[0] + before[1] + before[2]);

    for (int i = 0; i < 3; ++i) {
        s_score[i] += (i == landlord_index) ? landlord_delta : farmer_delta;
    }

    ESP_LOGI(TAG, "本局变化: 1号=%+d, 2号=%+d, 3号=%+d",
             s_score[0] - before[0], s_score[1] - before[1], s_score[2] - before[2]);
    print_scores("变更后");

    if (score_total() != 0) {
        ESP_LOGE(TAG, "总分校验失败，请检查计分逻辑");
    }
}

static void apply_score_command(int command)
{
    switch (command) {
    case CMD_P1_LANDLORD_WIN:
        settle_round(1, true);
        break;
    case CMD_P2_LANDLORD_WIN:
        settle_round(2, true);
        break;
    case CMD_P3_LANDLORD_WIN:
        settle_round(3, true);
        break;
    case CMD_P1_LANDLORD_LOSE:
        settle_round(1, false);
        break;
    case CMD_P2_LANDLORD_LOSE:
        settle_round(2, false);
        break;
    case CMD_P3_LANDLORD_LOSE:
        settle_round(3, false);
        break;
    case CMD_QUERY_SCORE:
        print_scores("查询分数");
        break;
    case CMD_RESET_SCORE:
        reset_scores();
        break;
    default:
        ESP_LOGW(TAG, "未知命令ID: %d", command);
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

static bool add_command_checked(esp_err_t err, const char *phrase)
{
    if (err == ESP_OK) {
        return true;
    }
    ESP_LOGE(TAG, "命令添加失败 [%s]: %s", phrase, esp_err_to_name(err));
    return false;
}

static bool configure_chinese_commands(multinet_model_t *model)
{
    ESP_ERROR_CHECK(esp_mn_commands_alloc(model->iface, model->data));

    bool ok =
        add_command_checked(esp_mn_commands_add(CMD_P1_LANDLORD_WIN, "yi hao di zhu ying liang fen"),
                            "一号地主赢两分") &&
        add_command_checked(esp_mn_commands_add(CMD_P2_LANDLORD_WIN, "er hao di zhu ying liang fen"),
                            "二号地主赢两分") &&
        add_command_checked(esp_mn_commands_add(CMD_P3_LANDLORD_WIN, "san hao di zhu ying liang fen"),
                            "三号地主赢两分") &&
        add_command_checked(esp_mn_commands_add(CMD_P1_LANDLORD_LOSE, "yi hao di zhu shu liang fen"),
                            "一号地主输两分") &&
        add_command_checked(esp_mn_commands_add(CMD_P2_LANDLORD_LOSE, "er hao di zhu shu liang fen"),
                            "二号地主输两分") &&
        add_command_checked(esp_mn_commands_add(CMD_P3_LANDLORD_LOSE, "san hao di zhu shu liang fen"),
                            "三号地主输两分") &&
        add_command_checked(esp_mn_commands_add(CMD_QUERY_SCORE, "cha xun fen shu"),
                            "查询分数") &&
        add_command_checked(esp_mn_commands_add(CMD_RESET_SCORE, "chong zhi suo you fen shu"),
                            "重置所有分数");

    esp_mn_error_t *errors = ok ? esp_mn_commands_update() : NULL;
    if (errors != NULL) {
        ESP_LOGE(TAG, "命令词更新失败，请检查拼音命令词");
        ok = false;
    }

    esp_mn_commands_free();
    return ok;
}

static bool init_chinese_multinet(srmodel_list_t *models, multinet_model_t *model)
{
    char *model_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_CHINESE);
    if (model_name == NULL) {
        ESP_LOGE(TAG, "缺少中文 MultiNet 模型");
        return false;
    }

    model->language = "Chinese";
    model->iface = esp_mn_handle_from_name(model_name);
    model->data = model->iface != NULL ? model->iface->create(model_name, COMMAND_TIMEOUT_MS) : NULL;
    if (model->data == NULL) {
        ESP_LOGE(TAG, "中文 MultiNet 创建失败: %s", model_name);
        return false;
    }

    if (!configure_chinese_commands(model)) {
        model->iface->destroy(model->data);
        model->data = NULL;
        return false;
    }

    ESP_LOGI(TAG, "中文命令已加载: %s", model_name);
    return true;
}

static void feed_task(void *arg)
{
    speech_context_t *speech = (speech_context_t *)arg;
    const int chunk = speech->afe_iface->get_feed_chunksize(speech->afe_data);
    const int channels = speech->afe_iface->get_feed_channel_num(speech->afe_data);

    if (chunk <= 0 || channels != 1) {
        ESP_LOGE(TAG, "不支持的 AFE 输入: chunk=%d channels=%d", chunk, channels);
        vTaskDelete(NULL);
        return;
    }

    int32_t *raw = (int32_t *)audio_alloc((size_t)chunk * sizeof(*raw));
    int16_t *pcm = (int16_t *)audio_alloc((size_t)chunk * sizeof(*pcm));
    if (raw == NULL || pcm == NULL) {
        ESP_LOGE(TAG, "AFE feed 缓冲区分配失败");
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
            ESP_LOGW(TAG, "I2S 读取失败: %s", esp_err_to_name(err));
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
        ESP_LOGE(TAG, "Chunk 不匹配: AFE=%d MN=%d", afe_chunk, mn_chunk);
        vTaskDelete(NULL);
        return;
    }

    bool command_session = false;
    bool timed_out = false;
    ESP_LOGI(TAG, "先说唤醒词 Hi ESP，再说斗地主计分命令");

    while (true) {
        afe_fetch_result_t *result = speech->afe_iface->fetch(speech->afe_data);
        if (result == NULL || result->ret_value == ESP_FAIL) {
            ESP_LOGW(TAG, "AFE fetch 失败");
            continue;
        }

        if (result->wakeup_state == WAKENET_DETECTED) {
            speech->chinese.iface->clean(speech->chinese.data);
            speech->afe_iface->disable_wakenet(speech->afe_data);
            command_session = true;
            timed_out = false;
            ESP_LOGI(TAG, "已唤醒，开始监听命令");
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
            ESP_LOGI(TAG, "命令超时，重新等待唤醒词");
        }
    }
}

void app_main(void)
{
    i2s_init();

    s_speech.models = esp_srmodel_init("model");
    if (s_speech.models == NULL) {
        ESP_LOGE(TAG, "模型分区加载失败");
        return;
    }

    afe_config_t *afe_config = afe_config_init("M", s_speech.models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (afe_config == NULL) {
        ESP_LOGE(TAG, "AFE 配置创建失败");
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
        ESP_LOGE(TAG, "语音识别初始化失败");
        return;
    }

    ESP_LOGI(TAG, "ESP32-S3 ESP-SR 斗地主计分器启动");
    ESP_LOGI(TAG, "单麦 INMP441: BCLK=%d WS=%d SD=%d", PIN_BCLK, PIN_WS, PIN_SD);
    print_scores("初始分数");
    s_speech.afe_iface->print_pipeline(s_speech.afe_data);

    if (xTaskCreatePinnedToCore(feed_task, "afe_feed", 8192, &s_speech, 5, NULL, 0) != pdPASS ||
        xTaskCreatePinnedToCore(detect_task, "sr_detect", 12288, &s_speech, 5, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "语音任务创建失败");
    }
}
