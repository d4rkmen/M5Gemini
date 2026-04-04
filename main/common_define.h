/**
 * @file common_define.h
 * @brief Common macros and definitions shared across HAL and app code
 */
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>

#define delay(ms) vTaskDelay(pdMS_TO_TICKS(ms))
#define millis() (esp_timer_get_time() / 1000)

#define BOOT_ANIM_TIMEOUT 3000
