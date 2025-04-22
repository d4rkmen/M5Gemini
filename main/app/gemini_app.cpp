#include "gemini_app.h"
#include "esp_log.h"
#include "app/utils/ui/dialog.h"
#include "app/utils/ui/settings_screen.h"
#include "app/utils/theme/theme_define.h"
#include "http_client.h"
#include "ws_client.h"
#include <format>
#include <vector>
#include <utility>
#include "assets/g_fonts.hpp"
#include "assets/gemini_icon.h"
#include "assets/qr_gemini.h"
#include "assets/qr_elevenlabs.h"
#include "assets/qr_deepgram.h"
#include "assets/anm_disconnected.h"
#include "assets/anm_wifi.h"
#include "assets/anm_internet.h"
#include "assets/anm_error.h"
#include "assets/anm_playing3.h"
#include "assets/anm_mic.h"

#include "hal_cardputer.h"
#include "wifi/wifi.h"
#include "event_bits.h"

static const char* TAG = "GEMINI_APP";
static const char* GEMINI_NS = "gemini";
static const char* HINT_MAIN = "[ENTER] NEW CHAT [ESC] SETTINGS";
static const char* RESPONSE_HINT = "[UP][DOWN] [LEFT][RIGHT] [Fn][ENTER] [ESC]";
static const char* API_KEY_HINT = "[ENTER] [ESC]";

// TTS configuration
static const char* ELEVENLABS_NS = "elevenlabs";
// STT configuration
static const char* DEEPGRAM_NS = "deepgram";

std::string settings_file_name = "/sdcard/settings.txt";

static bool is_repeat = false;
static bool is_start = false;
#define KEY_HOLD_MS 500
#define KEY_REPEAT_MS 50
static bool is_rendered = false;

#define FONT_14 &font_geist14

// Animation constants
#define ANIMATION_STACK_SIZE 4096
#define ANIMATION_PRIORITY 1
#define ANIMATION_CORE 1
#define ANIMATION_FRAME_DELAY_MS 20

#define HINT_HEIGHT 12

extern const uint8_t snap_wav_start[] asm("_binary_snap_wav_start");
extern const uint8_t snap_wav_end[] asm("_binary_snap_wav_end");

#if 0
static TimerHandle_t _logHeapTimer = nullptr;

void logFreeHeapCallback(TimerHandle_t xTimer)
{
    static bool s_tick_tock = false;
    ESP_LOGI("TIMER",
             "%s uptime: %08lu, RAM: %lu, %lu free",
             s_tick_tock ? "Tick" : "Tock",
             (unsigned long)millis() / 1000,
             (unsigned long)heap_caps_get_total_size(MALLOC_CAP_8BIT),
             (unsigned long)esp_get_free_heap_size());
    s_tick_tock = !s_tick_tock;
}
#endif
// Constructor
GeminiApp::GeminiApp(HAL::Hal* hal) : _hal(hal), _currentScreen(SCREEN_START), _scrollPosition(0)
{
    //
    _hal->canvas()->setFont(&fonts::efontEN_16);
    _hintTextContext = new UTILS::HL_TEXT::HLTextContext_t();
    UTILS::HL_TEXT::hl_text_init(_hintTextContext, _hal->canvas(), 20, 1500);

    _descScrollContext = new UTILS::SCROLL_TEXT::ScrollTextContext_t();
    UTILS::SCROLL_TEXT::scroll_text_init(_descScrollContext,
                                         _hal->canvas(),
                                         _hal->canvas()->width(),
                                         16,
                                         20,    // scroll speed
                                         1000); // pause time

    // Create event group for synchronization and control
    _control_event_group = xEventGroupCreate();
    // clear all bits
    xEventGroupClearBits(_control_event_group, 0xFFFFFF);
    // Initialize the animation sprite
    _sprite = new LGFX_Sprite(_hal->canvas());
    _sprite->createSprite(50, 50);

    // Initialize TTS resources
    initTTS();
    // create one second timer to log free heap size
#if 0
    _logHeapTimer = xTimerCreate("LogFreeHeap", pdMS_TO_TICKS(1000), pdTRUE, NULL, logFreeHeapCallback);
    xTimerStart(_logHeapTimer, 0);
#endif
}

// Destructor
GeminiApp::~GeminiApp()
{
#if 0
    // Stop and delete the heap logging timer
    if (_logHeapTimer != nullptr)
    {
        xTimerStop(_logHeapTimer, 0);
        xTimerDelete(_logHeapTimer, 0);
        _logHeapTimer = nullptr;
    }
#endif

    _sprite->deleteSprite();
    delete _sprite;

    // Clean up TTS resources
    stopTTS();
    if (_control_event_group != nullptr)
    {
        vEventGroupDelete(_control_event_group);
        _control_event_group = nullptr;
    }

    // Clean up settings screen resources
    if (_hintTextContext)
    {
        UTILS::HL_TEXT::hl_text_free(_hintTextContext);
        delete _hintTextContext;
        _hintTextContext = nullptr;
    }

    if (_descScrollContext)
    {
        UTILS::SCROLL_TEXT::scroll_text_free(_descScrollContext);
        delete _descScrollContext;
        _descScrollContext = nullptr;
    }
}

// Initialize TTS resources
void GeminiApp::initTTS()
{
    ESP_LOGD(TAG, "Initializing TTS resources");

    // Initialize with task not running
    xEventGroupClearBits(_control_event_group,
                         TTS_STOP_REQUEST_BIT | TTS_PLAY_TASK_STARTED_BIT | TTS_PLAY_TASK_STOPPED_BIT |
                             TTS_STREAM_TASK_STARTED_BIT | TTS_STREAM_TASK_STOPPED_BIT | TTS_PLAYBACK_START_REQUEST_BIT |
                             TTS_PLAYBACK_STOP_REQUEST_BIT);

    _tts_stream_task_handle = nullptr;
}

// Start TTS processing in a separate task
void GeminiApp::startTTS()
{
    ESP_LOGD(TAG, "Starting TTS for text: %ld characters", (uint32_t)_apiResponse.length());

    // Check if TTS is enabled and configured
    if (!_hal->settings()->getBool(ELEVENLABS_NS, "enabled"))
    {
        ESP_LOGE(TAG, "TTS is disabled, skipping");
        return;
    }
    if (_hal->settings()->getString(ELEVENLABS_NS, "api_key").empty())
    {
        UTILS::UI::show_error_dialog(_hal, "Failed", "ElevenLabs API key not set. Please go to ElevenLabs settings", "OK");
        return;
    }
    if (_hal->settings()->getString(ELEVENLABS_NS, "voice").empty())
    {
        UTILS::UI::show_error_dialog(_hal, "Failed", "ElevenLabs voice not set. Please go to ElevenLabs settings", "OK");
        return;
    }
    if (_hal->settings()->getString(ELEVENLABS_NS, "model").empty())
    {
        UTILS::UI::show_error_dialog(_hal, "Failed", "ElevenLabs model not set. Please go to ElevenLabs settings", "OK");
        return;
    }
    if (_apiResponse.empty())
    {
        UTILS::UI::show_error_dialog(_hal, "Failed", "Text to speak is empty", "OK");
        return;
    }

    // Stop any existing TTS task gracefully
    stopTTS();

    if ((xEventGroupGetBits(_control_event_group) & (TTS_PLAY_TASK_STARTED_BIT | TTS_STREAM_TASK_STARTED_BIT)) ==
        (TTS_PLAY_TASK_STARTED_BIT | TTS_STREAM_TASK_STARTED_BIT))
    {
        // Wait for the previous task to finish (with a timeout)
        ESP_LOGD(TAG, "Waiting for previous TTS task to exit...");
        EventBits_t bits = xEventGroupWaitBits(_control_event_group,
                                               TTS_PLAY_TASK_STOPPED_BIT | TTS_STREAM_TASK_STOPPED_BIT,
                                               pdFALSE,
                                               pdTRUE,
                                               pdMS_TO_TICKS(5000) // 5-second timeout
        );

        if ((bits & TTS_PLAY_TASK_STOPPED_BIT) != TTS_PLAY_TASK_STOPPED_BIT)
        {
            ESP_LOGW(TAG, "Timeout waiting for playback task to exit. Task might still be running.");
            // Proceed with caution, old task might not have cleaned up fully
        }
        if ((bits & TTS_STREAM_TASK_STOPPED_BIT) != TTS_STREAM_TASK_STOPPED_BIT)
        {
            ESP_LOGW(TAG, "Timeout waiting for HTTP task to exit. Task might still be running.");
        }
        if ((bits & (TTS_PLAY_TASK_STOPPED_BIT | TTS_STREAM_TASK_STOPPED_BIT)) ==
            (TTS_PLAY_TASK_STOPPED_BIT | TTS_STREAM_TASK_STOPPED_BIT))
        {
            ESP_LOGD(TAG, "Previous TTS task exited");
        }
    }

    // Clear any lingering stop request from the previous stop call
    xEventGroupClearBits(_control_event_group,
                         TTS_STOP_REQUEST_BIT | TTS_PLAY_TASK_STARTED_BIT | TTS_PLAY_TASK_STOPPED_BIT |
                             TTS_STREAM_TASK_STARTED_BIT | TTS_STREAM_TASK_STOPPED_BIT | TTS_PLAYBACK_START_REQUEST_BIT |
                             TTS_PLAYBACK_STOP_REQUEST_BIT);
    // set system volume
    _hal->speaker()->setVolume(255);
    _hal->speaker()->setChannelVolume(AUDIO_CHANNEL, _hal->settings()->getNumber(ELEVENLABS_NS, "volume"));
    // Create the new TTS task
    BaseType_t result =
        xTaskCreate(tts_stream_task, "tts_stream_task", TTS_TASK_STACK_SIZE, this, TTS_TASK_PRIORITY, &_tts_stream_task_handle);

    if (result != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create TTS task");
        _tts_stream_task_handle = nullptr;
    }
    else
    {
        ESP_LOGD(TAG, "HTTP TTS task created successfully");
        // Set the running bit
    }
}

// Stop running TTS task gracefully
void GeminiApp::stopTTS()
{
    // Signal the task to stop
    ESP_LOGD(TAG, "Setting stop request bit for TTS task");
    xEventGroupSetBits(_control_event_group, TTS_STOP_REQUEST_BIT);
}

// TTS task code - handles calling the ElevenLabs API
void GeminiApp::tts_stream_task(void* parameter)
{
    GeminiApp* app = static_cast<GeminiApp*>(parameter);
    HAL::Hal* hal = app->_hal;
    EventGroupHandle_t control_event_group = app->_control_event_group;
    TaskHandle_t task_handle = app->_tts_stream_task_handle;
    std::string text_to_process = app->_apiResponse;
    std::string api_key = hal->settings()->getString(ELEVENLABS_NS, "api_key");
    std::string voice_id = hal->settings()->getString(ELEVENLABS_NS, "voice");
    std::string model_id = hal->settings()->getString(ELEVENLABS_NS, "model");
    xEventGroupSetBits(control_event_group, TTS_STREAM_TASK_STARTED_BIT);

    ESP_LOGD(TAG, "HTTP TTS task started (Handle: %p)", task_handle);
    // Pass the control event group to the API call
    std::string result = call_11labs_api(hal, api_key, voice_id, text_to_process, model_id, control_event_group);

    // Clear the running bit and any pending stop request in the control event group
    if (control_event_group != nullptr)
    {
        ESP_LOGD(TAG, "Clearing HTTP TTS control bits (Handle: %p)", task_handle);
        xEventGroupSetBits(control_event_group, TTS_STREAM_TASK_STOPPED_BIT);
    }

    ESP_LOGD(TAG, "HTTP TTS task completed and exiting (Handle: %p)", task_handle);
    vTaskDelete(NULL); // Task deletes itself
}

// Initialize the application
void GeminiApp::init()
{
    ESP_LOGI(TAG, "Initializing");
    setState(APP_STATE_DISCONNECTED);
    // Get settings metadata
    _groups = _hal->settings()->getMetadata();
    // Initialize WiFi module
    if (_hal->wifi()->init())
    {
        _hal->wifi()->set_status_callback(
            [this](HAL::wifi_status_t status)
            {
                ESP_LOGI(TAG, "WiFi status changed to %d", status);
                _wifiStatus = status;
                switch (status)
                {
                case HAL::WIFI_STATUS_IDLE:
                case HAL::WIFI_STATUS_DISCONNECTED:
                    setState(APP_STATE_DISCONNECTED);
                    break;
                case HAL::WIFI_STATUS_CONNECTING:
                    setState(APP_STATE_CONNECTING_WIFI);
                    break;
                case HAL::WIFI_STATUS_CONNECTED_WEAK:
                case HAL::WIFI_STATUS_CONNECTED_GOOD:
                case HAL::WIFI_STATUS_CONNECTED_STRONG:
                    setState(APP_STATE_IDLE);
                    break;
                }
            });
        // Connect to WiFi if enabled
        if (_hal->settings()->getBool("wifi", "enabled"))
        {
            _hal->wifi()->connect();
        }
    }
    // set brightness
    int brightness = _hal->settings()->getNumber("system", "brightness");
    if (brightness > 0)
        _hal->display()->setBrightness(brightness);
    // set volume
    int volume = _hal->settings()->getNumber("system", "volume");
    if (volume > 0)
    {
        _hal->speaker()->setVolume(255);
        _hal->speaker()->setChannelVolume(SYSTEM_CHANNEL, volume);
    }
    // play boot sound
    if (_hal->settings()->getBool("system", "boot_sound"))
        _hal->speaker()->playWav(snap_wav_start, snap_wav_end - snap_wav_start, 1, SYSTEM_CHANNEL);
    // Set initial state
    _currentScreen = SCREEN_START;
    is_rendered = false;
}

// Main update loop
void GeminiApp::update()
{
    bool need_update = false;
    // read bits
    EventBits_t bits = xEventGroupGetBits(_control_event_group);
    switch (_currentScreen)
    {
    case SCREEN_START:
        need_update |= drawMainScreen();
        need_update |= drawAnimation(need_update);
        handleMainScreenInput();
        // Draw hint at bottom
        need_update |= UTILS::HL_TEXT::hl_text_render(_hintTextContext,
                                                      HINT_MAIN,
                                                      0,
                                                      _hal->canvas()->height() - HINT_HEIGHT,
                                                      TFT_DARKGREY,
                                                      TFT_WHITE,
                                                      THEME_COLOR_BG);

        // Update the display
        if (need_update)
            _hal->canvas_update();

        break;
    case SCREEN_QR_GEMINI:
        need_update |= drawGeminiQRScreen();
        handleApiKeyScreenInput();
        need_update |= UTILS::HL_TEXT::hl_text_render(_hintTextContext,
                                                      API_KEY_HINT,
                                                      65,
                                                      _hal->canvas()->height() - HINT_HEIGHT,
                                                      TFT_DARKGREY,
                                                      TFT_WHITE,
                                                      THEME_COLOR_BG);
        if (need_update)
            _hal->canvas_update();
        break;
    case SCREEN_QR_ELEVENLABS:
        need_update |= drawElevenLabsQRScreen();
        handleApiKeyScreenInput();
        need_update |= UTILS::HL_TEXT::hl_text_render(_hintTextContext,
                                                      API_KEY_HINT,
                                                      65,
                                                      _hal->canvas()->height() - HINT_HEIGHT,
                                                      TFT_DARKGREY,
                                                      TFT_WHITE,
                                                      THEME_COLOR_BG);
        if (need_update)
            _hal->canvas_update();
        break;
    case SCREEN_QR_DEEPGRAM:
        need_update |= drawDeepgramQRScreen();
        handleApiKeyScreenInput();
        need_update |= UTILS::HL_TEXT::hl_text_render(_hintTextContext,
                                                      API_KEY_HINT,
                                                      65,
                                                      _hal->canvas()->height() - HINT_HEIGHT,
                                                      TFT_DARKGREY,
                                                      TFT_WHITE,
                                                      THEME_COLOR_BG);
        if (need_update)
            _hal->canvas_update();
        break;

    case SCREEN_SETTINGS:
        handleSettingsMenu();
        break;

    case SCREEN_CHAT:
        switch (_appState)
        {
        case APP_STATE_DISCONNECTED:
            break;
        case APP_STATE_STREAM_ERROR:
            if (!(bits & STT_STREAM_ERROR_BIT))
            {
                setState(APP_STATE_CONNECTING_STT);
                is_rendered = false;
            }
            break;
        case APP_STATE_CONNECTING_WIFI:
            break;
        case APP_STATE_CONNECTING_STT:
            if (bits & STT_STREAM_CONNECTED_BIT)
            {
                setState(APP_STATE_LISTENING_MIC);
                _partialPrompt = "";
                is_rendered = false;
            }
            else if (bits & STT_STREAM_ERROR_BIT)
            {
                setState(APP_STATE_STREAM_ERROR);
                _partialPrompt = "";
                is_rendered = false;
            }
            break;
        case APP_STATE_LISTENING_MIC:
            if (bits & STT_TRANSCRIPT_BIT)
            {
                setState(APP_STATE_CONNECTING_GEMINI);
                xEventGroupClearBits(_control_event_group, STT_TRANSCRIPT_BIT);
                _userPrompt = get_transcript();
                _lastHistoryIndex = _chat.size();
                for (const auto& line : splitTextIntoLines(_userPrompt))
                    _chat.push_back({HISTORY_ITEM_TYPE_USER, line});
                _partialPrompt = "";
                updateScrollPosition();
                // debug only
                xEventGroupSetBits(_control_event_group, STT_STOP_REQUEST_BIT);
                vTaskDelay(pdMS_TO_TICKS(100));
                callGeminiAPI();
                is_rendered = false;
            }
            else if (bits & STT_PARTIAL_TRANSCRIPT_BIT)
            {
                _partialPrompt = get_transcript();
                updateScrollPosition();
                xEventGroupClearBits(_control_event_group, STT_PARTIAL_TRANSCRIPT_BIT);
                is_rendered = false;
            }
            if (!(bits & STT_STREAM_CONNECTED_BIT))
            {
                setState(APP_STATE_CONNECTING_STT);
                is_rendered = false;
            }
            break;
        case APP_STATE_CONNECTING_GEMINI:
            if (bits & GEMINI_TASK_STOPPED_BIT)
            {
                setState(APP_STATE_IDLE);
                xEventGroupClearBits(_control_event_group, GEMINI_TASK_STOPPED_BIT);
                // check error
                if (_apiResponse.find("Error:") == 0)
                {
                    // Show error dialog
                    UTILS::UI::show_error_dialog(_hal, "API error", _apiResponse, "OK");
                    // check history and jump to main if no history
                    if (_history.empty())
                        _currentScreen = SCREEN_START;
                    else
                        _currentScreen = SCREEN_CHAT;
                    is_rendered = false;
                }
                else
                {
                    setState(APP_STATE_CONNECTING_TTS);
                    // Start TTS task
                    startTTS();
                    // Add successful chat to history
                    for (const auto& line : splitTextIntoLines(_apiResponse))
                        _chat.push_back({HISTORY_ITEM_TYPE_MODEL, line});
                    _history.push_back({_userPrompt, _apiResponse});
                    _partialPrompt = "";
                    // Calculate scroll position to show the start of the latest request
                    updateScrollPosition();
                    _currentScreen = SCREEN_CHAT;
                    is_rendered = false;
                }
            }
            break;
        case APP_STATE_CONNECTING_TTS:
            if (bits & TTS_PLAYBACK_START_REQUEST_BIT)
            {
                setState(APP_STATE_TTS_PLAYING);
                is_rendered = false;
            }
            break;
        case APP_STATE_TTS_PLAYING:
            if ((bits & (TTS_PLAY_TASK_STOPPED_BIT | STT_STREAM_TASK_STOPPED_BIT)) ==
                (TTS_PLAY_TASK_STOPPED_BIT | STT_STREAM_TASK_STOPPED_BIT))
            {
                setState(APP_STATE_IDLE);
                _partialPrompt = "";
                deepgram_streaming_stop();
                vTaskDelay(pdMS_TO_TICKS(100));
                // can start listening again
                is_rendered = false;
            }
            break;
        case APP_STATE_IDLE:
            if (_wifiStatus > HAL::WIFI_STATUS_CONNECTING)
            {
                if (_hal->settings()->getBool("deepgram", "enabled"))
                {
                    setState(APP_STATE_CONNECTING_STT);
                    if (deepgram_streaming_start(_hal, _control_event_group))
                    {
                        _currentScreen = SCREEN_CHAT;
                    }
                    else
                    {
                        UTILS::UI::show_error_dialog(_hal, "Failed", "Failed to start Deepgram streaming", "OK");
                        _currentScreen = SCREEN_START;
                        setState(APP_STATE_IDLE);
                    }
                }
                is_rendered = false;
                break;
            }
            break;
        }
        need_update |= drawResponseScreen();
        need_update |= drawAnimation(need_update);
        handleResponseScreenInput();
        // Draw hint at bottom
        need_update |= UTILS::HL_TEXT::hl_text_render(_hintTextContext,
                                                      RESPONSE_HINT,
                                                      0,
                                                      _hal->canvas()->height() - HINT_HEIGHT,
                                                      TFT_DARKGREY,
                                                      TFT_WHITE,
                                                      THEME_COLOR_BG);
        if (need_update)
            _hal->canvas_update();

        break;
    }
}

// Draw the main screen
bool GeminiApp::drawMainScreen()
{
    if (is_rendered)
        return false;

    // Clear history when returning to main screen
    _history.clear();
    _chat.clear();
    _partialPrompt = "";

    // Clear the screen
    _hal->canvas()->fillScreen(THEME_COLOR_BG);

    // Draw header
    _hal->canvas()->fillRect(0, 0, _hal->canvas()->width(), 20, THEME_COLOR_BG);
    _hal->canvas()->setTextColor(THEME_COLOR_TITLE);
    _hal->canvas()->setFont(FONT_16);
    int offestY = TEXT_PADDING;
    int lineHeight = _hal->canvas()->fontHeight(FONT_16);
    _hal->canvas()->drawCenterString("Gemini AI", _hal->canvas()->width() / 2, offestY);
    offestY += lineHeight * 2;
    _hal->canvas()->pushImage((_hal->canvas()->width() - 64) / 2, offestY, 64, 64, image_data_gemini_icon, TFT_BLACK);
    // draw STT enabled
    if (_hal->settings()->getBool("deepgram", "enabled") && !_hal->settings()->getString("deepgram", "api_key").empty())
    {
        _hal->canvas()->pushImage((_hal->canvas()->width() - 64) / 2 - 50 - 8, offestY + 7, 50, 50, image_data_mic, TFT_BLACK);
    }
    // draw TTS enabled
    if (_hal->settings()->getBool("elevenlabs", "enabled") && !_hal->settings()->getString("elevenlabs", "api_key").empty())
    {
        _hal->canvas()
            ->pushImage((_hal->canvas()->width() - 64) / 2 + 64 + 8, offestY + 7, 50, 50, image_data_playing3, TFT_BLACK);
    }
    offestY += 64 + 2;
    // Draw main content area
    _hal->canvas()->setTextColor(TFT_WHITE);
    _hal->canvas()->drawCenterString("v" BUILD_NUMBER, _hal->canvas()->width() / 2, offestY);

    is_rendered = true;
    return true;
}

// Handle input on the main screen
void GeminiApp::handleMainScreenInput()
{
    _hal->keyboard()->updateKeyList();
    _hal->keyboard()->updateKeysState();

    if (_hal->keyboard()->isKeyPressing(KEY_NUM_ENTER))
    {
        _hal->keyboard()->waitForRelease(KEY_NUM_ENTER);
        _hal->playNextSound();
        // hold Fn to keep last prompt
        is_rendered = false;
        // Check if we have API key and WiFi credentials
        if (_hal->settings()->getString(GEMINI_NS, "api_key").empty())
        {
            UTILS::UI::show_error_dialog(_hal, "Failed", "API key not set. Please go to settings", "OK");
            return;
        }

        // Try to connect to WiFi
        if (!_hal->settings()->getBool("wifi", "enabled"))
        {
            UTILS::UI::show_error_dialog(_hal, "Failed", "WiFi is disabled, check settings", "OK");
            return;
        }
        _partialPrompt = "";
        // hold Fn to enter first prompt
        if (_hal->settings()->getBool("deepgram", "enabled") && !_hal->keyboard()->keysState().fn)
        {
            _currentScreen = SCREEN_CHAT;
            is_rendered = false;
        }
        else
        {
            if (!_hal->keyboard()->keysState().fn)
                _userPrompt = "";
            // Show the input dialog to get user's prompt
            bool result = UTILS::UI::show_edit_string_dialog(_hal, "Enter your prompt", _userPrompt, false, 1000);

            if (result && !_userPrompt.empty())
            {
                setState(APP_STATE_CONNECTING_GEMINI);
                xEventGroupClearBits(_control_event_group, STT_TRANSCRIPT_BIT);
                // Call the API asynchronously to start the task
                // _history.push_back({_userPrompt, "..."});
                _lastHistoryIndex = _chat.size();
                for (const auto& line : splitTextIntoLines(_userPrompt))
                    _chat.push_back({HISTORY_ITEM_TYPE_USER, line});
                _partialPrompt = "";
                updateScrollPosition();
                callGeminiAPI();
                // Connect to WiFi and call API
                _currentScreen = SCREEN_CHAT;
                is_rendered = false;
            }
        }
    }
    else if (_hal->keyboard()->isKeyPressing(KEY_NUM_ESC))
    {
        _hal->keyboard()->waitForRelease(KEY_NUM_ESC);
        _hal->playNextSound();
        _currentScreen = SCREEN_SETTINGS;
        setState(APP_STATE_IDLE);
        // UTILS::UI::SETTINGS_SCREEN::reset();
        is_rendered = false;
    }
}

// Handle the settings menu (render and input)
void GeminiApp::handleSettingsMenu()
{
    // Update the settings screen
    bool need_update = UTILS::UI::SETTINGS_SCREEN::update(
        _hal,
        _groups,
        _hintTextContext,
        _descScrollContext,
        [this](int group_index)
        {
            ESP_LOGD(TAG, "handleSettingsMenu on_enter() group_index=%d", group_index);
            switch (group_index)
            {
            case -1:
                _currentScreen = SCREEN_START;
                is_rendered = false;
                break;
            case 2:
                if (_hal->settings()->getString(GEMINI_NS, "api_key").empty())
                {
                    _currentScreen = SCREEN_QR_GEMINI;
                    is_rendered = false;
                }
                break;
            case 3:
                if (_hal->settings()->getString(ELEVENLABS_NS, "api_key").empty())
                {
                    _currentScreen = SCREEN_QR_ELEVENLABS;
                    is_rendered = false;
                }
                break;
            case 4:
                if (_hal->settings()->getString(DEEPGRAM_NS, "api_key").empty())
                {
                    _currentScreen = SCREEN_QR_DEEPGRAM;
                    is_rendered = false;
                }
                break;
            case 5:
                // export settings
                _hal->sdcard()->mount(false);
                if (_hal->sdcard()->is_mounted())
                {
                    _hal->settings()->exportToFile(settings_file_name);
                    _hal->sdcard()->eject();
                    UTILS::UI::show_message_dialog(_hal, "Success", "Settings exported to: " + settings_file_name, 5000);
                }
                else
                {
                    UTILS::UI::show_message_dialog(_hal, "Error", "Failed to mount SD card", 5000);
                }
                break;
            case 6:
                // import settings
                _hal->sdcard()->mount(false);
                if (_hal->sdcard()->is_mounted())
                {
                    _hal->settings()->importFromFile(settings_file_name);
                    _hal->sdcard()->eject();
                    // set brightness
                    _hal->display()->setBrightness(_hal->settings()->getNumber("system", "brightness"));
                    // set volume
                    // _hal->speaker()->setChannelVolume(SYSTEM_CHANNEL, _hal->settings()->getNumber("system", "volume"));
                    // _hal->speaker()->setChannelVolume(SYSTEM_CHANNEL, _hal->settings()->getNumber("system", "volume"));
                    // restart wifi
                    UTILS::UI::show_progress(_hal, "WiFi", -1, "Stopping...");
                    _hal->wifi()->init();
                    // Connect to WiFi if enabled
                    if (_hal->settings()->getBool("wifi", "enabled"))
                    {
                        UTILS::UI::show_progress(_hal, "WiFi", -1, "Starting...");
                        _hal->wifi()->connect();
                    }

                    UTILS::UI::show_message_dialog(_hal, "Success", "Settings imported from: " + settings_file_name, 5000);
                }
                else
                {
                    UTILS::UI::show_error_dialog(_hal, "Error", "Failed to mount SD card", "OK");
                }
                break;
            }
        });

    // Update the display if needed
    if (need_update)
    {
        _hal->canvas_update();
    }
}

// Split text into lines that fit the screen width
std::vector<std::string> GeminiApp::splitTextIntoLines(const std::string& text)
{
    std::vector<std::string> lines;
    std::string currentLine;

    for (char c : text)
    {
        if (c == '\n')
        {
            lines.push_back(currentLine);
            currentLine.clear();
            continue;
        }

        currentLine += c;

        // Check if line is too long
        if (_hal->canvas()->textWidth(currentLine.c_str(), FONT_14) > _hal->canvas()->width() - (TEXT_PADDING * 2))
        {
            // Find last space to break at
            size_t lastSpace = currentLine.find_last_of(' ');
            if (lastSpace != std::string::npos)
            {
                std::string lineToAdd = currentLine.substr(0, lastSpace);
                lines.push_back(lineToAdd);
                currentLine = currentLine.substr(lastSpace + 1);
            }
            else
            {
                // No space found, just break at current position
                lines.push_back(currentLine);
                currentLine.clear();
            }
        }
    }

    // Add any remaining text
    if (!currentLine.empty())
    {
        lines.push_back(currentLine);
    }

    return lines;
}

#if 1
float linear(float t) { return t; }
float easeInQuad(float t) { return powf(t, 2.0f); }
float easeOutQuad(float t) { return t * (2.0f - t); }
float easeInOutQuad(float t) { return t < .5f ? 2.0f * powf(t, 2) : -1.0f + (4.0f - 2.0f * t) * t; }
float easeInCubic(float t) { return powf(t, 3.0f); }
float easeOutCubic(float t) { return 1.0f - powf(1.0f - t, 3.0f); }
float easeInOutCubic(float t)
{
    return t < .5f ? 4.0f * powf(t, 3) : (t - 1.0f) * (2.0f * t - 2.0f) * (2.0f * t - 2.0f) + 1.0f;
}
float easeInQuart(float t) { return powf(t, 4.0f); }
float easeOutQuart(float t) { return 1.0f - powf(1.0f - t, 4.0f); }
float easeInOutQuart(float t) { return t < 0.5 ? 8.0f * t * t * t * t : 1.0f - powf(-2.0f * t + 2.0f, 4.0f) / 2.0f; }
float easeInQuint(float t) { return powf(t, 5.0f); }
float easeOutQuint(float t) { return 1.0f - powf(1.0f - t, 5.0f); }
float easeInOutQuint(float t)
{
    return t < 0.5 ? 16.0f * t * t * t * t * t
                   : 1.0f - powf(-2.0f * t + 2.0f, 5.0f) / 2.0f; /*t<.5f ? 16.0f*powf(t,5) : 1.0f+16.0f*(--t)*powf(t,4);*/
}
float easeInSine(float t) { return 1.0f - cosf((t * M_PI) / 2.0f); }
float easeOutSine(float t) { return sinf((t * M_PI) / 2.0f); }
float easeInOutSine(float t) { return -(cosf(M_PI * t) - 1.0f) / 2.0f; }
float easeInExpo(float t) { return t == 0 ? 0 : powf(2, 10.0f * t - 10.0f); }
float easeOutExpo(float t) { return t == 1.0f ? 1.0f : 1.0f - powf(2, -10.0f * t); }
float easeInOutExpo(float t)
{
    return t == 0      ? 0
           : t == 1.0f ? 1.0f
           : t < 0.5f  ? powf(2, 20.0f * t - 10.0f) / 2.0f
                       : (2.0f - powf(2, -20.0f * t + 10.0f)) / 2.0f;
}
float easeInCirc(float t) { return 1.0f - sqrtf(1.0f - powf(t, 2.0f)); }
float easeOutCirc(float t) { return sqrtf(1.0f - powf(t - 1, 2.0f)); }
float easeInOutCirc(float t)
{
    return t < 0.5f ? (1.0f - sqrtf(1.0f - powf(2.0f * t, 2.0f))) / 2.0f
                    : (sqrtf(1 - powf(-2.0f * t + 2.0f, 2.0f)) + 1.0f) / 2.0f;
}
#endif

void GeminiApp::setState(AppState state)
{
    if (_appState == state)
        return;
    ESP_LOGI(TAG, "Setting app state to %d", state);
    _appState = state;
    _anim_context.timer_start = millis();
}

// Draw the animation
bool GeminiApp::drawAnimation(bool need_update)
{
    switch (_appState)
    {
    case APP_STATE_IDLE:
        _sprite->fillScreen(THEME_COLOR_BG);
        break;
    case APP_STATE_DISCONNECTED:
        _sprite->pushImage(0, 0, 50, 50, image_data_disconnected);
        break;
    case APP_STATE_STREAM_ERROR:
        _sprite->pushImage(0, 0, 50, 50, image_data_conn_error);
        break;
    case APP_STATE_CONNECTING_WIFI:
        _sprite->pushImage(0, 0, 50, 50, image_data_connecting_wifi);
        break;
    case APP_STATE_CONNECTING_TTS:
    case APP_STATE_CONNECTING_STT:
    case APP_STATE_CONNECTING_GEMINI:
        _sprite->pushImage(0, 0, 50, 50, image_data_connecting_internet);
        break;
    case APP_STATE_TTS_PLAYING:
        _sprite->pushImage(0, 0, 50, 50, image_data_playing3);
        break;
    case APP_STATE_LISTENING_MIC:
        _sprite->pushImage(0, 0, 50, 50, image_data_mic);
        break;
    }
    uint32_t timer_now = millis();
    // int x_offset = (_hal->canvas()->width() - _sprite->width()) / 2;
    // int y_offset = (_hal->canvas()->height() - _sprite->height()) / 2;
    int x_offset = _hal->canvas()->width() - _sprite->width() - 1;
    int y_offset = 0;

    uint32_t full_pos = (timer_now - _anim_context.timer_start) % (_anim_context.duration * 2);
    uint32_t timer_pos = (full_pos % _anim_context.duration) * _anim_context.steps / _anim_context.duration;
    // make reverse move when > duration
    float ifloat;
    if (full_pos >= _anim_context.duration)
    {
        ifloat = easeOutExpo(float(_anim_context.steps - timer_pos) / float(_anim_context.steps));
    }
    else
    {
        ifloat = easeOutExpo(float(timer_pos) / float(_anim_context.steps));
    }
    uint8_t alpha = ifloat * 255;
    if (alpha == _anim_context.last_alpha && need_update == false)
        return false;
    _anim_context.last_alpha = alpha;
    // mixing sprite with background according to alpha
    for (int py = 0; py < _sprite->height(); py++)
    {
        for (int px = 0; px < _sprite->width(); px++)
        {
            uint16_t fg = _sprite->readPixel(px, py);                              // foreground (sprite pixel)
            uint16_t bg = _hal->canvas()->readPixel(px + x_offset, py + y_offset); // background pixel

            uint32_t fg24 = _sprite->color16to24(fg);
            uint8_t fg_r = (fg24 >> 16) & 0xFF;
            uint8_t fg_g = (fg24 >> 8) & 0xFF;
            uint8_t fg_b = fg24 & 0xFF;

            uint32_t bg24 = _hal->canvas()->color16to24(bg);
            uint8_t bg_r = (bg24 >> 16) & 0xFF;
            uint8_t bg_g = (bg24 >> 8) & 0xFF;
            uint8_t bg_b = bg24 & 0xFF;

            uint8_t r = (fg_r * alpha + bg_r * (255 - alpha)) / 255;
            uint8_t g = (fg_g * alpha + bg_g * (255 - alpha)) / 255;
            uint8_t b = (fg_b * alpha + bg_b * (255 - alpha)) / 255;
            // transparent color
            if (fg == THEME_COLOR_BG)
                _sprite->drawPixel(px, py, _sprite->color888(bg_r, bg_g, bg_b));
            else
                _sprite->drawPixel(px, py, _sprite->color888(r, g, b));
        }
    }
    _sprite->pushSprite(x_offset, y_offset);
    is_rendered = false;
    return true;
}

// Draw the response screen
bool GeminiApp::drawResponseScreen()
{
    if (is_rendered)
        return false;
    int scrollbar_width = 5;
    // Clear the screen
    _hal->canvas()->fillScreen(THEME_COLOR_BG);
    _hal->canvas()->setFont(FONT_14);

    // Draw response text with scrolling
    int textStartY = TEXT_PADDING;
    int textHeight = _hal->canvas()->height() - textStartY - HINT_HEIGHT; // Leave space for bottom hint

    if (_chat.empty() && _partialPrompt.empty())
    {
        _hal->canvas()->setTextColor(TFT_DARKGRAY, THEME_COLOR_BG);
        _hal->canvas()->drawCenterString("no chat history", _hal->canvas()->width() / 2, _hal->canvas()->height() / 2, FONT_16);
    }
    else
    {
        // Calculate line height and maximum visible lines
        int lineHeight = _hal->canvas()->fontHeight(FONT_14);
        int maxVisibleLines = textHeight / lineHeight;
        // store for control handler
        std::vector<std::string> partial_prompt = splitTextIntoLines(_partialPrompt);
        _totalLines = _chat.size() + partial_prompt.size();

        // Adjust scroll position if needed
        if (_scrollPosition > _totalLines - maxVisibleLines)
        {
            _scrollPosition = std::max(0, _totalLines - maxVisibleLines);
        }

        // Display visible lines
        for (int i = 0; i < maxVisibleLines && i + _scrollPosition < _totalLines; i++)
        {
            int y = textStartY + (i * lineHeight);
            int current_line_index = i + _scrollPosition;

            // Determine if the current line is part of a request or response
            bool is_request =
                (current_line_index < _chat.size() && _chat[current_line_index].first == HISTORY_ITEM_TYPE_USER) ||
                (current_line_index >= _chat.size());
            bool is_last = current_line_index >= _lastHistoryIndex;
            // Use different color/style for request vs response
            if (is_request)
            {
                _hal->canvas()->fillRect(0,
                                         y,
                                         TEXT_PADDING - 1,
                                         lineHeight,
                                         is_last ? TFT_WHITE : TFT_DARKGRAY);                          // Bar color for request
                _hal->canvas()->setTextColor(is_last ? THEME_COLOR_TITLE : THEME_COLOR_TITLE_HISTORY); // Text color for request
            }
            else
            {
                _hal->canvas()->fillRect(0,
                                         y,
                                         TEXT_PADDING - 1,
                                         lineHeight,
                                         is_last ? THEME_COLOR_TITLE : THEME_COLOR_TITLE_HISTORY); // Bar color for response
                _hal->canvas()->setTextColor(is_last ? TFT_WHITE : TFT_DARKGRAY);                  // Text color for response
            }

            _hal->canvas()->drawString(current_line_index >= _chat.size()
                                           ? partial_prompt[current_line_index - _chat.size()].c_str()
                                           : _chat[current_line_index].second.c_str(),
                                       TEXT_PADDING,
                                       y);
        }

        int scrollbar_height = lineHeight * maxVisibleLines;

        // Draw scroll indicators if needed
        if (_totalLines > maxVisibleLines)
        {
            int scrollbar_x = _hal->canvas()->width() - scrollbar_width - 1;
            _hal->canvas()->drawRect(scrollbar_x, textStartY, scrollbar_width, scrollbar_height, TFT_DARKGREY);
            int thumb_height = scrollbar_height * maxVisibleLines / _totalLines;
            int thumb_pos = textStartY + (scrollbar_height - thumb_height) * _scrollPosition / (_totalLines - maxVisibleLines);
            _hal->canvas()->fillRect(scrollbar_x, thumb_pos, scrollbar_width, thumb_height, TFT_ORANGE);
        }
    }
    // Update the display
    is_rendered = true;
    return true;
}

// Handle input on the response screen
void GeminiApp::handleResponseScreenInput()
{
    // Calculate maximum scroll position
    int lineHeight = _hal->canvas()->fontHeight(FONT_14);
    int textHeight = _hal->canvas()->height() - 14;
    int maxVisibleLines = textHeight / lineHeight;
    _hal->keyboard()->updateKeyList();
    _hal->keyboard()->updateKeysState();

    if (_hal->keyboard()->isPressed())
    {
        if (_hal->keyboard()->isKeyPressing(KEY_NUM_UP))
        {
            if (!is_repeat || !_hal->keyboard()->waitForRelease(KEY_NUM_UP, is_start ? KEY_HOLD_MS : KEY_REPEAT_MS))
            {
                is_start = !is_repeat;
                is_repeat = true;
                _hal->playNextSound();

                // Scroll up
                if (_scrollPosition > 0)
                {
                    _scrollPosition--;
                    is_rendered = false;
                }
            }
        }
        else if (_hal->keyboard()->isKeyPressing(KEY_NUM_DOWN))
        {
            if (!is_repeat || !_hal->keyboard()->waitForRelease(KEY_NUM_DOWN, is_start ? KEY_HOLD_MS : KEY_REPEAT_MS))
            {
                is_start = !is_repeat;
                is_repeat = true;
                _hal->playNextSound();

                // Scroll down if not at bottom
                if (_scrollPosition < _totalLines - maxVisibleLines)
                {
                    _scrollPosition++;
                    is_rendered = false;
                }
            }
        }
        else if (_hal->keyboard()->isKeyPressing(KEY_NUM_LEFT))
        {
            if (!is_repeat || !_hal->keyboard()->waitForRelease(KEY_NUM_LEFT, is_start ? KEY_HOLD_MS : KEY_REPEAT_MS))
            {
                is_start = !is_repeat;
                is_repeat = true;
                _hal->playNextSound();

                // Jump up by visible_items count (page up)
                int jump = maxVisibleLines;
                if (_scrollPosition > 0)
                {
                    _scrollPosition = std::max(0, _scrollPosition - jump);
                    is_rendered = false;
                }
            }
        }
        else if (_hal->keyboard()->isKeyPressing(KEY_NUM_RIGHT))
        {
            if (!is_repeat || !_hal->keyboard()->waitForRelease(KEY_NUM_RIGHT, is_start ? KEY_HOLD_MS : KEY_REPEAT_MS))
            {
                is_start = !is_repeat;
                is_repeat = true;
                _hal->playNextSound();

                // Jump down by visible_items count (page down)
                int jump = maxVisibleLines;
                if (_scrollPosition < _totalLines - maxVisibleLines)
                {
                    _scrollPosition = std::min(_totalLines - maxVisibleLines, _scrollPosition + jump);
                    is_rendered = false;
                }
            }
        }
        else if (_hal->keyboard()->isKeyPressing(KEY_NUM_ESC))
        {
            _hal->keyboard()->waitForRelease(KEY_NUM_ESC);
            _hal->playNextSound();

            setState(APP_STATE_IDLE);
            stopTTS();
            _partialPrompt = "";
            deepgram_streaming_stop();
            _currentScreen = SCREEN_START;
            is_rendered = false;
        }
        else if (_hal->keyboard()->isKeyPressing(KEY_NUM_BACKSPACE))
        {
            _hal->keyboard()->waitForRelease(KEY_NUM_ESC);
            _hal->playNextSound();

            setState(APP_STATE_IDLE);
            stopTTS();
            _partialPrompt = "";
            deepgram_streaming_stop();
            _currentScreen = SCREEN_START;
            is_rendered = false;
        }
        else if (_hal->keyboard()->isKeyPressing(KEY_NUM_ENTER))
        {
            _hal->keyboard()->waitForRelease(KEY_NUM_ENTER);
            _hal->playNextSound();
            stopTTS();
            // need to redraw after dialog anyway
            is_rendered = false;
            _partialPrompt = "";
            // hold Fn to enter first prompt
            if (_hal->settings()->getBool("deepgram", "enabled") && !_hal->keyboard()->keysState().fn)
            {
                _currentScreen = SCREEN_CHAT;
            }
            else
            {
                deepgram_streaming_stop();
                // hold Fn to keep prompt
                if (!_hal->keyboard()->keysState().fn)
                    _userPrompt = "";
                // Show the input dialog to get user's prompt
                bool result = UTILS::UI::show_edit_string_dialog(_hal, "Enter your prompt", _userPrompt, false, 1000);

                if (result && !_userPrompt.empty())
                {
                    setState(APP_STATE_CONNECTING_GEMINI);
                    xEventGroupClearBits(_control_event_group, STT_TRANSCRIPT_BIT);
                    // Call the API asynchronously to start the task
                    _history.push_back({_userPrompt, "..."});
                    callGeminiAPI();
                    _currentScreen = SCREEN_CHAT;
                }
            }
        }
    }
    else
        is_repeat = false;
}

// Call Gemini API with proper error handling
void GeminiApp::callGeminiAPI()
{
    // Clear any previous bits
    xEventGroupClearBits(_control_event_group, GEMINI_TASK_STARTED_BIT | GEMINI_TASK_STOPPED_BIT);

    // Create the Gemini API task
    BaseType_t result = xTaskCreate(gemini_task,
                                    "gemini_task",
                                    TTS_TASK_STACK_SIZE, // Reuse the TTS stack size since they're similar tasks
                                    this,
                                    TTS_TASK_PRIORITY,     // Similar priority to TTS
                                    &_gemini_task_handle); // Same core as TTS

    if (result != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create Gemini API task");
        _gemini_task_handle = nullptr;
    }
}

// Gemini API task handler function
void GeminiApp::gemini_task(void* parameter)
{
    GeminiApp* app = static_cast<GeminiApp*>(parameter);
    HAL::Hal* hal = app->_hal;
    EventGroupHandle_t control_event_group = app->_control_event_group;
    std::string prompt = app->_userPrompt;

    // Set the started bit
    xEventGroupSetBits(control_event_group, GEMINI_TASK_STARTED_BIT);

    ESP_LOGD(TAG, "Gemini API task started");

    // Get the API settings from the HAL settings
    std::string model = hal->settings()->getString(GEMINI_NS, "model");
    std::string api_key = hal->settings()->getString(GEMINI_NS, "api_key");
    std::string rules = hal->settings()->getString(GEMINI_NS, "rules");

    // Call the Google API with the history and prompt
    std::string response = call_google_api(model, api_key, rules, app->_history, prompt);

    // Store the response in the class
    app->_apiResponse = response;

    // Signal task completion by setting the stopped bit
    xEventGroupSetBits(control_event_group, GEMINI_TASK_STOPPED_BIT);

    ESP_LOGD(TAG, "Gemini API task completed");

    // Task deletes itself
    vTaskDelete(NULL);
}

// Method to get the response from the API task
std::string GeminiApp::get_response()
{
    // Wait for the API task to complete with a timeout
    EventBits_t bits = xEventGroupWaitBits(_control_event_group,
                                           GEMINI_TASK_STOPPED_BIT,
                                           pdFALSE,               // Don't clear the bits
                                           pdTRUE,                // Wait for all bits
                                           pdMS_TO_TICKS(30000)); // 30-second timeout (API calls can take time)

    // Check if we got a valid response
    if ((bits & GEMINI_TASK_STOPPED_BIT) != GEMINI_TASK_STOPPED_BIT)
    {
        ESP_LOGE(TAG, "Timeout waiting for Gemini API response");
        return "Error: API request timed out";
    }

    // Clear the bits for next use
    xEventGroupClearBits(_control_event_group, GEMINI_TASK_STARTED_BIT | GEMINI_TASK_STOPPED_BIT);

    return _apiResponse;
}

// Draw the Gemini QR code screen
bool GeminiApp::drawGeminiQRScreen()
{
    if (is_rendered)
        return false;

    // Clear the screen
    _hal->canvas()->fillScreen(THEME_COLOR_BG);
    _hal->canvas()->pushImage(0, 0, 135, 135, image_data_qr_gemini);
    _hal->canvas()->setFont(FONT_16);
    _hal->canvas()->setTextColor(THEME_COLOR_TITLE);
    int center_x = _hal->canvas()->height() + (_hal->canvas()->width() - _hal->canvas()->height()) / 2;
    _hal->canvas()->drawCenterString("Gemini", center_x, _hal->canvas()->height() / 2 - _hal->canvas()->fontWidth());
    _hal->canvas()->setTextColor(TFT_LIGHTGREY);
    _hal->canvas()->drawCenterString("Get API key", center_x, _hal->canvas()->height() / 2 + _hal->canvas()->fontWidth());

    is_rendered = true;
    return true;
}

// Draw the ElevenLabs QR code screen
bool GeminiApp::drawElevenLabsQRScreen()
{
    if (is_rendered)
        return false;

    // Clear the screen
    _hal->canvas()->fillScreen(THEME_COLOR_BG);
    _hal->canvas()->pushImage(0, 0, 135, 135, image_data_qr_elevenlabs);
    _hal->canvas()->setFont(FONT_16);
    _hal->canvas()->setTextColor(TFT_ORANGE);
    int center_x = _hal->canvas()->height() + (_hal->canvas()->width() - _hal->canvas()->height()) / 2;
    _hal->canvas()->drawCenterString("ElevenLabs", center_x, _hal->canvas()->height() / 2 - _hal->canvas()->fontWidth());
    _hal->canvas()->setTextColor(TFT_LIGHTGREY);
    _hal->canvas()->drawCenterString("Get API key", center_x, _hal->canvas()->height() / 2 + _hal->canvas()->fontWidth());
    is_rendered = true;
    return true;
}

// Draw the Deepgram QR code screen
bool GeminiApp::drawDeepgramQRScreen()
{
    if (is_rendered)
        return false;

    // Clear the screen
    _hal->canvas()->fillScreen(THEME_COLOR_BG);
    _hal->canvas()->pushImage(0, 0, 135, 135, image_data_qr_deepgram);
    _hal->canvas()->setFont(FONT_16);
    _hal->canvas()->setTextColor(TFT_VIOLET);
    int center_x = _hal->canvas()->height() + (_hal->canvas()->width() - _hal->canvas()->height()) / 2;
    _hal->canvas()->drawCenterString("Deepgram", center_x, _hal->canvas()->height() / 2 - _hal->canvas()->fontWidth());
    _hal->canvas()->setTextColor(TFT_LIGHTGREY);
    _hal->canvas()->drawCenterString("Get API key", center_x, _hal->canvas()->height() / 2 + _hal->canvas()->fontWidth());
    is_rendered = true;
    return true;
}

// Handle input on the API key screen
void GeminiApp::handleApiKeyScreenInput()
{
    _hal->keyboard()->updateKeyList();
    _hal->keyboard()->updateKeysState();
    if (_hal->keyboard()->isPressed())
    {
        if (_hal->keyboard()->isKeyPressing(KEY_NUM_ESC))
        {
            _hal->keyboard()->waitForRelease(KEY_NUM_ESC);
            _hal->playNextSound();

            _currentScreen = SCREEN_SETTINGS;
            is_rendered = false;
        }
        else if (_hal->keyboard()->isKeyPressing(KEY_NUM_ENTER))
        {
            _hal->keyboard()->waitForRelease(KEY_NUM_ENTER);
            _hal->playNextSound();

            _currentScreen = SCREEN_SETTINGS;
            is_rendered = false;
        }
    }
}

void GeminiApp::updateScrollPosition()
{
    // no partial responce - show last request item
    _scrollPosition = _partialPrompt.empty() ? _lastHistoryIndex : _chat.size();
}