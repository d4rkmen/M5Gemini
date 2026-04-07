#include "gemini_app.h"
#include "audio_buffer.h"
#include "esp_log.h"
#include "esp_system.h"
#include "app/utils/ui/dialog.h"
#include "app/utils/ui/settings_screen.h"
#include "app/utils/ui/key_repeat.h"
#include "app/utils/theme/theme_define.h"
#include "s2s_client.h"
#include <vector>
#include "assets/g_fonts.hpp"
#include "assets/gemini_icon.h"
#include "assets/qr_gemini.h"
#include "assets/anm_disconnected.h"
#include "assets/anm_wifi.h"
#include "assets/anm_internet.h"
#include "assets/anm_error.h"
#include "assets/anm_playing3.h"
#include "assets/anm_mic.h"

#include "hal_cardputer.h"
#include "wifi/wifi.h"

static const char* TAG = "GEMINI_APP";
static const char* GEMINI_NS = "gemini";
static const char* HINT_MAIN = "[ENTER] START [ESC] SETTINGS";
static const char* API_KEY_HINT = "[ENTER] [ESC]";

static bool is_repeat = false;
static uint32_t next_fire_ts = 0xFFFFFFFF;
static bool is_rendered = false;

#define HINT_HEIGHT 12

extern const uint8_t snap_wav_start[] asm("_binary_snap_wav_start");
extern const uint8_t snap_wav_end[] asm("_binary_snap_wav_end");

// ═══════════════════════════════════════════════════════════
// Constructor / Destructor
// ═══════════════════════════════════════════════════════════

GeminiApp::GeminiApp(HAL::Hal* hal) : _hal(hal), _currentScreen(SCREEN_START), _scrollPosition(0)
{
    _hal->canvas()->setFont(FONT_16);
    _hintTextContext = new UTILS::HL_TEXT::HLTextContext_t();
    UTILS::HL_TEXT::hl_text_init(_hintTextContext, _hal->canvas(), 20, 1500);

    _descScrollContext = new UTILS::SCROLL_TEXT::ScrollTextContext_t();
    UTILS::SCROLL_TEXT::scroll_text_init(_descScrollContext, _hal->canvas(), _hal->canvas()->width(), 16, 20, 1000);

    _control_event_group = xEventGroupCreate();
    xEventGroupClearBits(_control_event_group, 0xFFFFFF);

    _sprite = new LGFX_Sprite(_hal->canvas());
    _sprite->createSprite(50, 50);

    _s2s_shared.mutex = xSemaphoreCreateMutex();
    _s2s_active = false;
}

GeminiApp::~GeminiApp()
{
    stopS2S();

    _sprite->deleteSprite();
    delete _sprite;

    if (_control_event_group != nullptr)
    {
        vEventGroupDelete(_control_event_group);
        _control_event_group = nullptr;
    }

    if (_s2s_shared.mutex)
    {
        vSemaphoreDelete(_s2s_shared.mutex);
        _s2s_shared.mutex = nullptr;
    }

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

// ═══════════════════════════════════════════════════════════
// S2S session management
// ═══════════════════════════════════════════════════════════

void GeminiApp::startS2S()
{
    if (_s2s_active)
        stopS2S();

    std::string api_key = _hal->settings()->getString(GEMINI_NS, "api_key");
    std::string model = _hal->settings()->getString(GEMINI_NS, "model");
    std::string voice = _hal->settings()->getString(GEMINI_NS, "voice");
    std::string rules = _hal->settings()->getString(GEMINI_NS, "rules");
    int32_t volume = _hal->settings()->getNumber(GEMINI_NS, "volume");

    if (api_key.empty())
    {
        UTILS::UI::show_error_dialog(_hal, "Failed", "API key not set. Please go to settings", "OK");
        return;
    }

    xEventGroupClearBits(_control_event_group, S2S_FATAL_ERROR_BIT | S2S_ERROR_BIT);
    if (xSemaphoreTake(_s2s_shared.mutex, pdMS_TO_TICKS(200)) == pdTRUE)
    {
        _s2s_shared.input_transcript.clear();
        _s2s_shared.output_transcript.clear();
        _s2s_shared.error_message.clear();
        xSemaphoreGive(_s2s_shared.mutex);
    }

    if (s2s_start(_hal, _control_event_group, &_s2s_shared, api_key, model, voice, rules, volume))
    {
        _s2s_active = true;
        setState(APP_STATE_S2S_CONNECTING);
    }
    else
    {
        UTILS::UI::show_error_dialog(_hal, "Failed", "Could not start S2S session", "OK");
        setState(APP_STATE_IDLE);
    }
}

void GeminiApp::stopS2S()
{
    if (!_s2s_active)
        return;
    s2s_stop();
    _s2s_active = false;
}

// ═══════════════════════════════════════════════════════════
// Init
// ═══════════════════════════════════════════════════════════

void GeminiApp::init()
{
    ESP_LOGI(TAG, "Initializing");
    setState(APP_STATE_DISCONNECTED);

    _groups = _hal->settings()->getMetadata();
    _groups[2].callback = [this](SETTINGS::SettingGroup_t& group)
    {
        if (_hal->settings()->getString("gemini", "api_key").empty())
        {
            _currentScreen = SCREEN_QR_GEMINI;
            is_rendered = false;
        }
    };

    if (_hal->wifi()->init())
    {
        _hal->wifi()->set_status_callback(
            [this](HAL::wifi_status_t status)
            {
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
        if (_hal->settings()->getBool("wifi", "enabled"))
            _hal->wifi()->connect();
    }

    int brightness = _hal->settings()->getNumber("system", "brightness");
    if (brightness > 0)
        _hal->display()->setBrightness(brightness);

    int volume = _hal->settings()->getNumber("system", "volume");
    if (volume > 0)
    {
        _hal->speaker()->setVolume(255);
        _hal->speaker()->setChannelVolume(SYSTEM_CHANNEL, volume);
    }

    if (_hal->settings()->getBool("system", "boot_sound"))
        _hal->speaker()->playWav(snap_wav_start, snap_wav_end - snap_wav_start, 1, SYSTEM_CHANNEL);

    _currentScreen = SCREEN_START;
    is_rendered = false;
}

// ═══════════════════════════════════════════════════════════
// Hints
// ═══════════════════════════════════════════════════════════

const char* GeminiApp::getHintForState() const
{
    switch (_appState)
    {
    case APP_STATE_S2S_LISTENING:
        return "[ENTER] SEND [\u2191][\u2193] SCROLL [ESC] STOP";
    case APP_STATE_S2S_SPEAKING:
        return "[ENTER] SKIP [\u2191][\u2193] SCROLL [ESC] STOP";
    case APP_STATE_S2S_CONNECTING:
    case APP_STATE_S2S_ERROR:
        return "[ESC] CANCEL";
    default:
        return "[\u2191][\u2193][\u2190][\u2192] SCROLL [ESC] BACK";
    }
}

// ═══════════════════════════════════════════════════════════
// Main update loop
// ═══════════════════════════════════════════════════════════

void GeminiApp::update()
{
    bool need_update = false;
    EventBits_t bits = xEventGroupGetBits(_control_event_group);

    switch (_currentScreen)
    {
    case SCREEN_START:
        need_update |= drawMainScreen();
        need_update |= drawAnimation(need_update);
        handleMainScreenInput();
        need_update |= UTILS::HL_TEXT::hl_text_render(_hintTextContext,
                                                      HINT_MAIN,
                                                      0,
                                                      _hal->canvas()->height() - HINT_HEIGHT,
                                                      TFT_DARKGREY,
                                                      TFT_WHITE,
                                                      THEME_COLOR_BG);
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

    case SCREEN_SETTINGS:
        handleSettingsMenu();
        break;

    case SCREEN_CHAT:
        // ── S2S state machine ──
        switch (_appState)
        {
        case APP_STATE_DISCONNECTED:
        case APP_STATE_CONNECTING_WIFI:
        case APP_STATE_IDLE:
            break;

        case APP_STATE_S2S_CONNECTING:
            if (bits & S2S_SETUP_COMPLETE_BIT)
            {
                setState(APP_STATE_S2S_LISTENING);
                is_rendered = false;
            }
            else if (bits & S2S_ERROR_BIT)
            {
                setState(APP_STATE_S2S_ERROR);
                is_rendered = false;
            }
            break;

        case APP_STATE_S2S_LISTENING:
        {
            if (bits & S2S_INPUT_TRANSCRIPT_BIT)
            {
                xEventGroupClearBits(_control_event_group, S2S_INPUT_TRANSCRIPT_BIT);
                if (xSemaphoreTake(_s2s_shared.mutex, pdMS_TO_TICKS(50)) == pdTRUE)
                {
                    _partialPrompt = _s2s_shared.input_transcript;
                    xSemaphoreGive(_s2s_shared.mutex);
                }
                updateScrollPosition();
                is_rendered = false;
            }

            if ((bits & S2S_MODEL_SPEAKING_BIT) && !(bits & S2S_LISTENING_BIT))
            {
                if (xSemaphoreTake(_s2s_shared.mutex, pdMS_TO_TICKS(50)) == pdTRUE)
                {
                    _userPrompt = _s2s_shared.input_transcript;
                    _s2s_shared.input_transcript.clear();
                    _s2s_shared.output_transcript.clear();
                    xSemaphoreGive(_s2s_shared.mutex);
                }
                if (!_userPrompt.empty())
                {
                    _lastHistoryIndex = _chat.size();
                    for (const auto& line : splitTextIntoLines(_userPrompt))
                        _chat.push_back({HISTORY_ITEM_TYPE_USER, line});
                    trimChat();
                }
                _partialPrompt = "";
                updateScrollPosition();
                setState(APP_STATE_S2S_SPEAKING);
                is_rendered = false;
            }

            if (bits & (S2S_ERROR_BIT | S2S_TASK_STOPPED_BIT))
            {
                setState(APP_STATE_S2S_ERROR);
                is_rendered = false;
            }
            break;
        }

        case APP_STATE_S2S_SPEAKING:
        {
            // Late-arriving input transcript (server often sends after model starts speaking)
            if ((bits & S2S_INPUT_TRANSCRIPT_BIT) && _userPrompt.empty())
            {
                xEventGroupClearBits(_control_event_group, S2S_INPUT_TRANSCRIPT_BIT);
                if (xSemaphoreTake(_s2s_shared.mutex, pdMS_TO_TICKS(50)) == pdTRUE)
                {
                    _userPrompt = _s2s_shared.input_transcript;
                    xSemaphoreGive(_s2s_shared.mutex);
                }
                if (!_userPrompt.empty())
                {
                    _lastHistoryIndex = _chat.size();
                    for (const auto& line : splitTextIntoLines(_userPrompt))
                        _chat.push_back({HISTORY_ITEM_TYPE_USER, line});
                    trimChat();
                    updateScrollPosition();
                    is_rendered = false;
                }
            }

            if (bits & S2S_OUTPUT_TRANSCRIPT_BIT)
            {
                xEventGroupClearBits(_control_event_group, S2S_OUTPUT_TRANSCRIPT_BIT);
                if (xSemaphoreTake(_s2s_shared.mutex, pdMS_TO_TICKS(50)) == pdTRUE)
                {
                    _partialPrompt = _s2s_shared.output_transcript;
                    xSemaphoreGive(_s2s_shared.mutex);
                }
                updateScrollPosition();
                is_rendered = false;
            }

            if ((bits & S2S_LISTENING_BIT) && !(bits & S2S_MODEL_SPEAKING_BIT))
            {
                std::string model_text;
                if (xSemaphoreTake(_s2s_shared.mutex, pdMS_TO_TICKS(50)) == pdTRUE)
                {
                    model_text = _s2s_shared.output_transcript;
                    _s2s_shared.output_transcript.clear();
                    xSemaphoreGive(_s2s_shared.mutex);
                }
                if (!model_text.empty())
                {
                    for (const auto& line : splitTextIntoLines(model_text))
                        _chat.push_back({HISTORY_ITEM_TYPE_MODEL, line});
                    trimChat();
                }
                _partialPrompt = "";
                updateScrollPosition();
                setState(APP_STATE_S2S_LISTENING);
                is_rendered = false;
            }

            if (bits & (S2S_ERROR_BIT | S2S_TASK_STOPPED_BIT))
            {
                setState(APP_STATE_S2S_ERROR);
                is_rendered = false;
            }
            break;
        }

        case APP_STATE_S2S_ERROR:
            if (bits & S2S_TASK_STOPPED_BIT)
            {
                if (bits & S2S_FATAL_ERROR_BIT)
                {
                    std::string err_msg;
                    if (xSemaphoreTake(_s2s_shared.mutex, pdMS_TO_TICKS(100)) == pdTRUE)
                    {
                        err_msg = _s2s_shared.error_message;
                        _s2s_shared.error_message.clear();
                        xSemaphoreGive(_s2s_shared.mutex);
                    }
                    xEventGroupClearBits(_control_event_group, S2S_FATAL_ERROR_BIT | S2S_ERROR_BIT);
                    stopS2S();
                    UTILS::UI::show_error_dialog(_hal, "Error", err_msg.empty() ? "Session failed" : err_msg);
                    _currentScreen = SCREEN_START;
                    setState(APP_STATE_IDLE);
                    is_rendered = false;
                }
                else
                {
                    ESP_LOGW(TAG, "S2S session ended, attempting reconnect");
                    stopS2S();
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    startS2S();
                    is_rendered = false;
                }
            }
            break;
        }

        need_update |= drawResponseScreen();
        need_update |= drawAnimation(need_update);
        handleResponseScreenInput();
        need_update |= UTILS::HL_TEXT::hl_text_render(_hintTextContext,
                                                      getHintForState(),
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

// ═══════════════════════════════════════════════════════════
// Main screen
// ═══════════════════════════════════════════════════════════

bool GeminiApp::drawMainScreen()
{
    if (is_rendered)
        return false;

    _chat.clear();
    _partialPrompt = "";

    _hal->canvas()->fillScreen(THEME_COLOR_BG);
    _hal->canvas()->fillRect(0, 0, _hal->canvas()->width(), 20, THEME_COLOR_BG);
    _hal->canvas()->setTextColor(THEME_COLOR_TITLE);
    _hal->canvas()->setFont(FONT_16);

    int offestY = TEXT_PADDING;
    int lineHeight = _hal->canvas()->fontHeight(FONT_16);
    _hal->canvas()->drawCenterString("Gemini AI", _hal->canvas()->width() / 2, offestY);
    offestY += lineHeight * 2;
    _hal->canvas()->pushImage((_hal->canvas()->width() - 64) / 2, offestY, 64, 64, image_data_gemini_icon, TFT_BLACK);

    _hal->canvas()->pushImage((_hal->canvas()->width() - 64) / 2 - 50 - 8, offestY + 7, 50, 50, image_data_mic, TFT_BLACK);
    _hal->canvas()->pushImage((_hal->canvas()->width() - 64) / 2 + 64 + 8, offestY + 7, 50, 50, image_data_playing3, TFT_BLACK);

    offestY += 64 + 2;
    _hal->canvas()->setTextColor(TFT_WHITE);
    _hal->canvas()->drawCenterString("v" BUILD_NUMBER, _hal->canvas()->width() / 2, offestY);

    is_rendered = true;
    return true;
}

void GeminiApp::handleMainScreenInput()
{
    _hal->keyboard()->updateKeyList();
    _hal->keyboard()->updateKeysState();

    if (_hal->keyboard()->isKeyPressing(KEY_NUM_ENTER))
    {
        _hal->keyboard()->waitForRelease(KEY_NUM_ENTER);
        _hal->playNextSound();
        is_rendered = false;

        if (_hal->settings()->getString(GEMINI_NS, "api_key").empty())
        {
            UTILS::UI::show_error_dialog(_hal, "Failed", "API key not set. Go to settings", "OK");
            return;
        }
        if (!_hal->settings()->getBool("wifi", "enabled"))
        {
            UTILS::UI::show_error_dialog(_hal, "Failed", "WiFi is disabled", "OK");
            return;
        }

        _currentScreen = SCREEN_CHAT;
        startS2S();
    }
    else if (_hal->keyboard()->isKeyPressing(KEY_NUM_ESC))
    {
        _hal->keyboard()->waitForRelease(KEY_NUM_ESC);
        _hal->playNextSound();
        _currentScreen = SCREEN_SETTINGS;
        setState(APP_STATE_IDLE);
        is_rendered = false;
    }
}

// ═══════════════════════════════════════════════════════════
// Settings & QR
// ═══════════════════════════════════════════════════════════

void GeminiApp::handleSettingsMenu()
{
    bool need_update = UTILS::UI::SETTINGS_SCREEN::update(_hal,
                                                          _groups,
                                                          _hintTextContext,
                                                          _descScrollContext,
                                                          [this]()
                                                          {
                                                              _currentScreen = SCREEN_START;
                                                              is_rendered = false;
                                                          });
    if (need_update)
        _hal->canvas_update();
}

bool GeminiApp::drawGeminiQRScreen()
{
    if (is_rendered)
        return false;
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

void GeminiApp::handleApiKeyScreenInput()
{
    _hal->keyboard()->updateKeyList();
    _hal->keyboard()->updateKeysState();
    if (_hal->keyboard()->isPressed())
    {
        if (_hal->keyboard()->isKeyPressing(KEY_NUM_ESC) || _hal->keyboard()->isKeyPressing(KEY_NUM_ENTER))
        {
            _hal->keyboard()->waitForRelease(KEY_NUM_ESC);
            _hal->playNextSound();
            _currentScreen = SCREEN_SETTINGS;
            is_rendered = false;
        }
    }
}

// ═══════════════════════════════════════════════════════════
// Response / Chat screen
// ═══════════════════════════════════════════════════════════

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
        if (_hal->canvas()->textWidth(currentLine.c_str(), FONT_14) > _hal->canvas()->width() - (TEXT_PADDING * 2))
        {
            size_t lastSpace = currentLine.find_last_of(' ');
            if (lastSpace != std::string::npos)
            {
                lines.push_back(currentLine.substr(0, lastSpace));
                currentLine = currentLine.substr(lastSpace + 1);
            }
            else
            {
                lines.push_back(currentLine);
                currentLine.clear();
            }
        }
    }
    if (!currentLine.empty())
        lines.push_back(currentLine);
    return lines;
}

bool GeminiApp::drawResponseScreen()
{
    if (is_rendered)
        return false;
    int scrollbar_width = 5;
    _hal->canvas()->fillScreen(THEME_COLOR_BG);
    _hal->canvas()->setFont(FONT_14);

    int textStartY = TEXT_PADDING;
    int textHeight = _hal->canvas()->height() - textStartY - HINT_HEIGHT;

    if (_chat.empty() && _partialPrompt.empty())
    {
        const char* status_text = "connecting...";
        if (_appState == APP_STATE_S2S_LISTENING)
            status_text = "listening...";
        else if (_appState == APP_STATE_S2S_SPEAKING)
            status_text = "speaking...";
        else if (_appState == APP_STATE_S2S_ERROR)
            status_text = "reconnecting...";

        _hal->canvas()->setTextColor(TFT_DARKGRAY);
        _hal->canvas()->drawCenterString(status_text, _hal->canvas()->width() / 2, _hal->canvas()->height() / 2, FONT_16);
    }
    else
    {
        int lineHeight = _hal->canvas()->fontHeight(FONT_14);
        int maxVisibleLines = textHeight / lineHeight;
        std::vector<std::string> partial_prompt = splitTextIntoLines(_partialPrompt);
        _totalLines = _chat.size() + partial_prompt.size();

        if (_scrollPosition > _totalLines - maxVisibleLines)
            _scrollPosition = std::max(0, _totalLines - maxVisibleLines);

        // Partial prompt type depends on current state:
        // listening → user input transcript, speaking → model output transcript
        bool partial_is_user = (_appState == APP_STATE_S2S_LISTENING);

        for (int i = 0; i < maxVisibleLines && i + _scrollPosition < _totalLines; i++)
        {
            int y = textStartY + (i * lineHeight);
            int idx = i + _scrollPosition;

            bool is_partial = (idx >= (int)_chat.size());
            bool is_user;
            if (is_partial)
                is_user = partial_is_user;
            else
                is_user = (_chat[idx].first == HISTORY_ITEM_TYPE_USER);

            bool is_last = idx >= _lastHistoryIndex;

            if (is_user)
            {
                _hal->canvas()->fillRect(0, y, TEXT_PADDING - 1, lineHeight, is_last ? TFT_WHITE : TFT_DARKGRAY);
                _hal->canvas()->setTextColor(is_last ? THEME_COLOR_TITLE : THEME_COLOR_TITLE_HISTORY);
            }
            else
            {
                _hal->canvas()->fillRect(0,
                                         y,
                                         TEXT_PADDING - 1,
                                         lineHeight,
                                         is_last ? THEME_COLOR_TITLE : THEME_COLOR_TITLE_HISTORY);
                _hal->canvas()->setTextColor(is_last ? TFT_WHITE : TFT_DARKGRAY);
            }

            _hal->canvas()->drawString(is_partial ? partial_prompt[idx - _chat.size()].c_str() : _chat[idx].second.c_str(),
                                       TEXT_PADDING,
                                       y);
        }

        int scrollbar_height = lineHeight * maxVisibleLines;
        if (_totalLines > maxVisibleLines)
        {
            int sx = _hal->canvas()->width() - scrollbar_width - 1;
            _hal->canvas()->drawRect(sx, textStartY, scrollbar_width, scrollbar_height, TFT_DARKGREY);
            int th = scrollbar_height * maxVisibleLines / _totalLines;
            int tp = textStartY + (scrollbar_height - th) * _scrollPosition / (_totalLines - maxVisibleLines);
            _hal->canvas()->fillRect(sx, tp, scrollbar_width, th, TFT_ORANGE);
        }
    }
    is_rendered = true;
    return true;
}

void GeminiApp::handleResponseScreenInput()
{
    int lineHeight = _hal->canvas()->fontHeight(FONT_14);
    int textHeight = _hal->canvas()->height() - 14;
    int maxVisibleLines = textHeight / lineHeight;
    _hal->keyboard()->updateKeyList();
    _hal->keyboard()->updateKeysState();

    if (_hal->keyboard()->isPressed())
    {
        uint32_t now = millis();

        if (_hal->keyboard()->isKeyPressing(KEY_NUM_UP))
        {
            if (key_repeat_check(is_repeat, next_fire_ts, now))
            {
                if (_scrollPosition > 0)
                {
                    _scrollPosition--;
                    is_rendered = false;
                }
            }
        }
        else if (_hal->keyboard()->isKeyPressing(KEY_NUM_DOWN))
        {
            if (key_repeat_check(is_repeat, next_fire_ts, now))
            {
                if (_scrollPosition < _totalLines - maxVisibleLines)
                {
                    _scrollPosition++;
                    is_rendered = false;
                }
            }
        }
        else if (_hal->keyboard()->isKeyPressing(KEY_NUM_LEFT))
        {
            if (key_repeat_check(is_repeat, next_fire_ts, now))
            {
                if (_scrollPosition > 0)
                {
                    _scrollPosition = std::max(0, _scrollPosition - maxVisibleLines);
                    is_rendered = false;
                }
            }
        }
        else if (_hal->keyboard()->isKeyPressing(KEY_NUM_RIGHT))
        {
            if (key_repeat_check(is_repeat, next_fire_ts, now))
            {
                if (_scrollPosition < _totalLines - maxVisibleLines)
                {
                    _scrollPosition = std::min(_totalLines - maxVisibleLines, _scrollPosition + maxVisibleLines);
                    is_rendered = false;
                }
            }
        }
        else if (_hal->keyboard()->isKeyPressing(KEY_NUM_ESC) || _hal->keyboard()->isKeyPressing(KEY_NUM_BACKSPACE))
        {
            _hal->keyboard()->waitForRelease(KEY_NUM_ESC);
            _hal->playNextSound();
            stopS2S();
            setState(APP_STATE_IDLE);
            _partialPrompt = "";
            _currentScreen = SCREEN_START;
            is_rendered = false;
        }
        else if (_hal->keyboard()->isKeyPressing(KEY_NUM_ENTER))
        {
            _hal->keyboard()->waitForRelease(KEY_NUM_ENTER);
            _hal->playNextSound();
            is_rendered = false;

            if (_appState == APP_STATE_S2S_LISTENING || _appState == APP_STATE_S2S_SPEAKING)
            {
                xEventGroupSetBits(_control_event_group, S2S_USER_INTERRUPT_BIT);
            }
            else if (!_s2s_active)
            {
                startS2S();
            }
        }
    }
    else
    {
        is_repeat = false;
    }
}

void GeminiApp::updateScrollPosition() { _scrollPosition = _partialPrompt.empty() ? _lastHistoryIndex : (int)_chat.size(); }

void GeminiApp::trimChat()
{
    if ((int)_chat.size() > MAX_CHAT_LINES)
    {
        int excess = _chat.size() - MAX_CHAT_LINES;
        _chat.erase(_chat.begin(), _chat.begin() + excess);
        _lastHistoryIndex = std::max(0, _lastHistoryIndex - excess);
        _scrollPosition = std::max(0, _scrollPosition - excess);
    }
}

// ═══════════════════════════════════════════════════════════
// Animation & state
// ═══════════════════════════════════════════════════════════

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
float easeInOutQuint(float t) { return t < 0.5 ? 16.0f * t * t * t * t * t : 1.0f - powf(-2.0f * t + 2.0f, 5.0f) / 2.0f; }
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
    ESP_LOGI(TAG, "State -> %d, heap=%lu", state, (unsigned long)esp_get_free_heap_size());
    _appState = state;
    _anim_context.timer_start = millis();
}

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
    case APP_STATE_S2S_ERROR:
        _sprite->pushImage(0, 0, 50, 50, image_data_conn_error);
        break;
    case APP_STATE_CONNECTING_WIFI:
        _sprite->pushImage(0, 0, 50, 50, image_data_connecting_wifi);
        break;
    case APP_STATE_S2S_CONNECTING:
        _sprite->pushImage(0, 0, 50, 50, image_data_connecting_internet);
        break;
    case APP_STATE_S2S_SPEAKING:
        _sprite->pushImage(0, 0, 50, 50, image_data_playing3);
        break;
    case APP_STATE_S2S_LISTENING:
        _sprite->pushImage(0, 0, 50, 50, image_data_mic);
        break;
    }

    uint32_t timer_now = millis();
    int x_offset = _hal->canvas()->width() - _sprite->width() - 1;
    int y_offset = 0;

    uint32_t full_pos = (timer_now - _anim_context.timer_start) % (_anim_context.duration * 2);
    uint32_t timer_pos = (full_pos % _anim_context.duration) * _anim_context.steps / _anim_context.duration;
    float ifloat;
    if (full_pos >= _anim_context.duration)
        ifloat = easeOutExpo(float(_anim_context.steps - timer_pos) / float(_anim_context.steps));
    else
        ifloat = easeOutExpo(float(timer_pos) / float(_anim_context.steps));

    uint8_t alpha = ifloat * 255;
    if (alpha == _anim_context.last_alpha && need_update == false)
        return false;
    _anim_context.last_alpha = alpha;

    for (int py = 0; py < _sprite->height(); py++)
    {
        for (int px = 0; px < _sprite->width(); px++)
        {
            uint16_t fg = _sprite->readPixel(px, py);
            uint16_t bg = _hal->canvas()->readPixel(px + x_offset, py + y_offset);

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
