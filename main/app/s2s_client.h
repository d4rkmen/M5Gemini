#pragma once

#include "hal/hal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include <string>

// S2S Event bits
#define S2S_STOP_REQUEST_BIT      (1 << 0)
#define S2S_CONNECTED_BIT         (1 << 1)
#define S2S_ERROR_BIT             (1 << 2)
#define S2S_SETUP_COMPLETE_BIT    (1 << 3)
#define S2S_TASK_STARTED_BIT      (1 << 4)
#define S2S_TASK_STOPPED_BIT      (1 << 5)
#define S2S_LISTENING_BIT         (1 << 6)
#define S2S_MODEL_SPEAKING_BIT    (1 << 7)
#define S2S_TURN_COMPLETE_BIT     (1 << 8)
#define S2S_INTERRUPTED_BIT       (1 << 9)
#define S2S_INPUT_TRANSCRIPT_BIT  (1 << 10)
#define S2S_OUTPUT_TRANSCRIPT_BIT (1 << 11)
#define S2S_USER_INTERRUPT_BIT    (1 << 12)
#define S2S_FATAL_ERROR_BIT       (1 << 13)

#define S2S_MIC_SAMPLE_RATE  16000
#define S2S_SPK_SAMPLE_RATE  24000

struct S2SSharedData
{
    SemaphoreHandle_t mutex;
    std::string input_transcript;
    std::string output_transcript;
    std::string session_handle;
    std::string error_message;
};

bool s2s_start(HAL::Hal* hal, EventGroupHandle_t event_group, S2SSharedData* shared,
               const std::string& api_key, const std::string& model,
               const std::string& voice, const std::string& rules,
               int32_t volume);

void s2s_stop(void);
