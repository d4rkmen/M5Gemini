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
                    REQUIRES M5Unified M5GFX nvs_flash esp_wifi driver fatfs esp_http_client json
#                    EMBED_TXTFILES google_root_cert.pem elevenlabs_root_cert.pem
                    EMBED_FILES sound/error.wav sound/snap.wav
                    WHOLE_ARCHIVE)
                    
nvs_create_partition_image(apps_nvs ../apps_nvs.csv FLASH_IN_PROJECT)