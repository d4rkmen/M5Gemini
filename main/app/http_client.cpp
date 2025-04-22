#include "http_client.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string>
#include <format>
#include <vector>
#include <utility>
#include <stdlib.h>
#include <M5Unified.h>
#include <cstdio>
#include <algorithm>
#include "hal_cardputer.h"
#include "audio_buffer.h"
#include "event_bits.h"

static const char* TAG = "http_client";

// Gemini API base URL
#define GEMINI_API_BASE_URL "https://generativelanguage.googleapis.com/v1beta/models/{}:generateContent?key={}"
// Eleven Labs API base URL
#define ELEVENLABS_API_BASE_URL                                                                                                \
    "https://api.elevenlabs.io/v1/text-to-speech/{}/stream?output_format=pcm_8000" // Requesting STREAM 8kHz PCM
#define ELEVENLABS_API_BASE_NO_STREAM_URL "https://api.elevenlabs.io/v1/text-to-speech/{}?output_format=pcm_8000"
#define PLAY_CHUNK_SIZE 512

// Forward declarations
void tts_play_task(void* pvParameters);

esp_err_t _http_event_handler_gemini(esp_http_client_event_t* evt)
{
    std::string key, value;
    gemini_context_t* context = (gemini_context_t*)evt->user_data;
    if (!context)
    {
        ESP_LOGE(TAG, "Context is NULL");
        return ESP_FAIL;
    }
    switch (evt->event_id)
    {
    case HTTP_EVENT_ERROR:
        ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        // make key lowercased
        key = evt->header_key;
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        value = evt->header_value;
        if (key == "content-type")
        {
            context->content_type = value;
        }
        else if (key == "content-length")
        {
            context->content_length = atoi(value.c_str());
        }
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        // put only data_len bytes from evt->data into input_stream
        if (evt->data && evt->data_len > 0)
        {
            if (context->content_type.find("application/json") == 0)
            {
                context->input_stream.write((char*)evt->data, evt->data_len);
            }
            else
            {
                ESP_LOGE(TAG, "Unknown content type: %s", context->content_type.c_str());
            }
        }
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
        break;
    case HTTP_EVENT_REDIRECT:
        ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
        // esp_http_client_set_header(evt->client, "From", "user@example.com");
        // esp_http_client_set_header(evt->client, "Accept", "application/json");
        esp_http_client_set_redirection(evt->client);
        break;
    }
    return ESP_OK;
}

// Event handler for Eleven Labs API call
esp_err_t _http_event_handler_11labs(esp_http_client_event_t* evt)
{
    std::string key, value;
    elevenlabs_context_t* context = (elevenlabs_context_t*)evt->user_data;
    if (!context)
    {
        ESP_LOGE(TAG, "Context is NULL");
        return ESP_FAIL;
    }
    switch (evt->event_id)
    {
    case HTTP_EVENT_ERROR:
        ESP_LOGE(TAG, "HTTP_EVENT_ERROR");
        // Signal error to playback task
        if (context->control_event_group)
        {
            xEventGroupSetBits(context->control_event_group, TTS_STOP_REQUEST_BIT);
        }
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        // make key lowercased
        key = evt->header_key;
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        value = evt->header_value;
        if (key == "content-type")
        {
            context->content_type = value;
        }
        else if (key == "content-length")
        {
            context->content_length = atoi(value.c_str());
        }

        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        // check control events
        if (xEventGroupGetBits(context->control_event_group) & TTS_STOP_REQUEST_BIT)
        {
            ESP_LOGD(TAG, "Received STOP, closing client");
            esp_http_client_close(evt->client);
            return ESP_OK;
        }

        if (evt->data && evt->data_len > 0)
        {
            if (context->content_type.find("application/json") == 0)
            {
                // Handle JSON responses (likely errors)
                ESP_LOGD(TAG, "data: %.*s", evt->data_len, (char*)evt->data);
                context->input_stream.write((char*)evt->data, evt->data_len);

                // If we get JSON and the ring buffer exists, we should signal end of stream
                // since this is likely an error response
                if (context->control_event_group)
                {
                    xEventGroupSetBits(context->control_event_group, TTS_STOP_REQUEST_BIT);
                }
            }
            else if (context->content_type == "audio/pcm")
            {
#if 0
                uint32_t size = evt->data_len;
                uint32_t offset = 0;
                if (context->has_leftover)
                {
                    context->leftover[1] = *(uint8_t*)evt->data;
                    context->hal->speaker()->playRaw(reinterpret_cast<const int16_t*>(context->leftover),
                                                     1,
                                                     AUDIO_SAMPLE_RATE,
                                                     false, // not stereo
                                                     1,
                                                     AUDIO_CHANNEL, // loudspeaker channel
                                                     false);        // not stop current sound);
                    offset += 1;
                    size -= 1;
                }
                context->hal->speaker()->playRaw(reinterpret_cast<const int16_t*>(evt->data + offset),
                                                 size / sizeof(int16_t),
                                                 AUDIO_SAMPLE_RATE,
                                                 false, // not stereo
                                                 1,
                                                 AUDIO_CHANNEL, // loudspeaker channel
                                                 false);        // not stop current sound);
                if (context->has_leftover)
                {
                    context->has_leftover = false;
                }
                if (size % (AUDIO_BITS_PER_SAMPLE / 8) != 0)
                {
                    // get byte from evt->data at offset + size - 1
                    context->leftover[0] = *(uint8_t*)evt->data + offset + size - 1;
                    context->has_leftover = true;
                }
// Add the audio data to the ring buffer
#else
                if (context->audio_ring_buffer)
                {
                    // Attempt to write to the ring buffer - this will block if the buffer is full
                    BaseType_t ret =
                        xRingbufferSend(context->audio_ring_buffer,
                                        evt->data,
                                        evt->data_len,
                                        pdMS_TO_TICKS(BUFFER_SIZE_SECONDS * 1000)); // Allow some time to wait if buffer is full

                    if (ret != pdTRUE)
                    {
                        ESP_LOGE(TAG, "Failed to write %d bytes to ring buffer!", evt->data_len);
                    }
                    else
                    {
                        context->total_bytes_received += evt->data_len;
                        // Check if we have enough data buffered to start playback
                        if ((xEventGroupGetBits(context->control_event_group) & TTS_PLAYBACK_START_REQUEST_BIT) == 0)
                        {
                            size_t items = context->total_bytes_received;
                            size_t required_bytes =
                                AUDIO_SAMPLE_RATE * (AUDIO_BITS_PER_SAMPLE / 8) * AUDIO_CHANNELS * BUFFER_TIME_SECONDS;

                            // If we have enough data buffered or the content length is known, start playback
                            if (items >= required_bytes || (context->content_length != -1 && items >= context->content_length))
                            {
                                ESP_LOGD(TAG, "Ring buffer has %d bytes, starting playback", items);
                                // Signal playback task to start
                                if (context->control_event_group)
                                {
                                    ESP_LOGD(TAG, "Setting playback start request");
                                    xEventGroupSetBits(context->control_event_group, TTS_PLAYBACK_START_REQUEST_BIT);
                                }
                            }
                        }
                    }
                }
                else
                {
                    ESP_LOGE(TAG, "Ring buffer not initialized but received audio data!");
                }
#endif
            }
            else
            {
                ESP_LOGE(TAG, "Unknown content type: %s", context->content_type.c_str());
            }
        }
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        // Signal that no more data is coming
        if (context->control_event_group)
        {
            if ((xEventGroupGetBits(context->control_event_group) & TTS_PLAYBACK_START_REQUEST_BIT) == 0)
            {
                // Signal playback task to start
                ESP_LOGD(TAG, "Setting playback start request");
                xEventGroupSetBits(context->control_event_group, TTS_PLAYBACK_START_REQUEST_BIT);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            ESP_LOGD(TAG, "Signaled end of stream (received %d bytes total)", context->total_bytes_received);
            xEventGroupSetBits(context->control_event_group, TTS_PLAYBACK_STOP_REQUEST_BIT);
        }
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
        // Also signal end of stream
        if (context->control_event_group)
        {
            ESP_LOGD(TAG, "Disconnected, signaled end of stream");
            // if playing, play to the end, otherwise signal stop
            xEventGroupSetBits(context->control_event_group,
                               (xEventGroupGetBits(context->control_event_group) & TTS_PLAYBACK_START_REQUEST_BIT)
                                   ? TTS_PLAYBACK_STOP_REQUEST_BIT
                                   : TTS_STOP_REQUEST_BIT);
        }
        break;
    case HTTP_EVENT_REDIRECT:
        ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
        esp_http_client_set_redirection(evt->client);
        break;
    }
    return ESP_OK;
}

// Audio playback task
void tts_play_task(void* pvParameters)
{
    elevenlabs_context_t* context = (elevenlabs_context_t*)pvParameters;
    size_t item_size = 0;
    uint8_t* item = NULL;
    bool running = false;
    EventBits_t bits;
    uint8_t leftover[2];
    bool has_leftover = false;
    bool stopping = false;

    // Task startup
    ESP_LOGD(TAG, "Audio playback task started");
    xEventGroupSetBits(context->control_event_group, TTS_PLAY_TASK_STARTED_BIT);
    context->total_bytes_played = 0;

    // Wait for the start signal or stop signal
    bits = xEventGroupWaitBits(context->control_event_group,
                               TTS_PLAYBACK_START_REQUEST_BIT | TTS_STOP_REQUEST_BIT,
                               pdFALSE,      // Clear bits on exit
                               pdFALSE,      // Don't wait for all bits
                               portMAX_DELAY // Wait indefinitely
    );

    // If we got the stop signal without start, we're done
    if (bits & TTS_STOP_REQUEST_BIT)
    {
        ESP_LOGD(TAG, "Received STOP, exiting playback task");
        running = false;
    }
    if (bits & TTS_PLAYBACK_START_REQUEST_BIT)
    {
        ESP_LOGD(TAG, "Received START, starting audio playback");
        running = true;
    }

    // Main playback loop
    while (running)
    {
        // Get data from ring buffer, consider leftover (16-bit samples split into 2 bytes)
        item = (uint8_t*)xRingbufferReceiveUpTo(context->audio_ring_buffer,
                                                &item_size,
                                                pdMS_TO_TICKS(10),
                                                has_leftover ? 1 : PLAY_CHUNK_SIZE);

        if (item != NULL)
        {
            // We have audio data to play
            if (item_size > 0)
            {
                if (has_leftover)
                {
                    leftover[1] = item[0];
                }
                // Calculate number of samples (16-bit = 2 bytes per sample)
                uint32_t num_samples = item_size / (AUDIO_BITS_PER_SAMPLE / 8);
                // Play the audio
                context->hal->speaker()->playRaw(reinterpret_cast<const int16_t*>(has_leftover ? leftover : item),
                                                 has_leftover ? 1 : num_samples,
                                                 AUDIO_SAMPLE_RATE,
                                                 false, // not stereo
                                                 1,
                                                 AUDIO_CHANNEL, // loudspeaker channel
                                                 false          // not stop current sound
                );
                // do not return ring buffer until the audio is finish playing
                while (context->hal->speaker()->isPlaying())
                {
                    vTaskDelay(pdMS_TO_TICKS(1));
                }
                // Update stats
                context->total_bytes_played += item_size;
            }
            // handle leftover byte
            if (has_leftover)
            {
                has_leftover = false;
            }
            else if (item_size % (AUDIO_BITS_PER_SAMPLE / 8) != 0)
            {
                leftover[0] = item[item_size - 1];
                has_leftover = true;
            }

            // Return the space to the ring buffer
            vRingbufferReturnItem(context->audio_ring_buffer, (void*)item);
        }

        // Check if we should stop
        bits = xEventGroupGetBits(context->control_event_group);
        if (bits & TTS_STOP_REQUEST_BIT)
        {
            ESP_LOGW(TAG, "Received TASK STOP, exiting playback task");
            running = false;
        }
        // Check if we should end playback
        if (!stopping && (bits & TTS_PLAYBACK_STOP_REQUEST_BIT))
        {
            stopping = true;
            ESP_LOGW(TAG, "Received STOP, waiting for end of stream");
        }
        if (stopping && (context->total_bytes_played == context->total_bytes_received))
        {
            ESP_LOGW(TAG, "Reached end of stream");
            running = false;
        }
    }

    // Cleanup
    context->hal->speaker()->stop();

    // Signal that we're done
    xEventGroupSetBits(context->control_event_group, TTS_PLAY_TASK_STOPPED_BIT);

    ESP_LOGD(TAG,
             "Audio playback task complete, played %d/%d bytes",
             context->total_bytes_played,
             context->total_bytes_received);

    vTaskDelete(NULL);
}

std::string call_google_api(const std::string& model_name,
                            const std::string& api_key,
                            const std::string& rules,
                            const std::vector<std::pair<std::string, std::string>>& history,
                            const std::string& prompt)
{
    // Create a context for the Gemini HTTP client in the stack
    gemini_context_t context;
    context.content_type = "";
    context.content_length = -1;
    char* post_data = NULL;
    cJSON *root, *contents, *system_instruction, *jmessage, *jstatus, *parts_user, *part_user, *content_user;
    esp_http_client_handle_t client = NULL;
    esp_err_t err;
    int status_code;

    // Resource tracking flags
    bool client_initialized = false;

    if (api_key.empty())
    {
        return "Error: API key is empty. Please go to settings and set the API key";
    }
    if (model_name.empty())
    {
        return "Error: Model name is empty. Please go to settings and set the model name";
    }
    if (prompt.empty())
    {
        return "Error: Prompt is empty. Please enter a prompt";
    }
    ESP_LOGD(TAG, "Free RAM: %ld", (uint32_t)esp_get_free_heap_size());

    ESP_LOGD(TAG, "Calling Gemini API with prompt: %s", prompt.c_str());

    // Construct the URL with API key
    std::string url = std::format(GEMINI_API_BASE_URL, model_name, api_key);
    esp_http_client_config_t config = {
        .url = url.c_str(),
        // .cert_pem = google_root_cert_pem_start,
        .timeout_ms = 60000,
        .event_handler = _http_event_handler_gemini,
        .user_data = (void*)&context,
    };
    std::string response = "Error: Failed to create JSON request";

    // Create JSON request payload
    root = cJSON_CreateObject();
    if (!root)
    {
        ESP_LOGE(TAG, "Failed to create JSON object");
        goto cleanup;
    }

    // Build the JSON structure
    contents = cJSON_CreateArray();
    if (!contents)
    {
        ESP_LOGE(TAG, "Failed to create contents array");
        goto cleanup;
    }

    // Add history to contents
    for (const auto& turn : history)
    {
        // User turn
        cJSON* content_user_hist = cJSON_CreateObject();
        cJSON* parts_user_hist = cJSON_CreateArray();
        cJSON* part_user_hist = cJSON_CreateObject();
        cJSON_AddStringToObject(part_user_hist, "text", turn.first.c_str());
        cJSON_AddItemToArray(parts_user_hist, part_user_hist);
        cJSON_AddStringToObject(content_user_hist, "role", "user");
        cJSON_AddItemToObject(content_user_hist, "parts", parts_user_hist);
        cJSON_AddItemToArray(contents, content_user_hist);

        // Model turn
        cJSON* content_model_hist = cJSON_CreateObject();
        cJSON* parts_model_hist = cJSON_CreateArray();
        cJSON* part_model_hist = cJSON_CreateObject();
        cJSON_AddStringToObject(part_model_hist, "text", turn.second.c_str());
        cJSON_AddItemToArray(parts_model_hist, part_model_hist);
        cJSON_AddStringToObject(content_model_hist, "role", "model");
        cJSON_AddItemToObject(content_model_hist, "parts", parts_model_hist);
        cJSON_AddItemToArray(contents, content_model_hist);
    }

    // Add current user prompt
    content_user = cJSON_CreateObject();
    if (!content_user)
    {
        ESP_LOGE(TAG, "Failed to create content object");
        goto cleanup;
    }

    parts_user = cJSON_CreateArray();
    if (!parts_user)
    {
        ESP_LOGE(TAG, "Failed to create parts array");
        goto cleanup;
    }

    part_user = cJSON_CreateObject();
    if (!part_user)
    {
        ESP_LOGE(TAG, "Failed to create part object");
        goto cleanup;
    }

    if (!cJSON_AddStringToObject(part_user, "text", prompt.c_str()))
    {
        ESP_LOGE(TAG, "Failed to add text to part");
        goto cleanup;
    }

    if (!cJSON_AddStringToObject(content_user, "role", "user"))
    {
        ESP_LOGE(TAG, "Failed to add role user");
        goto cleanup;
    }
    cJSON_AddItemToArray(parts_user, part_user);
    cJSON_AddItemToObject(content_user, "parts", parts_user);
    cJSON_AddItemToArray(contents, content_user);

    // System instruction (if rules are provided)
    if (!rules.empty())
    {
        system_instruction = cJSON_CreateObject();
        if (!system_instruction)
        {
            ESP_LOGE(TAG, "Failed to create system_instruction object");
            goto cleanup;
        }
        cJSON* parts_system = cJSON_CreateArray();
        if (!parts_system)
        {
            ESP_LOGE(TAG, "Failed to create parts_system array");
            goto cleanup;
        }
        cJSON* part_system = cJSON_CreateObject();
        if (!part_system)
        {
            ESP_LOGE(TAG, "Failed to create part_system object");
            goto cleanup;
        }
        if (!cJSON_AddStringToObject(part_system, "text", rules.c_str()))
        {
            ESP_LOGE(TAG, "Failed to add text to part_system");
            goto cleanup;
        }
        cJSON_AddItemToArray(parts_system, part_system);
        cJSON_AddItemToObject(system_instruction, "parts", parts_system);
        cJSON_AddItemToObject(root, "system_instruction", system_instruction);
    }

    // Add contents array to root
    cJSON_AddItemToObject(root, "contents", contents);

    // Convert to string
    post_data = cJSON_Print(root);
    if (!post_data)
    {
        ESP_LOGE(TAG, "Failed to print JSON to string");
        goto cleanup;
    }

    client = esp_http_client_init(&config);
    if (!client)
    {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        response = "Error: Failed to initialize HTTP client";
        goto cleanup;
    }
    client_initialized = true;

    // Set headers and method
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    // Set post data
    esp_http_client_set_post_field(client, post_data, strlen(post_data));
    ESP_LOGD(TAG, "Post data: %s", post_data);

    err = esp_http_client_perform(client);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        response = std::format("Error: HTTP request failed: {}", esp_err_to_name(err));
        goto cleanup;
    }
    status_code = esp_http_client_get_status_code(client);
    ESP_LOGD(TAG, "HTTP Status = %d: %s", status_code, context.input_stream.str().c_str());
    // Get the status code
    if (status_code == 200)
    {
        // Parse JSON response to extract generated text
        cJSON* response_json = cJSON_Parse(context.input_stream.str().c_str());
        if (response_json)
        {
            // Navigate to the text content in the response structure
            // The structure is:
            // {
            //   "candidates": [
            //     {
            //       "content": {
            //         "parts": [
            //           {
            //             "text": "..."
            //           }
            //         ]
            //       }
            //     }
            //   ]
            // }
            cJSON* candidates = cJSON_GetObjectItem(response_json, "candidates");
            if (candidates && cJSON_IsArray(candidates) && cJSON_GetArraySize(candidates) > 0)
            {
                cJSON* candidate = cJSON_GetArrayItem(candidates, 0);
                cJSON* content = cJSON_GetObjectItem(candidate, "content");
                if (content)
                {
                    cJSON* parts = cJSON_GetObjectItem(content, "parts");
                    if (parts && cJSON_IsArray(parts) && cJSON_GetArraySize(parts) > 0)
                    {
                        cJSON* part = cJSON_GetArrayItem(parts, 0);
                        cJSON* text = cJSON_GetObjectItem(part, "text");
                        if (text && cJSON_IsString(text))
                        {
                            response = cJSON_GetStringValue(text);
                        }
                    }
                }
            }

            // Handle error responses
            cJSON* error = cJSON_GetObjectItem(response_json, "error");
            if (error)
            {
                cJSON* message = cJSON_GetObjectItem(error, "message");
                if (message && cJSON_IsString(message))
                {
                    response = std::format("Error: {}", cJSON_GetStringValue(message));
                }
                else
                {
                    response = "Error: Unknown API error occurred";
                }
            }

            cJSON_Delete(response_json);
        }
        else
        {
            response = "Error: Failed to parse JSON response";
        }
    } // 200
    else
    {
        // Parse JSON response to extract generated text
        cJSON* response_json = cJSON_Parse(context.input_stream.str().c_str());
        if (response_json)
        {
            // Handle error responses
            std::string message = "n/a";
            std::string status = "n/a";
            cJSON* error = cJSON_GetObjectItem(response_json, "error");
            if (error)
            {
                jmessage = cJSON_GetObjectItem(error, "message");
                if (jmessage && cJSON_IsString(jmessage))
                {
                    message = cJSON_GetStringValue(jmessage);
                }
                jstatus = cJSON_GetObjectItem(error, "status");
                if (jstatus && cJSON_IsString(jstatus))
                {
                    status = cJSON_GetStringValue(jstatus);
                }
            }
            cJSON_Delete(response_json);
            response = std::format("Error: {}, status: {}, message: {}", status_code, status, message);
        }
        else
        {
            response = std::format("Error: {}", status_code);
        }
    }

cleanup:
    // Clean up cJSON objects created in the history loop
    // Note: cJSON_Delete handles null pointers and child items recursively,
    // so we only need to delete the top-level objects we added to the array.
    // `root` deletion handles everything added to it.

    // Clean up resources based on what was allocated/initialized
    free(post_data);

    if (client_initialized)
    {
        esp_http_client_cleanup(client);
    }

    if (root)
    {
        cJSON_Delete(root);
    }

    ESP_LOGD(TAG, "API call completed");
    return response;
}

// Function to call Eleven Labs Text-to-Speech API
std::string call_11labs_api(HAL::Hal* hal,
                            const std::string& api_key,
                            const std::string& voice_id,
                            const std::string& text,
                            const std::string& model_id,
                            EventGroupHandle_t control_event_group) // Accept control event group handle
{
    elevenlabs_context_t context;
    context.content_type = "";
    context.content_length = -1;
    context.hal = hal;
    context.audio_ring_buffer = NULL;
    context.control_event_group = control_event_group; // Store the control group handle
    context.playback_task_handle = NULL;
    context.total_bytes_received = 0;
    context.total_bytes_played = 0;

    char* post_data = NULL;
    cJSON* root = NULL;
    esp_http_client_handle_t client = NULL;
    esp_err_t err;
    int status_code;
    std::string result_message = "OK"; // Default success message

    // Resource tracking flags
    bool client_initialized = false;
    bool ring_buffer_created = false;

    // Validate parameters
    if (!hal)
    {
        return "Error: HAL pointer is null";
    }
    if (!hal->speaker())
    {
        return "Error: Speaker not available in HAL";
    }
    if (api_key.empty())
    {
        return "Error: ElevenLabs API key is empty";
    }
    if (voice_id.empty())
    {
        return "Error: ElevenLabs Voice ID is empty";
    }
    if (text.empty())
    {
        return "Error: Text is empty";
    }
    ESP_LOGD(TAG, "Free RAM: %ld", (uint32_t)esp_get_free_heap_size());

    ESP_LOGD(TAG, "Calling ElevenLabs API with voice ID: %s", voice_id.c_str());

    std::string url = std::format(ELEVENLABS_API_BASE_URL, voice_id);

    // Configure HTTP client
    esp_http_client_config_t config = {
        .url = url.c_str(),
        // .cert_pem = elevenlabs_root_cert_pem_start,
        .timeout_ms = 30000, // Increased timeout for TTS
        .event_handler = _http_event_handler_11labs,
        .buffer_size = 2048, // Increased buffer for audio chunks
        .buffer_size_tx = 1024,
        .user_data = (void*)&context,
    };
    // hal->speaker()->begin();
    // Create the ring buffer
    context.audio_ring_buffer = xRingbufferCreateStatic(RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF, audio_buffer, &context.ring_buffer);
    if (!context.audio_ring_buffer)
    {
        ESP_LOGE(TAG, "Failed to create ring buffer");
        result_message = "Error: Failed to create audio buffer";
        goto cleanup;
    }
    ring_buffer_created = true;

    // Create audio playback task
    if (xTaskCreate(tts_play_task, "tts_play_task", 4096, &context, 5, &context.playback_task_handle) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create audio playback task");
        result_message = "Error: Failed to create audio playback task";
        goto cleanup;
    }

    // Create JSON request payload
    root = cJSON_CreateObject();
    if (!root)
    {
        ESP_LOGE(TAG, "Failed to create JSON object for ElevenLabs");
        result_message = "Error: Failed to create JSON request object";
        goto cleanup;
    }

    if (!cJSON_AddStringToObject(root, "text", text.c_str()))
    {
        ESP_LOGE(TAG, "Failed to add text to ElevenLabs JSON");
        result_message = "Error: Failed to add text to JSON";
        goto cleanup;
    }

    // Add model_id if provided
    if (!model_id.empty())
    {
        if (!cJSON_AddStringToObject(root, "model_id", model_id.c_str()))
        {
            ESP_LOGE(TAG, "Failed to add model_id to ElevenLabs JSON");
            // Non-critical error, could proceed without it
        }
    }

    // Convert JSON to string
    post_data = cJSON_PrintUnformatted(root);
    if (!post_data)
    {
        ESP_LOGE(TAG, "Failed to print ElevenLabs JSON to string");
        result_message = "Error: Failed to serialize JSON request";
        goto cleanup;
    }
    ESP_LOGD(TAG, "ElevenLabs post data: %s", post_data);

    client = esp_http_client_init(&config);
    if (!client)
    {
        ESP_LOGE(TAG, "Failed to initialize 11Labs HTTP client");
        result_message = "Error: Failed to initialize HTTP client";
        goto cleanup;
    }
    client_initialized = true;

    // Set headers and method
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "xi-api-key", api_key.c_str());

    // Set post data
    esp_http_client_set_post_field(client, post_data, strlen(post_data));
    ESP_LOGD(TAG, "Sending request to ElevenLabs API");

    // Perform the HTTP request - Audio streaming starts in the event handler
    err = esp_http_client_perform(client);
    if (err == ESP_OK)
    {
        status_code = esp_http_client_get_status_code(client);
        ESP_LOGD(TAG,
                 "ElevenLabs HTTP Status = %d, content_length = %ld",
                 status_code,
                 (uint32_t)esp_http_client_get_content_length(client));

        if (status_code != 200)
        {
            // Check content type
            if (context.content_type == "application/json")
            {
                // Parse error details from JSON response
                std::string message = "n/a";
                cJSON* response_json = cJSON_Parse(context.input_stream.str().c_str());
                if (response_json)
                {
                    cJSON* detail = cJSON_GetObjectItem(response_json, "detail");
                    if (detail && cJSON_IsObject(detail))
                    {
                        cJSON* jmessage = cJSON_GetObjectItem(detail, "message");
                        if (jmessage && cJSON_IsString(jmessage))
                        {
                            message = cJSON_GetStringValue(jmessage);
                        }
                    }
                    cJSON_Delete(response_json);
                }
                result_message = std::format("Error: {}, message: {}", status_code, message);
            }
            else
            {
                result_message = std::format("Error: {}, unknown content type: {}", status_code, context.content_type);
            }
        }
        else
        {
#if 0
            // HTTP request was successful, wait for playback to complete
            ESP_LOGD(TAG, "Waiting for audio playback to complete");
            // Wait for playback to complete, optional
            EventBits_t bits = xEventGroupWaitBits(context.control_event_group,
                                                   TTS_PLAY_TASK_STOPPED_BIT,
                                                   pdFALSE,                     // Don't clear bits
                                                   pdFALSE,                     // Any bit will do
                                                   pdMS_TO_TICKS(5 * 60 * 1000) // max waittimeout
            );

            if (bits & TTS_PLAY_TASK_STOPPED_BIT)
            {
                ESP_LOGD(TAG, "Audio playback completed successfully");
                result_message = "OK";
            }
            else
            {
                ESP_LOGE(TAG, "Timeout waiting for audio playback to complete");
                result_message = "Error: Timeout waiting for audio playback";
            }
#else
            result_message = "OK";
#endif
        }
    }
    else
    {
        ESP_LOGE(TAG, "ElevenLabs HTTP failed: %s", esp_err_to_name(err));
        result_message = std::string("Error: HTTP request failed: ") + esp_err_to_name(err);
    }

cleanup:
    ESP_LOGD(TAG, "Cleaning up resources");

    // Clean up cJSON
    if (root)
    {
        cJSON_Delete(root);
    }
    if (post_data)
    {
        free(post_data);
    }
    // Wait for the task to finish with a timeout
    if (xEventGroupGetBits(context.control_event_group) & TTS_PLAY_TASK_STARTED_BIT)
    {
        ESP_LOGD(TAG, "Waiting for audio playback task to exit");
        EventBits_t bits = xEventGroupWaitBits(context.control_event_group,
                                               TTS_PLAY_TASK_STOPPED_BIT,
                                               pdFALSE,
                                               pdFALSE,
                                               pdMS_TO_TICKS(5000) // 5-second timeout
        );

        if (!(bits & TTS_PLAY_TASK_STOPPED_BIT))
        {
            ESP_LOGW(TAG, "Timeout waiting for audio task to exit, may need to delete forcefully");
            // Optionally delete the task forcefully - use with caution
            // vTaskDelete(context.playback_task_handle);
        }
    }

    // Clean up HTTP client
    if (client_initialized)
    {
        esp_http_client_cleanup(client);
    }

    // Clean up ring buffer
    if (ring_buffer_created)
    {
        vRingbufferDelete(context.audio_ring_buffer);
    }

    ESP_LOGD(TAG, "ElevenLabs API call finished: %s", result_message.c_str());
    return result_message;
}
