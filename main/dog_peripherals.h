#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef WS2812_NUM_LEDS // Cheap way to detect dog board for now

void dog_peripherals_init(void);
void dog_audio_play_chunk(const uint8_t *data, size_t size);
void dog_audio_play_async(uint8_t *data, size_t size); // takes ownership of malloc'd data
void dog_audio_play_tone(void);

#endif
