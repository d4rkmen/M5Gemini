#include <stdio.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/ringbuf.h"

#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_event.h"
#include <cJSON.h>
#include "hal_cardputer.h"
#include "ws_client.h"
#include "audio_buffer.h"
#include "event_bits.h"
#include <sstream>
#include <format>

#define MIC_BUFFER_SAMPLES 256   // 256 samples = 20ms
#define STT_SEND_CHUNK_SIZE 1536 // Size of chunk to send to WebSocket
#define STT_WAIT_CHUNK_MS 100
#define STT_SEND_CHUNK_TIMEOUT 3000
#define STT_MIC_TASK_STACK_SIZE 1024 * 5
#define STT_STREAM_TASK_STACK_SIZE 1024 * 4

static std::string last_transcript;

std::string get_transcript(void)
{
    std::string transcript = last_transcript;
    last_transcript = "";
    return transcript;
}

static const char* TAG = "WS_CLIENT";
#define DEEPGRAM_BASE_URL                                                                                                      \
    "wss://api.deepgram.com/v1/"                                                                                               \
    "listen?model={}&language=en-US&channels=1&encoding=linear16&sample_rate=8000&interim_results=true&endpointing={}&smart_"  \
    "format={}"
// "&utterance_end_ms=1000&vad_events=false"

typedef struct
{
    HAL::Hal* hal;
    StaticRingbuffer_t ring_buffer;
    RingbufHandle_t audio_ring_buffer;
    esp_websocket_client_handle_t ws_client;
    EventGroupHandle_t control_event_group;
    TaskHandle_t mic_task_handle;    // Handle for the microphone reading task
    TaskHandle_t stream_task_handle; // Handle for the WebSocket sending task
    size_t total_bytes_sent;
} deepgram_context_t;

// Global static context - needed for stop function access
static deepgram_context_t deepgram_context;

static void websocket_event_handler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data)
{
    esp_websocket_event_data_t* data = (esp_websocket_event_data_t*)event_data;
    deepgram_context_t* context = (deepgram_context_t*)handler_args;
    if (!context)
    {
        ESP_LOGE(TAG, "Handler context is NULL!");
        return;
    }

    switch (event_id)
    {
    case WEBSOCKET_EVENT_BEGIN:
        ESP_LOGD(TAG, "WEBSOCKET_EVENT_BEGIN");
        xEventGroupClearBits(context->control_event_group, STT_STREAM_ERROR_BIT);
        break;
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGD(TAG, "WEBSOCKET_EVENT_CONNECTED");
        xEventGroupClearBits(context->control_event_group, STT_RECORD_STOP_REQUEST_BIT | STT_STREAM_ERROR_BIT);
        xEventGroupSetBits(context->control_event_group, STT_STREAM_CONNECTED_BIT | STT_RECORD_START_REQUEST_BIT);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "WEBSOCKET_EVENT_DISCONNECTED");
        xEventGroupClearBits(context->control_event_group, STT_STREAM_CONNECTED_BIT);
        xEventGroupSetBits(context->control_event_group, STT_RECORD_STOP_REQUEST_BIT);
        ESP_LOGD(TAG, "HTTP status code: %d", data->error_handle.esp_ws_handshake_status_code);
        if (data->error_handle.error_type == WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT)
        {
            ESP_LOGW(TAG,
                     "esp-tls: %d, tls stack: %d, transport socket errno: %d",
                     data->error_handle.esp_tls_last_esp_err,
                     data->error_handle.esp_tls_stack_err,
                     data->error_handle.esp_transport_sock_errno);
        }
        break;
    case WEBSOCKET_EVENT_DATA:
        ESP_LOGD(TAG, "WEBSOCKET_EVENT_DATA");
        ESP_LOGD(TAG, "Received opcode=%d", data->op_code);
        if (data->op_code == 0x1)
        { // Opcode 0x1 indicates text data
            ESP_LOGD(TAG, "%.*s", data->data_len, (char*)data->data_ptr);
            cJSON* root = cJSON_Parse(data->data_ptr);
            if (root)
            {
                cJSON* type = cJSON_GetObjectItem(root, "type");
                // Open: str = "Open"
                // Close: str = "Close"
                // Transcript: str = "Results"
                // Metadata: str = "Metadata"
                // UtteranceEnd: str = "UtteranceEnd"
                // SpeechStarted: str = "SpeechStarted"
                // Finalize: str = "Finalize"
                // Error: str = "Error"
                // Unhandled: str = "Unhandled"
                // Warning: str = "Warning"
                if (type)
                {
                    ESP_LOGD(TAG, "Type: %s", type->valuestring);
                    if (strcmp(type->valuestring, "Results") == 0)
                    {
                        cJSON* ch = cJSON_GetObjectItem(root, "channel");
                        if (ch)
                        {
                            cJSON* alternatives = cJSON_GetObjectItem(ch, "alternatives");
                            if (alternatives)
                            {
                                // iterate the array of alternatives
                                cJSON* alternative = NULL;
                                cJSON_ArrayForEach(alternative, alternatives)
                                {
                                    cJSON* transcript = cJSON_GetObjectItem(alternative, "transcript");
                                    if (transcript)
                                    {
                                        ESP_LOGW(TAG, "Transcript: %s", transcript->valuestring);
                                        cJSON* is_final = cJSON_GetObjectItem(root, "is_final");
                                        if (is_final)
                                        {
                                            if (cJSON_IsTrue(is_final))
                                            {
                                                if (strlen(transcript->valuestring) > 0)
                                                {
                                                    last_transcript = transcript->valuestring;
                                                    // set bits stop recording
                                                    xEventGroupSetBits(context->control_event_group,
                                                                       STT_RECORD_STOP_REQUEST_BIT | STT_TRANSCRIPT_BIT);
                                                }
                                                // call gemini API
                                            }
                                            else
                                            {
                                                // wait the UI task to get the transcript
                                                if (strlen(transcript->valuestring) > 0 &&
                                                    !(xEventGroupGetBits(context->control_event_group) &
                                                      STT_PARTIAL_TRANSCRIPT_BIT))
                                                {
                                                    last_transcript = transcript->valuestring;
                                                    xEventGroupSetBits(context->control_event_group,
                                                                       STT_PARTIAL_TRANSCRIPT_BIT);
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                cJSON_Delete(root);
            }
        }
        else if (data->op_code == 0x2)
        { // Opcode 0x2 indicates binary data
            // should not happen
            ESP_LOG_BUFFER_HEX("Received binary data", data->data_ptr, data->data_len);
        }
        else if (data->op_code == 0x08 && data->data_len == 2)
        {
            ESP_LOGW(TAG, "Closed by peer with code=%d", data->data_ptr[0] << 8 | data->data_ptr[1]);
        }
        else if (data->op_code == 0x0A)
        {
            // ping, send keep alive
            ESP_LOGD(TAG, "PING, send keep alive");
            const char* keep_alive_msg = "{\"type\":\"KeepAlive\"}";
            esp_websocket_client_send_text(context->ws_client, keep_alive_msg, strlen(keep_alive_msg), pdMS_TO_TICKS(2000));
        }
        else
        {
            ESP_LOGW(TAG, "Received=%.*s", data->data_len, (char*)data->data_ptr);
            // handle JSON responce
        }
        // show stat (debug)
        // ESP_LOGD(TAG,
        //          "Total payload length=%d, data_len=%d, current payload offset=%d\r\n",
        //          data->payload_len,
        //          data->data_len,
        //          data->payload_offset);

        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGD(TAG, "WEBSOCKET_EVENT_ERROR");
        ESP_LOGD(TAG, "HTTP status code: %d", data->error_handle.esp_ws_handshake_status_code);
        if (data->error_handle.error_type == WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT)
        {
            ESP_LOGW(TAG,
                     "esp-tls: %d, tls stack: %d, transport socket errno: %d",
                     data->error_handle.esp_tls_last_esp_err,
                     data->error_handle.esp_tls_stack_err,
                     data->error_handle.esp_transport_sock_errno);
        }
        xEventGroupSetBits(context->control_event_group, STT_STREAM_ERROR_BIT);
        break;
    case WEBSOCKET_EVENT_FINISH:
        ESP_LOGD(TAG, "WEBSOCKET_EVENT_FINISH");
        break;
    }
}

// Task dedicated to reading microphone data and putting it into the ring buffer
static void stt_mic_task(void* pvParameters)
{
    deepgram_context_t* context = (deepgram_context_t*)pvParameters;
    EventBits_t bits;
    bool running = true;
    bool is_recording = false;
    int16_t frame_buffer[MIC_BUFFER_SAMPLES];
    ESP_LOGI(TAG, "Mic task started");
    xEventGroupSetBits(context->control_event_group, STT_MIC_TASK_STARTED_BIT);
    context->hal->speaker()->end();
    context->hal->mic()->begin();
    while (running)
    {
        // Check if stop has been requested
        bits = xEventGroupGetBits(context->control_event_group);
        if (bits & STT_STOP_REQUEST_BIT)
        {
            ESP_LOGD(TAG, "Mic task received STOP signal, closing task");
            // clearing the ring buffer, discarding all data
            running = false;
            continue;
        }
        if (!is_recording && (bits & STT_RECORD_START_REQUEST_BIT))
        {
            ESP_LOGD(TAG, "Mic task received START RECORDING signal");
            // clear the bit
            xEventGroupClearBits(context->control_event_group, STT_RECORD_START_REQUEST_BIT);
            is_recording = true;
        }
        if (is_recording && (bits & STT_RECORD_STOP_REQUEST_BIT))
        {
            ESP_LOGD(TAG, "Mic task received STOP RECORDING signal");
            while (context->hal->mic()->isRecording())
            {
                vTaskDelay(pdMS_TO_TICKS(1));
            }
            context->hal->mic()->end();
            is_recording = false;
            // clear the bit
            xEventGroupClearBits(context->control_event_group, STT_RECORD_STOP_REQUEST_BIT);
        }

        if (is_recording && context->hal->mic()->record(frame_buffer, MIC_BUFFER_SAMPLES, AUDIO_SAMPLE_RATE, false))
        {
            if (xRingbufferSend(context->audio_ring_buffer,
                                frame_buffer,
                                MIC_BUFFER_SAMPLES * sizeof(int16_t),
                                pdMS_TO_TICKS(BUFFER_SIZE_SECONDS * 1000)) != pdTRUE)
            {
                ESP_LOGW(TAG, "Failed to send frame_buffer to ring buffer");
            }
        }
    }
    ESP_LOGI(TAG, "Mic task stopped");
    context->hal->mic()->end();
    context->hal->speaker()->begin();
    xEventGroupSetBits(context->control_event_group, STT_MIC_TASK_STOPPED_BIT);
    vTaskDelete(NULL); // Delete self
}

// Task dedicated to reading from the ring buffer and sending via WebSocket
static void stt_stream_task(void* pvParameters)
{
    deepgram_context_t* context = (deepgram_context_t*)pvParameters;
    uint8_t* ws_send_buffer = NULL;
    size_t item_size = 0;
    bool running = false;
    // EventBits_t bits;

    ESP_LOGI(TAG, "Streaming task started");
    context->total_bytes_sent = 0;
    xEventGroupSetBits(context->control_event_group, STT_STREAM_TASK_STARTED_BIT);
    running = true; // Start immediately, will block on ringbuffer read
    // Main loop: read from ring buffer and send via WebSocket
    while (running)
    {
        EventBits_t bits = xEventGroupGetBits(context->control_event_group);
        if (bits & STT_STOP_REQUEST_BIT)
        {
            running = false;
            ESP_LOGD(TAG, "Streaming task received STOP signal, stopping");
            break;
        }

        // Receive data from the ring buffer (blocks for a short time if empty)
        ws_send_buffer = (uint8_t*)xRingbufferReceiveUpTo(context->audio_ring_buffer,
                                                          &item_size,
                                                          pdMS_TO_TICKS(STT_WAIT_CHUNK_MS),
                                                          STT_SEND_CHUNK_SIZE);

        if (ws_send_buffer)
        {
            if (item_size > 0)
            {
                // Send data if WebSocket is connected
                if (bits & STT_STREAM_CONNECTED_BIT)
                // if (esp_websocket_client_is_connected(context->ws_client))
                {
#if 0
                    FILE* file = fopen("/sdcard/deepgram.wav", "ab");
                    int data_sent = 0;
                    if (file)
                    {
                        data_sent = fwrite(ws_send_buffer, 1, item_size, file);
                        fclose(file);
                    }
#else
                    int data_sent = esp_websocket_client_send_bin(context->ws_client,
                                                                  (const char*)ws_send_buffer,
                                                                  item_size,
                                                                  pdMS_TO_TICKS(STT_SEND_CHUNK_TIMEOUT));
#endif
                    if (data_sent == item_size)
                    {
                        ESP_LOGD(TAG, ">> %d OK", item_size);
                        context->total_bytes_sent += item_size;
                    }
                    else
                    {
                        ESP_LOGE(TAG, ">> %d / %d ERROR", item_size, data_sent);
                    }
                }
                else
                {
                    ESP_LOGD(TAG, "Discarding %d bytes", item_size);
                }
            }
            // Return the buffer item
            vRingbufferReturnItem(context->audio_ring_buffer, (void*)ws_send_buffer);
            ws_send_buffer = NULL;
        }
    }

    ESP_LOGI(TAG, "Streaming task stopping. Sent %d bytes total", context->total_bytes_sent);
    xEventGroupSetBits(context->control_event_group, STT_STREAM_TASK_STOPPED_BIT);
    vTaskDelete(NULL); // Delete self
}

bool deepgram_streaming_start(HAL::Hal* hal, EventGroupHandle_t control_event_group)
{
    ESP_LOGI(TAG, "Initializing Deepgram streaming...");
    // clear event bits
    xEventGroupClearBits(control_event_group,
                         STT_STREAM_CONNECTED_BIT | STT_STREAM_TASK_STARTED_BIT | STT_STREAM_TASK_STOPPED_BIT |
                             STT_STOP_REQUEST_BIT | STT_MIC_TASK_STARTED_BIT | STT_MIC_TASK_STOPPED_BIT |
                             STT_RECORD_START_REQUEST_BIT | STT_RECORD_STOP_REQUEST_BIT | STT_TRANSCRIPT_BIT |
                             STT_STREAM_ERROR_BIT);
    std::string model = hal->settings()->getString("deepgram", "model");
    int32_t endpointing = hal->settings()->getNumber("deepgram", "endpointing");
    bool smart_format = hal->settings()->getBool("deepgram", "smart_format");
    std::string url = std::format(DEEPGRAM_BASE_URL, model, endpointing, smart_format ? "true" : "false");
    std::string api_key = hal->settings()->getString("deepgram", "api_key");
    // Use the global static context
    memset(&deepgram_context, 0, sizeof(deepgram_context_t));
    deepgram_context.hal = hal;
    deepgram_context.control_event_group = control_event_group;
    esp_err_t start_ret;
    BaseType_t stream_task_created;
    BaseType_t mic_task_created;
    // Initialize WebSocket client
    esp_websocket_client_config_t websocket_cfg = {};
    // Create ring buffer
    deepgram_context.audio_ring_buffer =
        xRingbufferCreateStatic(RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF, audio_buffer, &deepgram_context.ring_buffer);
    if (!deepgram_context.audio_ring_buffer)
    {
        ESP_LOGE(TAG, "Failed to create ring buffer");
        goto cleanup;
    }

    websocket_cfg.uri = url.c_str();
    websocket_cfg.task_prio = 5;
    websocket_cfg.buffer_size = 2048;
    websocket_cfg.reconnect_timeout_ms = 1500;
    websocket_cfg.network_timeout_ms = 5000;

    ESP_LOGD(TAG, "Connecting to %s...", websocket_cfg.uri);
    deepgram_context.ws_client = esp_websocket_client_init(&websocket_cfg);
    if (!deepgram_context.ws_client)
    {
        ESP_LOGE(TAG, "Failed to init WebSocket client");
        goto cleanup;
    }

    if (api_key.empty())
    {
        ESP_LOGE(TAG, "Deepgram API Key not set!");
        goto cleanup;
    }
    esp_websocket_client_append_header(deepgram_context.ws_client, "Authorization", std::format("Token {}", api_key).c_str());

    esp_websocket_register_events(deepgram_context.ws_client,
                                  WEBSOCKET_EVENT_ANY,
                                  websocket_event_handler,
                                  (void*)&deepgram_context);

    // Create the microphone reading task
    mic_task_created = xTaskCreate(stt_mic_task,
                                   "stt_mic_task",
                                   STT_MIC_TASK_STACK_SIZE,
                                   (void*)&deepgram_context,
                                   5,
                                   &deepgram_context.mic_task_handle);

    if (mic_task_created != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create mic task");
        goto cleanup;
    }

    // Create the WebSocket sending task
    stream_task_created = xTaskCreate(stt_stream_task,
                                      "stt_stream_task",
                                      STT_STREAM_TASK_STACK_SIZE,
                                      (void*)&deepgram_context,
                                      6,
                                      &deepgram_context.stream_task_handle);

    if (stream_task_created != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create WebSocket sending task");
        goto cleanup;
    }

    // Start WebSocket client
    start_ret = esp_websocket_client_start(deepgram_context.ws_client);
    if (start_ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start WebSocket client: %s", esp_err_to_name(start_ret));
        goto cleanup;
    }

    // Wait briefly for tasks to potentially start
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "WebSocket client started");
    return true;

cleanup:
    // Clean up tasks if they were created
    if (deepgram_context.mic_task_handle)
    {
        vTaskDelete(deepgram_context.mic_task_handle);
        deepgram_context.mic_task_handle = NULL;
    }
    if (deepgram_context.stream_task_handle)
    {
        vTaskDelete(deepgram_context.stream_task_handle);
        deepgram_context.stream_task_handle = NULL;
    }

    // Clean up WebSocket client
    if (deepgram_context.ws_client)
    {
        esp_websocket_client_destroy(deepgram_context.ws_client);
        deepgram_context.ws_client = NULL;
    }

    // Clean up ring buffer
    if (deepgram_context.audio_ring_buffer)
    {
        vRingbufferDelete(deepgram_context.audio_ring_buffer);
        deepgram_context.audio_ring_buffer = NULL;
    }

    ESP_LOGW(TAG, "Deepgram streaming initialization failed, resources cleaned up");
    return false;
}

void deepgram_streaming_stop(void)
{
    ESP_LOGI(TAG, "Stopping Deepgram streaming...");
    xEventGroupSetBits(deepgram_context.control_event_group, STT_STOP_REQUEST_BIT);
    vTaskDelay(pdMS_TO_TICKS(100));

    if (deepgram_context.ws_client)
    {
        const char* close_stream_msg = "{\"type\":\"CloseStream\"}";
        esp_websocket_client_send_text(deepgram_context.ws_client,
                                       close_stream_msg,
                                       strlen(close_stream_msg),
                                       pdMS_TO_TICKS(2000));
        ESP_LOGD(TAG, "Stopping WebSocket client");
        esp_websocket_client_stop(deepgram_context.ws_client);
        ESP_LOGD(TAG, "Destroying WebSocket client");
        esp_websocket_client_destroy(deepgram_context.ws_client);
        deepgram_context.ws_client = NULL;
    }

    if (deepgram_context.audio_ring_buffer)
    {
        ESP_LOGD(TAG, "Deleting ring buffer");
        vRingbufferDelete(deepgram_context.audio_ring_buffer);
        deepgram_context.audio_ring_buffer = NULL;
    }

    deepgram_context.mic_task_handle = NULL;
    deepgram_context.stream_task_handle = NULL;

    ESP_LOGI(TAG, "Deepgram streaming stopped and resources cleaned up");
}