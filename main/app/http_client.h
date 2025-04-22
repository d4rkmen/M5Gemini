#pragma once

#include <string>
#include <vector>
#include <sstream>
#include <map>
#include "hal.h"
// Add FreeRTOS includes
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/event_groups.h"

// Audio playback configuration
// #define AUDIO_SAMPLE_RATE 8000
// #define AUDIO_BITS_PER_SAMPLE 16
// #define AUDIO_CHANNELS 1
// #define BUFFER_TIME_SECONDS 0.5 // Minimum buffer time before starting playback (seconds)
// #define BUFFER_SIZE_SECONDS 1   // Buffer size (seconds)
// #define RINGBUF_SIZE (AUDIO_SAMPLE_RATE * (AUDIO_BITS_PER_SAMPLE / 8) * AUDIO_CHANNELS * BUFFER_SIZE_SECONDS)

struct gemini_context_t
{
    // input stream
    std::stringstream input_stream;
    // headers map
    // std::map<std::string, std::string> headers;
    std::string content_type;
    int32_t content_length;
};

struct elevenlabs_context_t
{
    // input stream
    std::stringstream input_stream;
    // headers map
    // std::map<std::string, std::string> headers;
    std::string content_type;
    int32_t content_length;
    // hal
    HAL::Hal* hal;
#if 0
    uint8_t leftover[2];
    bool has_leftover = false;
#endif
    // Buffered playback resources
    StaticRingbuffer_t ring_buffer;         // Ring buffer for audio data
    RingbufHandle_t audio_ring_buffer;      // Handle for the ring buffer
    EventGroupHandle_t control_event_group; // Handle for the main app's control event group
    TaskHandle_t playback_task_handle;      // Handle for the playback task

    // Statistics
    size_t total_bytes_received; // Total audio data received
    size_t total_bytes_played;   // Total audio data played
};

/**
 * @brief Call Google Gemini API
 *
 * @param model_name Model name to use
 * @param api_key Google API key
 * @param rules Rules to use
 * @param history History of messages
 * @param prompt User prompt to send to Gemini
 * @return std::string Response from Gemini API or error message
 */
std::string call_google_api(const std::string& model_name,
                            const std::string& api_key,
                            const std::string& rules,
                            const std::vector<std::pair<std::string, std::string>>& history,
                            const std::string& prompt);

/**
 * @brief Call Eleven Labs API
 *
 * @param api_key Eleven Labs API key
 * @param voice_id Eleven Labs voice ID
 * @param text Text to convert to speech
 * @return std::string Response from Eleven Labs API or error message
 */
std::string call_11labs_api(HAL::Hal* hal,
                            const std::string& api_key,
                            const std::string& voice_id,
                            const std::string& text,
                            const std::string& model_id,
                            EventGroupHandle_t control_event_group);
