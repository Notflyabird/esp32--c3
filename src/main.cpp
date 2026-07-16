#include <Arduino.h>
#include "driver/i2s.h"

/*
  ESP32-C3 + INMP441 I2S digital microphone test

  Default wiring:
    INMP441 VDD -> 3V3
    INMP441 GND -> GND
    INMP441 SCK -> GPIO4
    INMP441 WS  -> GPIO5
    INMP441 SD  -> GPIO6
    INMP441 L/R -> GND  (left channel)

  If L/R is connected to 3V3, change I2S_CHANNEL_FMT_ONLY_LEFT to
  I2S_CHANNEL_FMT_ONLY_RIGHT in i2s_config.

  Test method:
    1. Open serial monitor at 115200 baud.
    2. Keep quiet for 3-5 seconds and watch noise dBFS.
    3. Speak near the microphone or clap once and watch peak/rms changes.

  Notes:
    - INMP441 sensitivity cannot be measured as absolute dB SPL without a
      calibrated sound source. This sketch reports relative sensitivity in dBFS.
    - A good microphone should show low stable noise when quiet and a clear
      increase when speaking or clapping.
*/

static constexpr i2s_port_t I2S_PORT = I2S_NUM_0;

static constexpr gpio_num_t PIN_I2S_BCLK = GPIO_NUM_4;
static constexpr gpio_num_t PIN_I2S_WS = GPIO_NUM_5;
static constexpr gpio_num_t PIN_I2S_DATA_IN = GPIO_NUM_6;

static constexpr uint32_t SAMPLE_RATE_HZ = 16000;
static constexpr size_t READ_SAMPLES = 512;
static constexpr uint32_t REPORT_INTERVAL_MS = 1000;

// 24-bit INMP441 samples are delivered left-aligned in a 32-bit I2S word.
static constexpr float PCM24_FULL_SCALE = 8388608.0f;

static int32_t raw_buffer[READ_SAMPLES];

static float dbfsFromRms(float rms)
{
  if (rms <= 1.0f) {
    return -120.0f;
  }
  return 20.0f * log10f(rms / PCM24_FULL_SCALE);
}

static void installI2S()
{
  const i2s_config_t i2s_config = {
      .mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX),
      .sample_rate = SAMPLE_RATE_HZ,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
      .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = 4,
      .dma_buf_len = 256,
      .use_apll = false,
      .tx_desc_auto_clear = false,
      .fixed_mclk = 0,
  };

  const i2s_pin_config_t pin_config = {
      .bck_io_num = PIN_I2S_BCLK,
      .ws_io_num = PIN_I2S_WS,
      .data_out_num = I2S_PIN_NO_CHANGE,
      .data_in_num = PIN_I2S_DATA_IN,
  };

  ESP_ERROR_CHECK(i2s_driver_install(I2S_PORT, &i2s_config, 0, nullptr));
  ESP_ERROR_CHECK(i2s_set_pin(I2S_PORT, &pin_config));
  ESP_ERROR_CHECK(i2s_zero_dma_buffer(I2S_PORT));
}

static const char *qualityText(float quietDbfs, float activeDbfs, float peakDbfs, uint32_t nonZeroSamples)
{
  const float responseDb = activeDbfs - quietDbfs;

  if (nonZeroSamples == 0) {
    return "BAD: no data, check SD/BCLK/WS wiring and power";
  }
  if (peakDbfs > -1.0f) {
    return "WARN: clipping, sound too loud or bit alignment/wiring is wrong";
  }
  if (quietDbfs > -20.0f) {
    return "WARN: noise is very high, check power/GND and wiring";
  }
  if (responseDb >= 12.0f) {
    return "OK: clear acoustic response";
  }
  if (responseDb >= 6.0f) {
    return "WEAK: response is small, speak closer or check mic port direction";
  }
  return "WAIT: keep quiet first, then speak/clap near the microphone";
}

void setup()
{
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("ESP32-C3 INMP441 microphone test");
  Serial.println("Pins: BCLK=GPIO4, WS=GPIO5, SD=GPIO6, L/R=GND(left)");
  Serial.println("Output: rms_dbfs, peak_dbfs, quiet_floor, response_delta, verdict");
  Serial.println();

  installI2S();
}

void loop()
{
  static uint32_t last_report_ms = 0;
  static double sum_squares = 0.0;
  static uint32_t sample_count = 0;
  static uint32_t non_zero_count = 0;
  static int32_t peak_abs = 0;
  static float quiet_floor_dbfs = 0.0f;
  static bool quiet_floor_ready = false;

  size_t bytes_read = 0;
  const esp_err_t err = i2s_read(
      I2S_PORT,
      raw_buffer,
      sizeof(raw_buffer),
      &bytes_read,
      pdMS_TO_TICKS(100));

  if (err != ESP_OK || bytes_read == 0) {
    Serial.printf("I2S read error: %d, bytes=%u\n", static_cast<int>(err), static_cast<unsigned>(bytes_read));
    delay(500);
    return;
  }

  const size_t samples_read = bytes_read / sizeof(raw_buffer[0]);
  for (size_t i = 0; i < samples_read; ++i) {
    const int32_t sample24 = raw_buffer[i] >> 8;
    const int32_t abs_sample = abs(sample24);

    sum_squares += static_cast<double>(sample24) * static_cast<double>(sample24);
    sample_count++;

    if (sample24 != 0) {
      non_zero_count++;
    }
    if (abs_sample > peak_abs) {
      peak_abs = abs_sample;
    }
  }

  const uint32_t now = millis();
  if (now - last_report_ms < REPORT_INTERVAL_MS || sample_count == 0) {
    return;
  }
  last_report_ms = now;

  const float rms = sqrt(sum_squares / sample_count);
  const float rms_dbfs = dbfsFromRms(rms);
  const float peak_dbfs = dbfsFromRms(static_cast<float>(peak_abs));

  if (!quiet_floor_ready) {
    quiet_floor_dbfs = rms_dbfs;
    quiet_floor_ready = true;
  } else if (rms_dbfs < quiet_floor_dbfs) {
    quiet_floor_dbfs = (quiet_floor_dbfs * 0.8f) + (rms_dbfs * 0.2f);
  }

  const float response_delta_db = rms_dbfs - quiet_floor_dbfs;
  const char *verdict = qualityText(quiet_floor_dbfs, rms_dbfs, peak_dbfs, non_zero_count);

  Serial.printf(
      "rms=%8.0f  rms_dbfs=%7.1f  peak_dbfs=%7.1f  quiet=%7.1f  delta=%6.1f  nonzero=%lu/%lu  %s\n",
      rms,
      rms_dbfs,
      peak_dbfs,
      quiet_floor_dbfs,
      response_delta_db,
      static_cast<unsigned long>(non_zero_count),
      static_cast<unsigned long>(sample_count),
      verdict);

  sum_squares = 0.0;
  sample_count = 0;
  non_zero_count = 0;
  peak_abs = 0;
}
