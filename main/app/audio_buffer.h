#pragma once

#include <stdint.h>

#define SYSTEM_CHANNEL 0
#define AUDIO_CHANNEL 1
#define AUDIO_BITS_PER_SAMPLE 16
#define AUDIO_CHANNELS 1

// Shared static buffer used by the S2S speaker ring buffer.
// Sized for ~0.67s of 24kHz 16-bit mono output.
// (portMAX_DELAY backpressure prevents data loss even with smaller buffer)
#define AUDIO_BUFFER_SIZE (1024 * 32)

extern uint8_t audio_buffer[AUDIO_BUFFER_SIZE];
