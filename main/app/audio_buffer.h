#pragma once

#include <stdint.h>

#define AUDIO_SAMPLE_RATE 8000
#define AUDIO_BITS_PER_SAMPLE 16
#define AUDIO_CHANNELS 1
#define BUFFER_TIME_SECONDS 0.5 // Minimum buffer time before starting playback (seconds)
#define BUFFER_SIZE_SECONDS 2   // Buffer size (seconds)
#define RINGBUF_SIZE (AUDIO_SAMPLE_RATE * (AUDIO_BITS_PER_SAMPLE / 8) * AUDIO_CHANNELS * BUFFER_SIZE_SECONDS)

extern uint8_t audio_buffer[RINGBUF_SIZE];
