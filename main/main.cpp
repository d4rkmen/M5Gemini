#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "hal/hal_cardputer.h"
#include "app/gemini_app.h"

static const char* TAG = "M5Gemini";

using namespace HAL;
#ifdef HAVE_SETTINGS
using namespace SETTINGS;
#endif

#ifdef HAVE_SETTINGS
Settings settings;
#endif
HalCardputer hal(&settings);

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Starting M5Gemini application");
    settings.init();
    hal.init();
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