#include "s2s_client.h"
#include "audio_buffer.h"
#include "esp_websocket_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "mjson.h"
#include "mbedtls/base64.h"
#include "freertos/ringbuf.h"
#include "hal_cardputer.h"
#if HAL_USE_WIFI
#include "esp_wifi.h"
#endif
#include <cmath>
#include <cstring>
#include "esp_rom_crc.h"
#include <format>

static const char* TAG = "S2S";

#define S2S_WS_BASE_URL                                                                                                        \
    "wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent"
#define S2S_WS_URI_BUF_SIZE 512

#define MIC_BUF_SAMPLES 256
#define MIC_REC_BLOCKS 3
#define PCM_SEND_BYTES 3200 // 100ms at 16kHz 16-bit mono
#define PLAY_CHUNK_SIZE 2048

#define WS_BUFFER_SIZE 4096
#define MSG_BUF_CAPACITY (1024 * 4)
#define B64_STREAM_BUF_SIZE 512

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
static uint8_t s_b64_buf[B64_STREAM_BUF_SIZE];
static volatile bool s_s2s_muted = false;

enum class WsStreamState : uint8_t
{
    NORMAL,
    IN_STRING,
    IN_DATA_COLON,
    IN_DATA_VALUE,
    IN_AUDIO_B64,
};

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

    WsStreamState ws_state;
    bool str_escaped;
    bool inline_data_pending;
    bool msg_had_audio;
    int brace_depth;
    int inline_data_depth;
    char key_buf[16];
    uint8_t key_len;
    size_t b64_len;

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
// In-place base64 decoder (zero allocation)
// Write pointer always trails read pointer (3 out per 4 in),
// so decoding over the source buffer is safe.
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

static size_t base64_decode_inplace(uint8_t* data, size_t len)
{
    while (len > 0 && (data[len - 1] == '=' || data[len - 1] == '\n' || data[len - 1] == '\r'))
        len--;

    uint8_t* out = data;
    size_t i = 0;
    while (i + 3 < len)
    {
        uint32_t n =
            (b64_lut[data[i]] << 18) | (b64_lut[data[i + 1]] << 12) | (b64_lut[data[i + 2]] << 6) | b64_lut[data[i + 3]];
        *out++ = n >> 16;
        *out++ = (n >> 8) & 0xFF;
        *out++ = n & 0xFF;
        i += 4;
    }
    if (i + 1 < len)
    {
        uint32_t n = (b64_lut[data[i]] << 18) | (b64_lut[data[i + 1]] << 12);
        *out++ = n >> 16;
        if (i + 2 < len)
        {
            n |= b64_lut[data[i + 2]] << 6;
            *out++ = (n >> 8) & 0xFF;
        }
    }
    return (size_t)(out - data);
}

static inline void s2s_append_meta(s2s_context_t* ctx, char c)
{
    if (ctx->msg_buf_len < MSG_BUF_CAPACITY - 1)
        ctx->msg_buf[ctx->msg_buf_len++] = c;
}

static void s2s_flush_b64_chunk(s2s_context_t* ctx)
{
    if (ctx->b64_len == 0)
        return;

    const EventBits_t drop_mask =
        S2S_STOP_REQUEST_BIT | S2S_ERROR_BIT | S2S_FATAL_ERROR_BIT | S2S_INTERRUPTED_BIT | S2S_USER_INTERRUPT_BIT;
    bool drop = (xEventGroupGetBits(ctx->event_group) & drop_mask) != 0;
    if (!drop)
        xEventGroupSetBits(ctx->event_group, S2S_MODEL_SPEAKING_BIT);

    size_t decoded_len = base64_decode_inplace(s_b64_buf, ctx->b64_len);
    ctx->b64_len = 0;

    if (decoded_len == 0 || drop || !ctx->spk_ring_buffer)
        return;

    ctx->payload_decoded += decoded_len;
    ctx->crc_received = esp_rom_crc32_le(ctx->crc_received, s_b64_buf, decoded_len);

    // Bounded wait + stop-bit check to avoid deadlock if the consumer
    // task has exited while the buffer is full.
    bool sent = false;
    for (int tries = 0; tries < 20; tries++)
    {
        if (xEventGroupGetBits(ctx->event_group) & drop_mask)
        {
            drop = true;
            break;
        }
        if (xRingbufferSend(ctx->spk_ring_buffer, s_b64_buf, decoded_len, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            sent = true;
            break;
        }
    }
    if (sent)
        ctx->rb_bytes_written += decoded_len;
    else if (!drop)
        ESP_LOGW(TAG, "Audio ring buffer full, dropping %d bytes", (int)decoded_len);
}

static void s2s_stream_reset(s2s_context_t* ctx)
{
    ctx->msg_buf_len = 0;
    ctx->ws_state = WsStreamState::NORMAL;
    ctx->str_escaped = false;
    ctx->inline_data_pending = false;
    ctx->msg_had_audio = false;
    ctx->brace_depth = 0;
    ctx->inline_data_depth = -1;
    ctx->key_len = 0;
    ctx->b64_len = 0;
}

static void s2s_stream_feed(s2s_context_t* ctx, const char* ptr, size_t len)
{
    for (size_t i = 0; i < len; ++i)
    {
        char c = ptr[i];
        switch (ctx->ws_state)
        {
        case WsStreamState::NORMAL:
            s2s_append_meta(ctx, c);
            if (c == '{')
            {
                ctx->brace_depth++;
                if (ctx->inline_data_pending)
                {
                    ctx->inline_data_depth = ctx->brace_depth;
                    ctx->inline_data_pending = false;
                }
            }
            else if (c == '}')
            {
                if (ctx->brace_depth == ctx->inline_data_depth)
                    ctx->inline_data_depth = -1;
                if (ctx->brace_depth > 0)
                    ctx->brace_depth--;
                ctx->inline_data_pending = false;
            }
            else if (c == '"')
            {
                ctx->ws_state = WsStreamState::IN_STRING;
                ctx->str_escaped = false;
                ctx->key_len = 0;
            }
            else if (c != ':' && c != ' ' && c != '\t' && c != '\r' && c != '\n')
            {
                ctx->inline_data_pending = false;
            }
            break;

        case WsStreamState::IN_STRING:
            s2s_append_meta(ctx, c);
            if (ctx->str_escaped)
            {
                ctx->str_escaped = false;
                if (ctx->key_len < sizeof(ctx->key_buf))
                    ctx->key_buf[ctx->key_len++] = c;
                else
                    ctx->key_len = sizeof(ctx->key_buf) + 1;
            }
            else if (c == '\\')
            {
                ctx->str_escaped = true;
                ctx->key_len = sizeof(ctx->key_buf) + 1;
            }
            else if (c == '"')
            {
                if (ctx->key_len == 10 && memcmp(ctx->key_buf, "inlineData", 10) == 0)
                {
                    ctx->inline_data_pending = true;
                    ctx->ws_state = WsStreamState::NORMAL;
                }
                else if (ctx->inline_data_depth >= 0 && ctx->brace_depth == ctx->inline_data_depth &&
                         ctx->key_len == 4 && memcmp(ctx->key_buf, "data", 4) == 0)
                {
                    ctx->ws_state = WsStreamState::IN_DATA_COLON;
                }
                else
                {
                    ctx->ws_state = WsStreamState::NORMAL;
                }
            }
            else
            {
                if (ctx->key_len < sizeof(ctx->key_buf))
                    ctx->key_buf[ctx->key_len++] = c;
                else
                    ctx->key_len = sizeof(ctx->key_buf) + 1;
            }
            break;

        case WsStreamState::IN_DATA_COLON:
            s2s_append_meta(ctx, c);
            if (c == ':')
            {
                ctx->ws_state = WsStreamState::IN_DATA_VALUE;
            }
            else if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            {
                if (c == '}')
                {
                    if (ctx->brace_depth == ctx->inline_data_depth)
                        ctx->inline_data_depth = -1;
                    if (ctx->brace_depth > 0)
                        ctx->brace_depth--;
                    ctx->ws_state = WsStreamState::NORMAL;
                }
                else if (c == '{')
                {
                    ctx->brace_depth++;
                    ctx->ws_state = WsStreamState::NORMAL;
                }
                else if (c == '"')
                {
                    ctx->ws_state = WsStreamState::IN_STRING;
                    ctx->str_escaped = false;
                    ctx->key_len = 0;
                }
                else
                {
                    ctx->ws_state = WsStreamState::NORMAL;
                }
            }
            break;

        case WsStreamState::IN_DATA_VALUE:
            s2s_append_meta(ctx, c);
            if (c == '"')
            {
                ctx->ws_state = WsStreamState::IN_AUDIO_B64;
                ctx->b64_len = 0;
                ctx->msg_had_audio = true;
            }
            else if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            {
                if (c == '{')
                    ctx->brace_depth++;
                else if (c == '}')
                {
                    if (ctx->brace_depth == ctx->inline_data_depth)
                        ctx->inline_data_depth = -1;
                    if (ctx->brace_depth > 0)
                        ctx->brace_depth--;
                }
                ctx->ws_state = WsStreamState::NORMAL;
            }
            break;

        case WsStreamState::IN_AUDIO_B64:
            while (i < len)
            {
                char bc = ptr[i];
                if (bc == '"')
                {
                    s2s_flush_b64_chunk(ctx);
                    s2s_append_meta(ctx, '"');
                    ctx->ws_state = WsStreamState::NORMAL;
                    break;
                }
                i++;
                if (bc == '\r' || bc == '\n' || bc == ' ' || bc == '\t')
                    continue;
                s_b64_buf[ctx->b64_len++] = static_cast<uint8_t>(bc);
                if (ctx->b64_len == B64_STREAM_BUF_SIZE)
                {
                    s2s_flush_b64_chunk(ctx);
                }
            }
            break;
        }
    }
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
            if ((data->op_code == 0x01 || data->op_code == 0x02) && data->payload_offset == 0)
                s2s_stream_reset(ctx);

            s2s_stream_feed(ctx, data->data_ptr, (size_t)data->data_len);

            if (data->fin && (data->payload_offset + data->data_len >= data->payload_len))
            {
                if (ctx->b64_len > 0)
                    s2s_flush_b64_chunk(ctx);
                ctx->msg_buf[ctx->msg_buf_len] = '\0';
                ESP_LOGD(TAG, "WS msg: payload=%d meta_buf=%d", (int)data->payload_len, (int)ctx->msg_buf_len);
                s2s_process_message(ctx);
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
// Process a complete JSON message from the server (metadata only;
// inline audio chunks are already streamed in s2s_stream_feed)
// ═══════════════════════════════════════════════════════════
static void s2s_process_message(s2s_context_t* ctx)
{
    const char* buf = ctx->msg_buf;
    int len = ctx->msg_buf_len;
    const char* tp;
    int tl;

    char txt_buf[256];
    int n;
    bool handled = ctx->msg_had_audio;

    // ── Transcriptions (may arrive standalone or bundled with audio / turn signals) ──
    n = mjson_get_string(buf, len, "$.serverContent.outputTranscription.text", txt_buf, sizeof(txt_buf));
    if (n > 0)
    {
        if (xSemaphoreTake(ctx->shared->mutex, pdMS_TO_TICKS(50)) == pdTRUE)
        {
            ctx->shared->output_transcript.append(txt_buf, n);
            xSemaphoreGive(ctx->shared->mutex);
            xEventGroupSetBits(ctx->event_group, S2S_OUTPUT_TRANSCRIPT_BIT);
        }
        handled = true;
    }

    n = mjson_get_string(buf, len, "$.serverContent.inputTranscription.text", txt_buf, sizeof(txt_buf));
    if (n > 0)
    {
        if (xSemaphoreTake(ctx->shared->mutex, pdMS_TO_TICKS(50)) == pdTRUE)
        {
            ctx->shared->input_transcript.append(txt_buf, n);
            xSemaphoreGive(ctx->shared->mutex);
            xEventGroupSetBits(ctx->event_group, S2S_INPUT_TRANSCRIPT_BIT);
        }
        handled = true;
    }

    // ── Turn / interruption signals ──
    if (mjson_find(buf, len, "$.serverContent.turnComplete", &tp, &tl) != MJSON_TOK_INVALID)
    {
        ESP_LOGI(TAG, "Turn complete");
        xEventGroupSetBits(ctx->event_group, S2S_TURN_COMPLETE_BIT);
        handled = true;
    }
    if (mjson_find(buf, len, "$.serverContent.interrupted", &tp, &tl) != MJSON_TOK_INVALID)
    {
        ESP_LOGW(TAG, "Model interrupted");
        xEventGroupSetBits(ctx->event_group, S2S_INTERRUPTED_BIT);
        handled = true;
    }
    if (mjson_find(buf, len, "$.serverContent.generationComplete", &tp, &tl) != MJSON_TOK_INVALID)
    {
        handled = true;
    }

    if (handled)
        return;

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
        bool user_sent = false;

        while (true)
        {
            bits = xEventGroupGetBits(ctx->event_group);
            if (bits & (S2S_STOP_REQUEST_BIT | S2S_ERROR_BIT | S2S_MODEL_SPEAKING_BIT))
                break;
            if (bits & S2S_USER_INTERRUPT_BIT)
            {
                user_sent = true;
                break;
            }

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

        if (user_sent)
        {
            xEventGroupClearBits(ctx->event_group, S2S_USER_INTERRUPT_BIT);
            ESP_LOGI(TAG, "User triggered SEND, ending mic stream");
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

        if (user_sent && !(xEventGroupGetBits(ctx->event_group) & (S2S_MODEL_SPEAKING_BIT | S2S_TURN_COMPLETE_BIT)))
        {
            EventBits_t wait_bits = xEventGroupWaitBits(
                ctx->event_group,
                S2S_MODEL_SPEAKING_BIT | S2S_TURN_COMPLETE_BIT | S2S_STOP_REQUEST_BIT | S2S_ERROR_BIT |
                    S2S_FATAL_ERROR_BIT | S2S_USER_INTERRUPT_BIT,
                pdFALSE,
                pdFALSE,
                pdMS_TO_TICKS(10000));
            if (!(wait_bits & S2S_MODEL_SPEAKING_BIT))
            {
                if (wait_bits & (S2S_STOP_REQUEST_BIT | S2S_ERROR_BIT | S2S_FATAL_ERROR_BIT))
                    break;
                continue;
            }
        }

        // ──────────────────────────────
        // SPEAKING PHASE: drain ring buffer → speaker
        // ──────────────────────────────

        // Pre-buffer: let the ring buffer fill ~300ms before starting the speaker
        // to absorb network burst variability and prevent early underruns.
        {
            constexpr size_t PRE_BUFFER_BYTES = 14400; // 300ms at 24kHz 16-bit mono
            TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(1500);
            while (xTaskGetTickCount() < deadline)
            {
                bits = xEventGroupGetBits(ctx->event_group);
                if (bits & (S2S_STOP_REQUEST_BIT | S2S_ERROR_BIT | S2S_FATAL_ERROR_BIT |
                            S2S_INTERRUPTED_BIT | S2S_USER_INTERRUPT_BIT))
                    break;
                if (bits & S2S_TURN_COMPLETE_BIT)
                    break;
                size_t free_bytes = xRingbufferGetCurFreeSize(ctx->spk_ring_buffer);
                if (AUDIO_BUFFER_SIZE - free_bytes >= PRE_BUFFER_BYTES)
                    break;
                vTaskDelay(pdMS_TO_TICKS(15));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
        ctx->hal->speaker()->begin();
        ctx->hal->speaker()->setVolume(255);
        ctx->hal->speaker()->setChannelVolume(AUDIO_CHANNEL, s_s2s_muted ? 0 : ctx->volume);
        ESP_LOGI(TAG,
                 "Playing response... (muted=%d, heap=%lu, dma_max=%lu)",
                 (int)s_s2s_muted,
                 (unsigned long)esp_get_free_heap_size(),
                 (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));

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
                constexpr size_t REBUFFER_BYTES = 4800; // 100ms cushion on underrun
                TickType_t rebuf_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(400);
                while (xTaskGetTickCount() < rebuf_deadline)
                {
                    bits = xEventGroupGetBits(ctx->event_group);
                    if (bits & (S2S_STOP_REQUEST_BIT | S2S_ERROR_BIT | S2S_FATAL_ERROR_BIT |
                                S2S_INTERRUPTED_BIT | S2S_USER_INTERRUPT_BIT | S2S_TURN_COMPLETE_BIT))
                        break;
                    if (AUDIO_BUFFER_SIZE - xRingbufferGetCurFreeSize(ctx->spk_ring_buffer) >= REBUFFER_BYTES)
                        break;
                    vTaskDelay(pdMS_TO_TICKS(15));
                }
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
        char uri_buf[S2S_WS_URI_BUF_SIZE];
        int uri_len = snprintf(uri_buf, sizeof(uri_buf), "%s?key=%s", S2S_WS_BASE_URL, api_key.c_str());
        if (uri_len <= 0 || (size_t)uri_len >= sizeof(uri_buf))
        {
            ESP_LOGE(TAG, "WebSocket URI too long (%d bytes, max %d)", uri_len, (int)sizeof(uri_buf) - 1);
            goto fail;
        }

        esp_websocket_client_config_t ws_cfg = {};
        ws_cfg.uri = uri_buf;
        ws_cfg.task_prio = 5;
        ws_cfg.buffer_size = WS_BUFFER_SIZE;
        ws_cfg.disable_auto_reconnect = true; // we handle reconnection ourselves
        ws_cfg.reconnect_timeout_ms = 0;
        ws_cfg.network_timeout_ms = 10000;

        s2s_ctx.ws_client = esp_websocket_client_init(&ws_cfg);
        if (!s2s_ctx.ws_client)
        {
            ESP_LOGE(TAG, "Failed to init WS client");
            goto fail;
        }
    }

    esp_websocket_register_events(s2s_ctx.ws_client, WEBSOCKET_EVENT_ANY, s2s_ws_event_handler, &s2s_ctx);

#if HAL_USE_WIFI
    esp_wifi_set_ps(WIFI_PS_NONE);
#endif

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

    ESP_LOGI(TAG,
             "S2S session starting, heap=%lu, dma_free=%lu, dma_max=%lu",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    return true;

fail:
#if HAL_USE_WIFI
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
#endif
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

#if HAL_USE_WIFI
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
#endif

    ESP_LOGI(TAG, "S2S stopped, heap=%lu", (unsigned long)esp_get_free_heap_size());
}

void s2s_set_muted(bool muted)
{
    s_s2s_muted = muted;
    if (s2s_ctx.hal && s2s_ctx.hal->speaker())
    {
        s2s_ctx.hal->speaker()->setChannelVolume(AUDIO_CHANNEL, muted ? 0 : s2s_ctx.volume);
    }
    ESP_LOGI(TAG, "Audio %s", muted ? "MUTED" : "UNMUTED");
}

bool s2s_is_muted(void) { return s_s2s_muted; }
