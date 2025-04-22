#pragma once

#include "hal/hal.h"
#include "settings/settings.h"
#include "app/utils/anim/hl_text.h"
#include "app/utils/anim/scroll_text.h"
#include <string>
#include <vector>
#include <functional>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "app/http_client.h"

// Application state
enum GeminiAppScreen
{
    SCREEN_START,
    SCREEN_SETTINGS,
    SCREEN_QR_GEMINI,
    SCREEN_QR_ELEVENLABS,
    SCREEN_QR_DEEPGRAM,
    SCREEN_CHAT,
};

// Application state
enum AppState
{
    APP_STATE_IDLE,
    APP_STATE_DISCONNECTED,
    APP_STATE_STREAM_ERROR,
    APP_STATE_CONNECTING_WIFI,
    APP_STATE_CONNECTING_STT,
    APP_STATE_LISTENING_MIC,
    APP_STATE_CONNECTING_GEMINI,
    APP_STATE_CONNECTING_TTS,
    APP_STATE_TTS_PLAYING,
};

enum HistoryItemType
{
    HISTORY_ITEM_TYPE_USER,
    HISTORY_ITEM_TYPE_MODEL,
};

// Font sizes and spacing
#define FONT_16 &fonts::efontEN_16
#define TEXT_PADDING 5

// Animation task parameters
#define TTS_TASK_STACK_SIZE 4096
#define TTS_TASK_PRIORITY 5
#define TTS_TASK_CORE 1

class GeminiApp
{
private:
    HAL::Hal* _hal;
    HAL::wifi_status_t _wifiStatus;

    // animation sprite
    LGFX_Sprite* _sprite;
    AppState _appState;
    struct
    {
        uint32_t duration = 1000;
        uint32_t timer_start = 0;
        uint32_t steps = 1000;
        uint8_t last_alpha = 255;
    } _anim_context;
    void setState(AppState state);

    GeminiAppScreen _currentScreen;
    std::vector<SETTINGS::SettingGroup_t> _groups;

    // UI resources
    UTILS::HL_TEXT::HLTextContext_t* _hintTextContext = nullptr;
    UTILS::SCROLL_TEXT::ScrollTextContext_t* _descScrollContext = nullptr;

    // Current conversation
    int _totalLines;
    std::string _userPrompt;
    std::string _partialPrompt;
    std::string _apiResponse;
    // for color highlight
    int _lastHistoryIndex;
    int _scrollPosition;

    std::vector<std::pair<int, std::string>> _chat;
    std::vector<std::pair<std::string, std::string>> _history;

    EventGroupHandle_t _control_event_group = nullptr;

    // Gemini API
    static void gemini_task(void* parameter);
    TaskHandle_t _gemini_task_handle = nullptr;
    std::string get_response();
    // Private methods
    bool drawMainScreen();
    void handleMainScreenInput();
    bool drawAnimation(bool need_update);
    void handleSettingsMenu();
    void drawLoadingScreen();

    bool drawResponseScreen();
    void handleResponseScreenInput();
    void updateScrollPosition();

    bool drawGeminiQRScreen();
    bool drawElevenLabsQRScreen();
    bool drawDeepgramQRScreen();
    void handleApiKeyScreenInput();

    void callGeminiAPI();
    std::vector<std::string> splitTextIntoLines(const std::string& text);

    // TTS
    void initTTS();
    void startTTS();
    void stopTTS();
    static void tts_stream_task(void* parameter);
    TaskHandle_t _tts_stream_task_handle = nullptr;

public:
    GeminiApp(HAL::Hal* hal);
    ~GeminiApp();

    void init();
    void update();
};