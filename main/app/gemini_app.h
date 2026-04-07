#pragma once

#include "hal/hal.h"
#include "settings/settings.h"
#include "app/utils/anim/hl_text.h"
#include "app/utils/anim/scroll_text.h"
#include "app/s2s_client.h"
#include <string>
#include <vector>
#include <functional>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

enum GeminiAppScreen
{
    SCREEN_START,
    SCREEN_SETTINGS,
    SCREEN_QR_GEMINI,
    SCREEN_CHAT,
};

enum AppState
{
    APP_STATE_IDLE,
    APP_STATE_DISCONNECTED,
    APP_STATE_CONNECTING_WIFI,
    APP_STATE_S2S_CONNECTING,
    APP_STATE_S2S_LISTENING,
    APP_STATE_S2S_SPEAKING,
    APP_STATE_S2S_ERROR,
};

enum HistoryItemType
{
    HISTORY_ITEM_TYPE_USER,
    HISTORY_ITEM_TYPE_MODEL,
};

#define FONT_16 &fonts::efontEN_16
#define TEXT_PADDING 5
#define MAX_CHAT_LINES 1000

class GeminiApp
{
private:
    HAL::Hal* _hal;
    HAL::wifi_status_t _wifiStatus;

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

    UTILS::HL_TEXT::HLTextContext_t* _hintTextContext = nullptr;
    UTILS::SCROLL_TEXT::ScrollTextContext_t* _descScrollContext = nullptr;

    // Chat display
    int _totalLines;
    std::string _userPrompt;
    std::string _partialPrompt;
    int _lastHistoryIndex;
    int _scrollPosition;

    std::vector<std::pair<int, std::string>> _chat;

    EventGroupHandle_t _control_event_group = nullptr;

    // S2S voice mode
    S2SSharedData _s2s_shared;
    bool _s2s_active = false;
    void startS2S();
    void stopS2S();

    // Drawing
    bool drawMainScreen();
    void handleMainScreenInput();
    bool drawAnimation(bool need_update);
    void handleSettingsMenu();

    bool drawResponseScreen();
    void handleResponseScreenInput();
    void updateScrollPosition();

    bool drawGeminiQRScreen();
    void handleApiKeyScreenInput();

    const char* getHintForState() const;
    std::vector<std::string> splitTextIntoLines(const std::string& text);
    void trimChat();

public:
    GeminiApp(HAL::Hal* hal);
    ~GeminiApp();

    void init();
    void update();
};
