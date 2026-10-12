#include "s2s_client.h"
#include "audio_buffer.h"
#include "esp_websocket_client.h"
#include "esp_log.h"
#include "mjson.h"
#include "mbedtls/base64.h"
#include "freertos/ringbuf.h"
#include "hal.h"
#include "common_define.h"
#include <cmath>
#include <cstring>
#include "esp_rom_crc.h"
#include <format>

static const char* TAG = "S2S";

#define S2S_WS_BASE_URL                                                                                                        \
    "wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent"

#define MIC_BUF_SAMPLES 256
#define MIC_REC_BLOCKS 3
#define PCM_SEND_BYTES 3200 // 100ms at 16kHz 16-bit mono
#define PLAY_CHUNK_SIZE 1024

#define WS_BUFFER_SIZE 4096
// Holds a server message with its audio payload taken out (see s2s_feed), so it
// only needs to fit the JSON around it and the transcription text.
#define MSG_BUF_CAPACITY (1024 * 8)
#define AUDIO_DECODE_CHUNK 1536

// JSON template for audio send — prefix/suffix are fixed, base64 payload goes in between
static const char JSON_AUDIO_PREFIX[] = "{\"realtimeInput\":{\"audio\":{\"data\":\"";
static const char JSON_AUDIO_SUFFIX[] = "\",\"mimeType\":\"audio/pcm;rate=16000\"}}}";
static const char JSON_STREAM_END[] = "{\"realtimeInput\":{\"audioStreamEnd\":true}}";

#define JSON_PREFIX_LEN (sizeof(JSON_AUDIO_PREFIX) - 1)
#define JSON_SUFFIX_LEN (sizeof(JSON_AUDIO_SUFFIX) - 1)
#define MAX_B64_LEN ((PCM_SEND_BYTES * 4 / 3) + 4)
#define JSON_BUF_SIZE (JSON_PREFIX_LEN + MAX_B64_LEN + JSON_SUFFIX_LEN + 1)

#define S2S_TASK_STACK_SIZE (1024 * 8)
#define S2S_TASK_PRIORITY 5

// ─── Static buffers (BSS, not heap — avoids malloc fragmentation) ───
static int16_t s_rec_buf[MIC_REC_BLOCKS][MIC_BUF_SAMPLES];
static uint8_t s_pcm_buf[PCM_SEND_BYTES];
static char s_json_buf[JSON_BUF_SIZE];
static char s_msg_buf[MSG_BUF_CAPACITY];
static uint8_t s_dec_buf[AUDIO_DECODE_CHUNK];

// ─── Context ───
typedef struct
{
    HAL::Hal* hal;
    esp_websocket_client_handle_t ws_client;
    EventGroupHandle_t event_group;
    S2SSharedData* shared;

    StaticRingbuffer_t spk_rb_struct;
    RingbufHandle_t spk_ring_buffer;

    char* msg_buf;
    size_t msg_buf_len;
    bool msg_truncated;

    // Streaming decode of inlineData audio, straight from the socket to the speaker buffer
    uint8_t key_match;  // progress through the "data":" pattern
    bool in_audio;      // inside the base64 string
    bool audio_drop;    // this message's audio is being discarded
    bool audio_warned;
    uint32_t b64_acc;
    uint8_t b64_count;
    size_t dec_len;

    TaskHandle_t task_handle;

    volatile uint32_t payload_decoded;
    volatile uint32_t rb_bytes_written;
    volatile uint32_t rb_bytes_read;

    uint32_t crc_received;
    uint32_t crc_played;

    std::string model;
    std::string api_key;
    std::string voice;
    std::string rules;
    int32_t volume;
} s2s_context_t;

static s2s_context_t s2s_ctx;

// ─── Forward declarations ───
static void s2s_process_message(s2s_context_t* ctx);
static void s2s_feed(s2s_context_t* ctx, const char* data, size_t len);
static void s2s_feed_reset(s2s_context_t* ctx);
static bool s2s_send_setup(s2s_context_t* ctx);
static void s2s_client_task(void* parameter);

static void set_fatal_error(s2s_context_t* ctx, const char* msg)
{
    if (xSemaphoreTake(ctx->shared->mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        ctx->shared->error_message = msg ? msg : "Unknown error";
        xSemaphoreGive(ctx->shared->mutex);
    }
    xEventGroupSetBits(ctx->event_group, S2S_FATAL_ERROR_BIT | S2S_ERROR_BIT);
}

// ═══════════════════════════════════════════════════════════
// WebSocket event handler (runs in the ESP WS client task)
// ═══════════════════════════════════════════════════════════
static void s2s_ws_event_handler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data)
{
    esp_websocket_event_data_t* data = (esp_websocket_event_data_t*)event_data;
    s2s_context_t* ctx = (s2s_context_t*)handler_args;
    if (!ctx)
        return;

    switch (event_id)
    {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected");
        xEventGroupSetBits(ctx->event_group, S2S_CONNECTED_BIT);
        xEventGroupClearBits(ctx->event_group, S2S_ERROR_BIT);
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket disconnected");
        xEventGroupClearBits(ctx->event_group, S2S_CONNECTED_BIT | S2S_SETUP_COMPLETE_BIT);
        xEventGroupSetBits(ctx->event_group, S2S_ERROR_BIT);
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == 0x01 || data->op_code == 0x02 || data->op_code == 0x00)
        {
            // A message can span several WebSocket frames: the first carries the
            // text/binary opcode, the rest are continuation frames (0x00), and only
            // the last has FIN set. Start over on a new message, not on every frame.
            if (data->payload_offset == 0 && data->op_code != 0x00)
                s2s_feed_reset(ctx);

            s2s_feed(ctx, data->data_ptr, data->data_len);

            if (data->payload_offset + data->data_len >= data->payload_len && data->fin)
            {
                ctx->msg_buf[ctx->msg_buf_len] = '\0';
                ESP_LOGD(TAG, "WS msg: payload=%d buf=%d", (int)data->payload_len, (int)ctx->msg_buf_len);
                if (ctx->msg_truncated)
                    ESP_LOGW(TAG, "WS msg larger than %d bytes, dropped", MSG_BUF_CAPACITY);
                else
                    s2s_process_message(ctx);
                s2s_feed_reset(ctx);
            }
            else if (data->payload_offset + data->data_len >= data->payload_len)
            {
                ESP_LOGD(TAG, "WS fragment: %d bytes, more to follow", (int)data->payload_len);
            }
        }
        else if (data->op_code == 0x08)
        {
            int code = data->data_len >= 2 ? ((uint8_t)data->data_ptr[0] << 8 | (uint8_t)data->data_ptr[1]) : -1;
            char reason[201] = {0};
            int reason_len = 0;
            if (data->data_len > 2)
            {
                reason_len = data->data_len - 2;
                if (reason_len > 200)
                    reason_len = 200;
                memcpy(reason, data->data_ptr + 2, reason_len);
                reason[reason_len] = '\0';
                ESP_LOGE(TAG, "WS CLOSE code=%d reason=%s", code, reason);
            }
            else
            {
                ESP_LOGE(TAG, "WS CLOSE code=%d", code);
            }

            bool expired = (reason_len > 0 && strstr(reason, "expired") != nullptr);
            bool fatal = !expired && (code == 1008 || code == 1003);
            if (fatal)
            {
                set_fatal_error(ctx, reason_len > 0 ? reason : "Connection rejected by server");
            }
            else
            {
                if (expired)
                    ESP_LOGW(TAG, "Session expired, will reconnect");
                xEventGroupSetBits(ctx->event_group, S2S_ERROR_BIT);
            }
        }
        else if (data->op_code != 0x09 && data->op_code != 0x0a)
        {
            ESP_LOGW(TAG, "WS unhandled opcode 0x%x, %d bytes", data->op_code, data->data_len);
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error");
        xEventGroupSetBits(ctx->event_group, S2S_ERROR_BIT);
        break;

    default:
        break;
    }
}

// ═══════════════════════════════════════════════════════════
// Base64 decode table (64 = not a base64 character)
// ═══════════════════════════════════════════════════════════
static const uint8_t b64_lut[256] = {
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 62, 64, 64, 64, 63, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61,
    64, 64, 64, 64, 64, 64, 64, 0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
    22, 23, 24, 25, 64, 64, 64, 64, 64, 64, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44,
    45, 46, 47, 48, 49, 50, 51, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
};

// ═══════════════════════════════════════════════════════════
// Streaming audio extraction
//
// Audio arrives as a base64 string inside the JSON, and one message can be
// larger than any buffer we can afford. So the payload is never stored: as the
// bytes come off the socket, everything except the audio string is copied to
// msg_buf (leaving "data":"" behind), and the audio is decoded on the fly and
// pushed to the speaker ring buffer in small pieces.
// ═══════════════════════════════════════════════════════════
static const EventBits_t S2S_AUDIO_DROP_MASK =
    S2S_STOP_REQUEST_BIT | S2S_ERROR_BIT | S2S_FATAL_ERROR_BIT | S2S_INTERRUPTED_BIT | S2S_USER_INTERRUPT_BIT;

static void s2s_feed_reset(s2s_context_t* ctx)
{
    ctx->msg_buf_len = 0;
    ctx->msg_truncated = false;
    ctx->key_match = 0;
    ctx->in_audio = false;
    ctx->audio_drop = false;
    ctx->audio_warned = false;
    ctx->b64_acc = 0;
    ctx->b64_count = 0;
    ctx->dec_len = 0;
}

static void s2s_audio_flush(s2s_context_t* ctx)
{
    size_t len = ctx->dec_len;
    ctx->dec_len = 0;
    if (len == 0 || ctx->audio_drop || !ctx->spk_ring_buffer)
        return;

    ctx->payload_decoded += len;
    ctx->crc_received = esp_rom_crc32_le(ctx->crc_received, s_dec_buf, len);

    // Bounded wait + stop-bit check to avoid deadlock if the consumer
    // task has exited while the buffer is full.
    for (int tries = 0; tries < 20; tries++)
    {
        if (xEventGroupGetBits(ctx->event_group) & S2S_AUDIO_DROP_MASK)
        {
            ctx->audio_drop = true;
            return;
        }
        if (xRingbufferSend(ctx->spk_ring_buffer, s_dec_buf, len, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            ctx->rb_bytes_written += len;
            return;
        }
    }
    if (!ctx->audio_warned)
    {
        ESP_LOGW(TAG, "Audio ring buffer full, dropping audio");
        ctx->audio_warned = true;
    }
}

static void s2s_audio_begin(s2s_context_t* ctx)
{
    ctx->in_audio = true;
    ctx->b64_acc = 0;
    ctx->b64_count = 0;
    ctx->dec_len = 0;
    ctx->audio_drop = xEventGroupGetBits(ctx->event_group) & S2S_AUDIO_DROP_MASK;
    if (!ctx->audio_drop)
        xEventGroupSetBits(ctx->event_group, S2S_MODEL_SPEAKING_BIT);
}

static void s2s_audio_end(s2s_context_t* ctx)
{
    // leftover of an unpadded or '='-padded final group
    if (ctx->b64_count >= 2)
    {
        uint32_t n = ctx->b64_acc << (6 * (4 - ctx->b64_count));
        s_dec_buf[ctx->dec_len++] = n >> 16;
        if (ctx->b64_count == 3)
            s_dec_buf[ctx->dec_len++] = (n >> 8) & 0xFF;
    }
    s2s_audio_flush(ctx);
    ctx->in_audio = false;
}

static inline void s2s_msg_append(s2s_context_t* ctx, char c)
{
    if (ctx->msg_buf_len < MSG_BUF_CAPACITY - 1)
        ctx->msg_buf[ctx->msg_buf_len++] = c;
    else
        ctx->msg_truncated = true;
}

static void s2s_feed(s2s_context_t* ctx, const char* data, size_t len)
{
    // "data" key, then optional whitespace around the colon, then the opening quote
    static const char KEY[] = "\"data\"";
    static const uint8_t KEY_LEN = sizeof(KEY) - 1;

    for (size_t i = 0; i < len; i++)
    {
        const char c = data[i];

        if (ctx->in_audio)
        {
            if (c == '"')
            {
                s2s_audio_end(ctx);
                s2s_msg_append(ctx, c);
                continue;
            }
            const uint8_t v = b64_lut[(uint8_t)c];
            if (v >= 64)
                continue; // padding, line breaks
            ctx->b64_acc = (ctx->b64_acc << 6) | v;
            if (++ctx->b64_count == 4)
            {
                s_dec_buf[ctx->dec_len++] = ctx->b64_acc >> 16;
                s_dec_buf[ctx->dec_len++] = (ctx->b64_acc >> 8) & 0xFF;
                s_dec_buf[ctx->dec_len++] = ctx->b64_acc & 0xFF;
                ctx->b64_acc = 0;
                ctx->b64_count = 0;
                if (ctx->dec_len > AUDIO_DECODE_CHUNK - 3)
                    s2s_audio_flush(ctx);
            }
            continue;
        }

        s2s_msg_append(ctx, c);

        const bool is_space = (c == ' ' || c == '\n' || c == '\r' || c == '\t');
        if (ctx->key_match < KEY_LEN)
        {
            ctx->key_match = (c == KEY[ctx->key_match]) ? ctx->key_match + 1 : (c == '"' ? 1 : 0);
        }
        else if (ctx->key_match == KEY_LEN)
        {
            // after the key: waiting for the colon
            if (c == ':')
                ctx->key_match++;
            else if (!is_space)
                ctx->key_match = (c == '"') ? 1 : 0;
        }
        else
        {
            // after the colon: waiting for the string to open
            if (c == '"')
            {
                ctx->key_match = 0;
                // only the audio payload is streamed; any other "data" string stays in the JSON
                ctx->msg_buf[ctx->msg_buf_len] = '\0';
                if (strstr(ctx->msg_buf, "\"inlineData\""))
                    s2s_audio_begin(ctx);
            }
            else if (!is_space)
            {
                ctx->key_match = 0;
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════
// Extract a JSON string value by key using raw string scan.
// Returns pointer into buf and sets *len. No allocation.
// ═══════════════════════════════════════════════════════════
// Process a complete JSON message from the server
// ═══════════════════════════════════════════════════════════
static void s2s_process_message(s2s_context_t* ctx)
{
    const char* buf = ctx->msg_buf;
    int len = ctx->msg_buf_len;
    const char* tp;
    int tl;

    // Audio never reaches this function: s2s_feed() has already decoded and queued it,
    // leaving an empty inlineData.data string in the message.

    // ── Session resumption (frequent) ──
    if (mjson_find(buf, len, "$.sessionResumptionUpdate", &tp, &tl) != MJSON_TOK_INVALID)
    {
        int resumable = 0;
        mjson_get_bool(buf, len, "$.sessionResumptionUpdate.resumable", &resumable);
        if (resumable)
        {
            char handle[128];
            int hn = mjson_get_string(buf, len, "$.sessionResumptionUpdate.newHandle", handle, sizeof(handle));
            if (hn > 0)
            {
                if (xSemaphoreTake(ctx->shared->mutex, pdMS_TO_TICKS(20)) == pdTRUE)
                {
                    ctx->shared->session_handle.assign(handle, hn);
                    xSemaphoreGive(ctx->shared->mutex);
                }
            }
        }
        return;
    }

    // ── Transcriptions ──
    // These can share a message with turnComplete / generationComplete, so they are
    // collected first and do not end the handling of the message.
    {
        char txt_buf[256];
        int n = mjson_get_string(buf, len, "$.serverContent.inputTranscription.text", txt_buf, sizeof(txt_buf));
        if (n > 0 && xSemaphoreTake(ctx->shared->mutex, pdMS_TO_TICKS(50)) == pdTRUE)
        {
            ctx->shared->input_transcript.append(txt_buf, n);
            xSemaphoreGive(ctx->shared->mutex);
            xEventGroupSetBits(ctx->event_group, S2S_INPUT_TRANSCRIPT_BIT);
        }
        n = mjson_get_string(buf, len, "$.serverContent.outputTranscription.text", txt_buf, sizeof(txt_buf));
        if (n > 0 && xSemaphoreTake(ctx->shared->mutex, pdMS_TO_TICKS(50)) == pdTRUE)
        {
            ctx->shared->output_transcript.append(txt_buf, n);
            xSemaphoreGive(ctx->shared->mutex);
            xEventGroupSetBits(ctx->event_group, S2S_OUTPUT_TRANSCRIPT_BIT);
        }
    }

    // ── Turn / interruption signals ──
    if (mjson_find(buf, len, "$.serverContent.turnComplete", &tp, &tl) != MJSON_TOK_INVALID)
    {
        ESP_LOGI(TAG, "Turn complete");
        xEventGroupSetBits(ctx->event_group, S2S_TURN_COMPLETE_BIT);
        return;
    }
    if (mjson_find(buf, len, "$.serverContent.interrupted", &tp, &tl) != MJSON_TOK_INVALID)
    {
        ESP_LOGW(TAG, "Model interrupted");
        xEventGroupSetBits(ctx->event_group, S2S_INTERRUPTED_BIT);
        return;
    }
    if (mjson_find(buf, len, "$.serverContent.generationComplete", &tp, &tl) != MJSON_TOK_INVALID)
        return;

    // ── Setup complete ──
    if (mjson_find(buf, len, "$.setupComplete", &tp, &tl) != MJSON_TOK_INVALID)
    {
        ESP_LOGI(TAG, "Setup complete");
        xEventGroupSetBits(ctx->event_group, S2S_SETUP_COMPLETE_BIT);
        return;
    }

    // ── Go away ──
    if (mjson_find(buf, len, "$.goAway", &tp, &tl) != MJSON_TOK_INVALID)
    {
        ESP_LOGW(TAG, "Server sent goAway, will reconnect");
        xEventGroupSetBits(ctx->event_group, S2S_ERROR_BIT);
        return;
    }

    char txt_buf[256];
    int n;

    // ── Error ──
    if (mjson_find(buf, len, "$.error", &tp, &tl) != MJSON_TOK_INVALID)
    {
        double code = 0;
        mjson_get_number(buf, len, "$.error.code", &code);
        n = mjson_get_string(buf, len, "$.error.message", txt_buf, sizeof(txt_buf));
        ESP_LOGE(TAG, "API error: code=%d msg=%s", (int)code, n > 0 ? txt_buf : "unknown");
        int ic = (int)code;
        bool fatal = (ic == 400 || ic == 401 || ic == 403 || ic == 404);
        if (fatal)
            set_fatal_error(ctx, n > 0 ? txt_buf : "API request rejected");
        else
            xEventGroupSetBits(ctx->event_group, S2S_ERROR_BIT);
        return;
    }

    // ── Ignore remaining known-harmless messages ──
    if (mjson_find(buf, len, "$.serverContent", &tp, &tl) != MJSON_TOK_INVALID ||
        mjson_find(buf, len, "$.usageMetadata", &tp, &tl) != MJSON_TOK_INVALID)
        return;

    ESP_LOGW(TAG, "Unrecognized msg: %.200s", buf);
}

// ═══════════════════════════════════════════════════════════
// Send setup message
// ═══════════════════════════════════════════════════════════
static bool s2s_send_setup(s2s_context_t* ctx)
{
    std::string model_path = "models/" + ctx->model;

    std::string handle;
    if (xSemaphoreTake(ctx->shared->mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        handle = ctx->shared->session_handle;
        xSemaphoreGive(ctx->shared->mutex);
    }
    if (!handle.empty())
        ESP_LOGI(TAG, "Resuming session");

    // Build sub-snippets using mjson %Q for proper JSON string escaping
    char voice_snippet[200] = "";
    if (!ctx->voice.empty())
    {
        mjson_snprintf(voice_snippet,
                       sizeof(voice_snippet),
                       ",\"speechConfig\":{\"voiceConfig\":{\"prebuiltVoiceConfig\":{\"voiceName\":%Q}}}",
                       ctx->voice.c_str());
    }

    char rules_snippet[1200] = "";
    if (!ctx->rules.empty())
    {
        mjson_snprintf(rules_snippet,
                       sizeof(rules_snippet),
                       "\"systemInstruction\":{\"parts\":[{\"text\":%Q}]},",
                       ctx->rules.c_str());
    }

    char handle_snippet[200] = "";
    if (!handle.empty())
    {
        mjson_snprintf(handle_snippet, sizeof(handle_snippet), "\"handle\":%Q", handle.c_str());
    }

    // Assemble final JSON into the static send buffer
    char* buf = s_json_buf;
    int cap = (int)sizeof(s_json_buf);
    int n = snprintf(buf,
                     cap,
                     "{\"setup\":{"
                     "\"model\":\"%s\","
                     "\"generationConfig\":{\"responseModalities\":[\"AUDIO\"]%s},"
                     "%s"
                     "\"inputAudioTranscription\":{},"
                     "\"outputAudioTranscription\":{},"
                     "\"sessionResumption\":{%s}"
                     "}}",
                     model_path.c_str(),
                     voice_snippet,
                     rules_snippet,
                     handle_snippet);

    if (n <= 0 || n >= cap)
    {
        ESP_LOGE(TAG, "Setup JSON too large");
        return false;
    }

    ESP_LOGI(TAG, "Sending setup (%d bytes)", n);
    int sent = esp_websocket_client_send_text(ctx->ws_client, buf, n, pdMS_TO_TICKS(5000));
    return sent > 0;
}

// ═══════════════════════════════════════════════════════════
// Send one audio chunk (base64-encoded PCM inside JSON)
// ═══════════════════════════════════════════════════════════
static bool s2s_send_audio(s2s_context_t* ctx, const uint8_t* pcm, size_t pcm_len)
{
    memcpy(s_json_buf, JSON_AUDIO_PREFIX, JSON_PREFIX_LEN);

    size_t b64_len = 0;
    int ret = mbedtls_base64_encode((unsigned char*)s_json_buf + JSON_PREFIX_LEN, MAX_B64_LEN, &b64_len, pcm, pcm_len);
    if (ret != 0)
        return false;

    memcpy(s_json_buf + JSON_PREFIX_LEN + b64_len, JSON_AUDIO_SUFFIX, JSON_SUFFIX_LEN);
    size_t total = JSON_PREFIX_LEN + b64_len + JSON_SUFFIX_LEN;
    s_json_buf[total] = '\0';

    int sent = esp_websocket_client_send_text(ctx->ws_client, s_json_buf, total, pdMS_TO_TICKS(3000));
    return sent == (int)total;
}

// ═══════════════════════════════════════════════════════════
// Main S2S client task
// ═══════════════════════════════════════════════════════════
static void s2s_client_task(void* parameter)
{
    s2s_context_t* ctx = (s2s_context_t*)parameter;
    xEventGroupSetBits(ctx->event_group, S2S_TASK_STARTED_BIT);
    ESP_LOGI(TAG, "S2S task started, heap=%lu", (unsigned long)esp_get_free_heap_size());

    // ── Wait for WebSocket connection ──
    EventBits_t bits =
        xEventGroupWaitBits(ctx->event_group, S2S_CONNECTED_BIT | S2S_STOP_REQUEST_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
    if (!(bits & S2S_CONNECTED_BIT))
    {
        ESP_LOGE(TAG, "WS connect timeout");
        xEventGroupSetBits(ctx->event_group, S2S_ERROR_BIT);
        goto exit;
    }

    // ── Send setup ──
    if (!s2s_send_setup(ctx))
    {
        ESP_LOGE(TAG, "Failed to send setup");
        xEventGroupSetBits(ctx->event_group, S2S_ERROR_BIT);
        goto exit;
    }

    // ── Wait for setupComplete ──
    bits = xEventGroupWaitBits(ctx->event_group,
                               S2S_SETUP_COMPLETE_BIT | S2S_STOP_REQUEST_BIT | S2S_ERROR_BIT | S2S_FATAL_ERROR_BIT,
                               pdFALSE,
                               pdFALSE,
                               pdMS_TO_TICKS(15000));
    if (!(bits & S2S_SETUP_COMPLETE_BIT))
    {
        if (!(bits & S2S_FATAL_ERROR_BIT))
        {
            ESP_LOGE(TAG, "Setup timeout");
            xEventGroupSetBits(ctx->event_group, S2S_ERROR_BIT);
        }
        goto exit;
    }

    ESP_LOGI(TAG, "Session ready, entering main loop");

    // ═══ Main conversation loop ═══
    while (!(xEventGroupGetBits(ctx->event_group) & (S2S_STOP_REQUEST_BIT | S2S_ERROR_BIT)))
    {
        // ──────────────────────────────
        // LISTENING PHASE: capture mic → send audio
        // ──────────────────────────────
        xEventGroupClearBits(ctx->event_group,
                             S2S_MODEL_SPEAKING_BIT | S2S_TURN_COMPLETE_BIT | S2S_INTERRUPTED_BIT | S2S_USER_INTERRUPT_BIT);

        // Flush any stale audio left in the speaker ring buffer
        {
            size_t flush_size;
            void* flush_item;
            while ((flush_item = xRingbufferReceiveUpTo(ctx->spk_ring_buffer, &flush_size, 0, 4096)) != NULL)
                vRingbufferReturnItem(ctx->spk_ring_buffer, flush_item);
        }
        ctx->payload_decoded = 0;
        ctx->rb_bytes_written = 0;
        ctx->rb_bytes_read = 0;
        ctx->crc_received = 0;
        ctx->crc_played = 0;

        ctx->hal->speaker()->end();
        vTaskDelay(pdMS_TO_TICKS(20));
        ctx->hal->mic()->begin();
        xEventGroupSetBits(ctx->event_group, S2S_LISTENING_BIT);
        ESP_LOGI(TAG, "Listening...");

        // Circular mic buffer pattern (matches M5Unified DMA pipeline)
        int rec_wr = 0, rec_queued = 0;
        bool mic_warmed = false;
        int warmup_skip = 0;
        size_t pcm_pos = 0;

        while (true)
        {
            bits = xEventGroupGetBits(ctx->event_group);
            if (bits & (S2S_STOP_REQUEST_BIT | S2S_ERROR_BIT | S2S_MODEL_SPEAKING_BIT | S2S_USER_INTERRUPT_BIT))
                break;

            if (!ctx->hal->mic()->record(s_rec_buf[rec_wr], MIC_BUF_SAMPLES, S2S_MIC_SAMPLE_RATE, false))
                continue;

            rec_queued++;
            rec_wr = (rec_wr + 1) % MIC_REC_BLOCKS;
            if (rec_queued < MIC_REC_BLOCKS)
                continue;

            int16_t* frame = s_rec_buf[rec_wr]; // slot freed 3 calls ago

            // Skip codec warmup silence
            if (!mic_warmed)
            {
                bool silent = true;
                for (int i = 0; i < MIC_BUF_SAMPLES; i++)
                    if (frame[i] != 0)
                    {
                        silent = false;
                        break;
                    }
                if (silent)
                    continue;
                if (warmup_skip++ < 2)
                    continue;
                mic_warmed = true;
            }

            // Accumulate into send buffer
            size_t frame_bytes = MIC_BUF_SAMPLES * sizeof(int16_t);
            if (pcm_pos + frame_bytes > PCM_SEND_BYTES)
                frame_bytes = PCM_SEND_BYTES - pcm_pos;
            memcpy(s_pcm_buf + pcm_pos, frame, frame_bytes);
            pcm_pos += frame_bytes;

            // Send when buffer full
            if (pcm_pos >= PCM_SEND_BYTES)
            {
                s2s_send_audio(ctx, s_pcm_buf, pcm_pos);
                pcm_pos = 0;
            }
        }

        // Flush remaining PCM
        if (pcm_pos > 0)
            s2s_send_audio(ctx, s_pcm_buf, pcm_pos);

        // Tell server mic stream paused
        esp_websocket_client_send_text(ctx->ws_client, JSON_STREAM_END, sizeof(JSON_STREAM_END) - 1, pdMS_TO_TICKS(2000));

        ctx->hal->mic()->end();
        xEventGroupClearBits(ctx->event_group, S2S_LISTENING_BIT);

        if (xEventGroupGetBits(ctx->event_group) & (S2S_STOP_REQUEST_BIT | S2S_ERROR_BIT))
            break;

        // ──────────────────────────────
        // SPEAKING PHASE: drain ring buffer → speaker
        // ──────────────────────────────

        // Pre-buffer: let the ring buffer fill before starting the speaker
        // to absorb network burst variability and prevent early underruns.
        // {
        //     const size_t PRE_BUFFER_BYTES = 12000; // ~250ms at 24kHz 16-bit
        //     TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
        //     while (xTaskGetTickCount() < deadline)
        //     {
        //         bits = xEventGroupGetBits(ctx->event_group);
        //         if (bits & (S2S_STOP_REQUEST_BIT | S2S_ERROR_BIT |
        //                     S2S_INTERRUPTED_BIT | S2S_USER_INTERRUPT_BIT))
        //             break;
        //         if (bits & S2S_TURN_COMPLETE_BIT)
        //             break;
        //         size_t free_bytes = xRingbufferGetCurFreeSize(ctx->spk_ring_buffer);
        //         if (AUDIO_BUFFER_SIZE - free_bytes >= PRE_BUFFER_BYTES)
        //             break;
        //         vTaskDelay(pdMS_TO_TICKS(20));
        //     }
        // }

        vTaskDelay(pdMS_TO_TICKS(20));
        ctx->hal->speaker()->begin();
        ctx->hal->speaker()->setVolume(255);
        ctx->hal->speaker()->setChannelVolume(AUDIO_CHANNEL, ctx->volume);
        // Let the codec volume ramp and amplifier come up before the first samples,
        // otherwise the start of the first word is played muted. Audio keeps
        // collecting in the ring buffer meanwhile, so nothing is skipped.
        vTaskDelay(pdMS_TO_TICKS(150));
        ESP_LOGI(TAG, "Playing response...");

        bool turn_ended = false;
        uint32_t total_bytes = 0;
        uint32_t underruns = 0;
        uint32_t play_start = millis();
        uint8_t carry_byte = 0;
        bool have_carry = false;

        // Triple buffer: playRaw stores a pointer (no copy), so we must keep
        // the data alive until the speaker is done. With 3 buffers and the
        // isPlaying()>=2 gate, the speaker can hold current + next while we
        // safely write to the third.
        static int16_t play_buf[3][PLAY_CHUNK_SIZE / sizeof(int16_t)];
        int buf_idx = 0;

        while (true)
        {
            bits = xEventGroupGetBits(ctx->event_group);
            if (bits & (S2S_STOP_REQUEST_BIT | S2S_ERROR_BIT | S2S_FATAL_ERROR_BIT))
                break;
            if (bits & (S2S_INTERRUPTED_BIT | S2S_USER_INTERRUPT_BIT))
                break;
            if (bits & S2S_TURN_COMPLETE_BIT)
                turn_ended = true;

            size_t item_size = 0;
            uint8_t* item = (uint8_t*)
                xRingbufferReceiveUpTo(ctx->spk_ring_buffer, &item_size, pdMS_TO_TICKS(turn_ended ? 10 : 200), PLAY_CHUNK_SIZE);

            if (item)
            {
                ctx->rb_bytes_read += item_size;
                uint8_t* pcm = item;
                size_t pcm_len = item_size;

                // Reconstruct split sample from previous chunk's trailing byte
                size_t dst_off = 0;
                if (have_carry && pcm_len > 0)
                {
                    int16_t sample = (int16_t)(carry_byte | ((uint16_t)pcm[0] << 8));
                    play_buf[buf_idx][0] = sample;
                    dst_off = 1;
                    pcm++;
                    pcm_len--;
                    have_carry = false;
                }

                // Save trailing byte if odd
                if (pcm_len & 1)
                {
                    carry_byte = pcm[pcm_len - 1];
                    have_carry = true;
                    pcm_len--;
                }

                // Copy PCM to play buffer and release ring buffer item
                if (pcm_len > 0)
                    memcpy(&play_buf[buf_idx][dst_off], pcm, pcm_len);
                vRingbufferReturnItem(ctx->spk_ring_buffer, item);

                size_t total_samples = dst_off + pcm_len / sizeof(int16_t);
                total_bytes += total_samples * sizeof(int16_t);

                if (total_samples > 0)
                {
                    ctx->crc_played =
                        esp_rom_crc32_le(ctx->crc_played, (const uint8_t*)play_buf[buf_idx], total_samples * sizeof(int16_t));

                    while (ctx->hal->speaker()->isPlaying(AUDIO_CHANNEL) >= 2)
                        vTaskDelay(1);

                    ctx->hal->speaker()
                        ->playRaw(play_buf[buf_idx], total_samples, S2S_SPK_SAMPLE_RATE, false, 1, AUDIO_CHANNEL, false);
                    buf_idx = (buf_idx + 1) % 3;
                }
            }
            else if (turn_ended)
            {
                break;
            }
            else
            {
                underruns++;
                ESP_LOGW(TAG,
                         "Speaker underrun #%lu (rb empty, %lums into playback)",
                         (unsigned long)underruns,
                         (unsigned long)(millis() - play_start));
            }
        }
        ESP_LOGI(TAG,
                 "Playback stats: decoded=%lu rb_wr=%lu rb_rd=%lu played=%lu underruns=%lu %lums%s",
                 (unsigned long)ctx->payload_decoded,
                 (unsigned long)ctx->rb_bytes_written,
                 (unsigned long)ctx->rb_bytes_read,
                 (unsigned long)total_bytes,
                 (unsigned long)underruns,
                 (unsigned long)(millis() - play_start),
                 (ctx->payload_decoded != ctx->rb_bytes_written || ctx->rb_bytes_written != ctx->rb_bytes_read ||
                  ctx->rb_bytes_read != total_bytes)
                     ? " **MISMATCH**"
                     : "");

        ESP_LOGI(TAG,
                 "CRC check: received=0x%08lx played=0x%08lx %s",
                 (unsigned long)ctx->crc_received,
                 (unsigned long)ctx->crc_played,
                 (ctx->crc_received == ctx->crc_played) ? "MATCH" : "**MISMATCH**");

        if (xEventGroupGetBits(ctx->event_group) & (S2S_INTERRUPTED_BIT | S2S_USER_INTERRUPT_BIT))
        {
            ctx->hal->speaker()->stop();
            size_t discard_size;
            void* discard;
            while ((discard = xRingbufferReceiveUpTo(ctx->spk_ring_buffer, &discard_size, 0, 4096)) != NULL)
                vRingbufferReturnItem(ctx->spk_ring_buffer, discard);
        }
        else
        {
            while (ctx->hal->speaker()->isPlaying(AUDIO_CHANNEL))
                vTaskDelay(1);
        }
        ctx->hal->speaker()->stop();
        ESP_LOGI(TAG, "Turn done");
    }

exit:
    // Ensure audio subsystem is in a clean state
    ctx->hal->mic()->end();
    vTaskDelay(pdMS_TO_TICKS(20));
    ctx->hal->speaker()->begin();

    xEventGroupClearBits(ctx->event_group, S2S_LISTENING_BIT | S2S_MODEL_SPEAKING_BIT);
    xEventGroupSetBits(ctx->event_group, S2S_TASK_STOPPED_BIT);
    ESP_LOGI(TAG, "S2S task exiting, heap=%lu", (unsigned long)esp_get_free_heap_size());
    vTaskDelete(NULL);
}

// ═══════════════════════════════════════════════════════════
// Public API
// ═══════════════════════════════════════════════════════════

bool s2s_start(HAL::Hal* hal,
               EventGroupHandle_t event_group,
               S2SSharedData* shared,
               const std::string& api_key,
               const std::string& model,
               const std::string& voice,
               const std::string& rules,
               int32_t volume)
{
    ESP_LOGI(TAG, "Starting S2S session...");

    if (api_key.empty() || model.empty())
    {
        ESP_LOGE(TAG, "API key or model not set");
        return false;
    }

    // Clear bits
    xEventGroupClearBits(event_group, 0xFFFFFF);

    memset(&s2s_ctx, 0, sizeof(s2s_ctx));
    s2s_ctx.hal = hal;
    s2s_ctx.event_group = event_group;
    s2s_ctx.shared = shared;
    s2s_ctx.model = model;
    s2s_ctx.api_key = api_key;
    s2s_ctx.voice = voice;
    s2s_ctx.rules = rules;
    s2s_ctx.volume = volume;

    s2s_ctx.msg_buf = s_msg_buf;
    s2s_ctx.msg_buf_len = 0;

    // Create speaker ring buffer using the shared static audio buffer
    s2s_ctx.spk_ring_buffer =
        xRingbufferCreateStatic(AUDIO_BUFFER_SIZE, RINGBUF_TYPE_BYTEBUF, audio_buffer, &s2s_ctx.spk_rb_struct);
    if (!s2s_ctx.spk_ring_buffer)
    {
        ESP_LOGE(TAG, "Failed to create speaker ring buffer");
        goto fail;
    }

    // Build WebSocket URL with API key
    {
        std::string url = std::format("{}?key={}", S2S_WS_BASE_URL, api_key);

        esp_websocket_client_config_t ws_cfg = {};
        ws_cfg.uri = url.c_str();
        ws_cfg.task_prio = 5;
        ws_cfg.buffer_size = WS_BUFFER_SIZE;
        ws_cfg.reconnect_timeout_ms = 0; // we handle reconnection ourselves
        ws_cfg.network_timeout_ms = 10000;

        s2s_ctx.ws_client = esp_websocket_client_init(&ws_cfg);
        if (!s2s_ctx.ws_client)
        {
            ESP_LOGE(TAG, "Failed to init WS client");
            goto fail;
        }
    }

    esp_websocket_register_events(s2s_ctx.ws_client, WEBSOCKET_EVENT_ANY, s2s_ws_event_handler, &s2s_ctx);

    if (esp_websocket_client_start(s2s_ctx.ws_client) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start WS client");
        goto fail;
    }

    // Create the main S2S task
    if (xTaskCreate(s2s_client_task, "s2s_task", S2S_TASK_STACK_SIZE, &s2s_ctx, S2S_TASK_PRIORITY, &s2s_ctx.task_handle) !=
        pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create S2S task");
        goto fail;
    }

    ESP_LOGI(TAG, "S2S session starting, heap=%lu", (unsigned long)esp_get_free_heap_size());
    return true;

fail:
    if (s2s_ctx.ws_client)
    {
        esp_websocket_client_stop(s2s_ctx.ws_client);
        esp_websocket_client_destroy(s2s_ctx.ws_client);
        s2s_ctx.ws_client = NULL;
    }
    if (s2s_ctx.spk_ring_buffer)
    {
        vRingbufferDelete(s2s_ctx.spk_ring_buffer);
        s2s_ctx.spk_ring_buffer = NULL;
    }
    s2s_ctx.msg_buf = NULL;
    return false;
}

void s2s_stop(void)
{
    ESP_LOGI(TAG, "Stopping S2S session...");
    xEventGroupSetBits(s2s_ctx.event_group, S2S_STOP_REQUEST_BIT);

    // Wait for the task to exit
    xEventGroupWaitBits(s2s_ctx.event_group, S2S_TASK_STOPPED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(5000));

    // Drain the speaker ring buffer so any WS-task producer waiting on
    // xRingbufferSend is released before we try to stop the WS client.
    if (s2s_ctx.spk_ring_buffer)
    {
        size_t drain_size;
        void* drain_item;
        while ((drain_item = xRingbufferReceiveUpTo(s2s_ctx.spk_ring_buffer, &drain_size, 0, 4096)) != NULL)
            vRingbufferReturnItem(s2s_ctx.spk_ring_buffer, drain_item);
    }

    if (s2s_ctx.ws_client)
    {
        esp_websocket_client_stop(s2s_ctx.ws_client);
        esp_websocket_client_destroy(s2s_ctx.ws_client);
        s2s_ctx.ws_client = NULL;
    }

    if (s2s_ctx.spk_ring_buffer)
    {
        vRingbufferDelete(s2s_ctx.spk_ring_buffer);
        s2s_ctx.spk_ring_buffer = NULL;
    }

    s2s_ctx.msg_buf = NULL;
    s2s_ctx.task_handle = NULL;

    // Release heap held by string copies
    std::string().swap(s2s_ctx.model);
    std::string().swap(s2s_ctx.api_key);
    std::string().swap(s2s_ctx.voice);
    std::string().swap(s2s_ctx.rules);

    ESP_LOGI(TAG, "S2S stopped, heap=%lu", (unsigned long)esp_get_free_heap_size());
}
