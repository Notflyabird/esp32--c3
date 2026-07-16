#pragma once

#include "driver/gpio.h"
#include "driver/i2s.h"

#define APP_I2S_PORT I2S_NUM_0
#define APP_PIN_BCLK GPIO_NUM_4
#define APP_PIN_WS GPIO_NUM_5
#define APP_PIN_SD GPIO_NUM_6

#define APP_SAMPLE_RATE 16000
#define APP_MIC_SAMPLE_SHIFT 14
#define APP_COMMAND_TIMEOUT_MS 6000

