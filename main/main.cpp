#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "sdkconfig.h"
#ifdef CONFIG_BOARD_STICKS3
#include "hal/hal_sticks3.h"
#else
#include "hal/hal_cardputer.h"
#endif
#include "app/gemini_app.h"

static const char* TAG = "M5Gemini";

using namespace HAL;
using namespace SETTINGS;

Settings settings;
#ifdef CONFIG_BOARD_STICKS3
HalStickS3 hal(&settings);
#else
HalCardputer hal(&settings);
#endif

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Starting M5Gemini application");
    settings.init();
    hal.init();
    settings.setHal(&hal);
    GeminiApp app(&hal);
    app.init();
    // Main loop
    while (1)
    {
        // Update the application
        app.update();
        // Provide some idle time for system tasks
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
