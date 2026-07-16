#pragma once

#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

void audio_input_init(void);
void *audio_input_alloc(size_t size);
esp_err_t audio_input_read_pcm_chunk(int32_t *raw, int16_t *pcm, int sample_count);

