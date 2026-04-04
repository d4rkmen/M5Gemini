file(GLOB_RECURSE APP_SRCS
    ./app/*.c
    ./app/*.cpp
)

file(GLOB_RECURSE HAL_SRCS
    ./hal/*.c
    ./hal/*.cpp
)

file(GLOB_RECURSE SETTINGS_SRCS
    ./settings/*.c
    ./settings/*.cpp
)

idf_component_register(SRCS "main.cpp" ${APP_SRCS} ${HAL_SRCS} ${SETTINGS_SRCS}
                    INCLUDE_DIRS "." "./hal"
                    REQUIRES LovyanGFX nvs_flash esp_wifi esp_adc driver fatfs esp_http_client json
                    EMBED_FILES sound/error.wav sound/snap.wav
                    WHOLE_ARCHIVE)
                    
nvs_create_partition_image(apps_nvs ../apps_nvs.csv FLASH_IN_PROJECT)
