/**
 * @file settings.cpp
 * @brief Settings management system implementation
 */

#include "settings.h"
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <stdexcept>

static const char* TAG = "SETTINGS";

namespace SETTINGS
{

    const char* Settings::NVS_PARTITION = "apps_nvs";

    Settings::Settings() : _initialized(false)
    {
        // back item
        SettingItem_t back_item = {"", "[..]", TYPE_NONE, "back", "back", "", "", "Return back to the parent menu"};
        // Define metadata for all settings
        SettingGroup_t wifi_group;
        wifi_group.name = "WiFi settings";
        wifi_group.nvs_namespace = "wifi";
        wifi_group.items = {
            back_item,
            {"enabled", "Enabled", TYPE_BOOL, "false", "false", "", "", "Enable WiFi and connect to selected network"},
            {"ssid",
             "Network",
             TYPE_STRING,
             "",
             "",
             "",
             "",
             "WiFi network name (SSID) to connect to. Hold [Fn] key to enter SSID manually"},
            {"pass", "Password", TYPE_STRING, "", "", "", "", "WiFi network security password"},
            {"static_ip", "Static IP", TYPE_BOOL, "false", "false", "", "", "Use static IP address or let DHCP assign it"},
            {"ip", "IP addr", TYPE_STRING, "", "", "", "", "Static IP address to use or last DHCP assigned address"},
            {"mask", "Mask", TYPE_STRING, "", "", "", "", "Subnet mask. Need to be set if static IP is used"},
            {"gw", "Gateway", TYPE_STRING, "", "", "", "", "Gateway IP address. Need to be set if static IP is used"},
            {"dns", "DNS", TYPE_STRING, "", "", "", "", "DNS server IP address. Need to be set if static IP is used"}};

        SettingGroup_t sys_group;
        sys_group.name = "System settings";
        sys_group.nvs_namespace = "system";
        sys_group.items = {
            back_item,
            {"brightness", "Brightness", TYPE_NUMBER, "100", "100", "10", "255", "Screen brightness level (10-255)"},
            {"volume", "Volume", TYPE_NUMBER, "64", "64", "0", "255", "System sound volume level (0-255)"},
            {"boot_sound", "Boot sound", TYPE_BOOL, "true", "true", "", "", "Play boot sound on startup"},
        };

        SettingGroup_t gemini_group;
        gemini_group.name = "Gemini settings";
        gemini_group.nvs_namespace = "gemini";
        gemini_group.items = {
            back_item,
            {"api_key", "API key", TYPE_STRING, "", "", "", "", "API key for Google Gemini API"},
            {"model",
             "Model name",
             TYPE_STRING,
             "gemini-1.5-flash",
             "",
             "gemini-2.5-pro-preview-03-25;gemini-2.0-flash-thinking-exp-01-21;gemini-2.0-flash;gemini-2.0-flash-lite;"
             "gemini-1.5-pro;gemini-1.5-flash",
             "",
             "Model name to use for Gemini. Hold [Fn] to enter custom"},
            {"rules",
             "Rules",
             TYPE_STRING,
             "you are conversational AI assistant with STT and TTS features running on M5 Cardputer the ESP32-S3 device. your "
             "name is Cardputer. always answer using ASCII characters only. limit your response to 800 tokens. give short "
             "direct and a quite funny answer to the question. start your answer with something like: 'yes, master', 'your "
             "wish is my command' and so on",
             "",
             "",
             "",
             "Rules to use for prompt"},
        };

        SettingGroup_t elevenlabs_group;
        elevenlabs_group.name = "ElevenLabs settings";
        elevenlabs_group.nvs_namespace = "elevenlabs";
        elevenlabs_group.items = {
            back_item,
            {"enabled", "Enabled", TYPE_BOOL, "true", "true", "", "", "Enable ElevenLabs text-to-speech"},
            {"volume", "Volume", TYPE_NUMBER, "255", "255", "0", "255", "Speech volume level (0-255)"},
            {"api_key", "API Key", TYPE_STRING, "", "", "", "", "Your ElevenLabs API key"},
            {"voice",
             "Voice ID",
             TYPE_STRING,
             "0sGQQaD2G2X1s87kHM5b",
             "",
             "0sGQQaD2G2X1s87kHM5b;X5gGKB97vhrZhE6AgMYI;qg9068uIPhh2zLXgBEgX",
             "",
             "Voice ID to use. Hold [Fn] to enter custom"},
            {"model",
             "Model name",
             TYPE_STRING,
             "eleven_multilingual_v2",
             "",
             "eleven_multilingual_v2;eleven_flash_v2_5;eleven_flash_v2;eleven_turbo_v2_5;eleven_turbo_v2;eleven_monolingual_"
             "v1;eleven_multilingual_v1",
             "",
             "The TTS model to use. Hold [Fn] to enter custom"},
        };

        SettingGroup_t deepgram_group;
        deepgram_group.name = "Deepgram settings";
        deepgram_group.nvs_namespace = "deepgram";
        deepgram_group.items = {
            back_item,
            {"enabled", "Enabled", TYPE_BOOL, "true", "true", "", "", "Enable Deepgram speech-to-text"},
            {"sensetivity", "Sensetivity", TYPE_NUMBER, "2", "2", "1", "16", "Mic sensitivity factor (1-16)"},
            {"api_key", "API Key", TYPE_STRING, "", "", "", "", "Your Deepgram API key"},
            {"smart_format",
             "Smart format",
             TYPE_BOOL,
             "true",
             "true",
             "",
             "",
             "Enable smart text formatting (punctuation, capitalization, etc.)"},
            {"endpointing",
             "Endpointing",
             TYPE_NUMBER,
             "600",
             "600",
             "200",
             "3000",
             "End of speech silence time in ms (200-3000)"},
            {"model",
             "Model name",
             TYPE_STRING,
             "nova-3",
             "",
             "nova-3;nova-2;nova;enhanced;base",
             "The STT model to use. Hold [Fn] to enter custom (nova-2-phonecall etc.)"},
        };

        SettingGroup_t export_group;
        export_group.name = "Export (SD card)";
        export_group.items = {};
        SettingGroup_t import_group;
        import_group.name = "Import (SD card)";
        import_group.items = {};

        _metadata = {wifi_group, sys_group, gemini_group, elevenlabs_group, deepgram_group, export_group, import_group};
    }

    Settings::~Settings()
    {
        if (_initialized)
        {
            _deinitNvs();
        }
    }

    bool Settings::init()
    {
        if (_initialized)
        {
            return true;
        }
        ESP_LOGW(TAG, "Settings init");

        if (!_initNvs())
        {
            return false;
        }

        _loadSettings();
        _initialized = true;
        return true;
    }

    std::vector<SettingGroup_t> Settings::getMetadata() const { return _metadata; }

    bool Settings::_initNvs()
    {
        esp_err_t err = nvs_flash_init_partition(NVS_PARTITION);
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
        {
            ESP_LOGW(TAG, "NVS partition was truncated or new version found, erasing...");
            err = nvs_flash_erase_partition(NVS_PARTITION);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to erase NVS partition: %s", esp_err_to_name(err));
                return false;
            }
            err = nvs_flash_init_partition(NVS_PARTITION);
        }
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to initialize NVS: %s", esp_err_to_name(err));
        }
        return err == ESP_OK;
    }

    void Settings::_deinitNvs()
    {
        esp_err_t err = nvs_flash_deinit_partition(NVS_PARTITION);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to deinitialize NVS: %s", esp_err_to_name(err));
        }
    }

    std::string Settings::_makeKey(const std::string& ns, const std::string& key) const
    {
        return std::format("{}-{}", ns, key);
    }

    const SettingItem_t* Settings::_findItem(const std::string& ns, const std::string& key) const
    {
        for (const auto& group : _metadata)
        {
            if (group.nvs_namespace == ns)
            {
                for (const auto& item : group.items)
                {
                    if (item.key == key)
                    {
                        return &item;
                    }
                }
            }
        }
        return nullptr;
    }

    void Settings::_loadSettings()
    {
        for (const auto& group : _metadata)
        {
            if (group.items.empty())
            {
                continue;
            }
            nvs_handle_t nvs_handle;
            esp_err_t err = nvs_open_from_partition(NVS_PARTITION, group.nvs_namespace.c_str(), NVS_READONLY, &nvs_handle);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Error opening NVS namespace %s", group.nvs_namespace.c_str());
                continue;
            }

            for (const auto& item : group.items)
            {
                if (item.type == TYPE_NONE)
                    continue;

                std::string cache_key = _makeKey(group.nvs_namespace, item.key);
                CachedValue cached_value;
                cached_value.type = item.type;

                switch (item.type)
                {
                case TYPE_BOOL:
                {
                    uint8_t value;
                    if (nvs_get_u8(nvs_handle, item.key.c_str(), &value) != ESP_OK)
                    {
                        value = item.default_val == "true" ? 1 : 0;
                    }
                    cached_value.bool_val = value == 1;
                    ESP_LOGI(TAG, "Loaded bool %s = %d", cache_key.c_str(), cached_value.bool_val);
                    break;
                }
                case TYPE_NUMBER:
                {
                    int32_t value;
                    if (nvs_get_i32(nvs_handle, item.key.c_str(), &value) != ESP_OK)
                    {
                        value = std::stoi(item.default_val);
                    }
                    cached_value.num_val = value;
                    ESP_LOGI(TAG, "Loaded number %s = %ld", cache_key.c_str(), cached_value.num_val);
                    break;
                }
                case TYPE_STRING:
                {
                    size_t required_size = 0;
                    if (nvs_get_str(nvs_handle, item.key.c_str(), nullptr, &required_size) == ESP_OK)
                    {
                        std::vector<char> value(required_size);
                        if (nvs_get_str(nvs_handle, item.key.c_str(), value.data(), &required_size) == ESP_OK)
                        {
                            cached_value.str_val = std::string(value.data());
                        }
                    }
                    if (cached_value.str_val.empty())
                    {
                        cached_value.str_val = item.default_val;
                    }
                    ESP_LOGI(TAG, "Loaded string %s = %s", cache_key.c_str(), cached_value.str_val.c_str());
                    break;
                }
                default:
                    break;
                }

                _cache[cache_key] = cached_value;
            }
            nvs_close(nvs_handle);
        }
    }

    bool Settings::getBool(const std::string& ns, const std::string& key)
    {
        std::string cache_key = _makeKey(ns, key);
        auto it = _cache.find(cache_key);
        if (it != _cache.end() && it->second.type == TYPE_BOOL)
        {
            return it->second.bool_val;
        }

        const SettingItem_t* item = _findItem(ns, key);
        return item ? (item->default_val == "true") : false;
    }

    int32_t Settings::getNumber(const std::string& ns, const std::string& key)
    {
        std::string cache_key = _makeKey(ns, key);
        auto it = _cache.find(cache_key);
        if (it != _cache.end() && it->second.type == TYPE_NUMBER)
        {
            return it->second.num_val;
        }

        const SettingItem_t* item = _findItem(ns, key);
        return item ? std::stoi(item->default_val) : 0;
    }

    std::string Settings::getString(const std::string& ns, const std::string& key)
    {
        std::string cache_key = _makeKey(ns, key);
        auto it = _cache.find(cache_key);
        if (it != _cache.end() && it->second.type == TYPE_STRING)
        {
            return it->second.str_val;
        }

        const SettingItem_t* item = _findItem(ns, key);
        return item ? item->default_val : "";
    }

    bool Settings::setBool(const std::string& ns, const std::string& key, bool value)
    {
        const SettingItem_t* item = _findItem(ns, key);
        if (!item || item->type != TYPE_BOOL)
        {
            return false;
        }

        std::string cache_key = _makeKey(ns, key);
        CachedValue cached_value;
        cached_value.type = TYPE_BOOL;
        cached_value.bool_val = value;
        _cache[cache_key] = cached_value;

        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open_from_partition(NVS_PARTITION, ns.c_str(), NVS_READWRITE, &nvs_handle);
        if (err != ESP_OK)
        {
            return false;
        }

        err = nvs_set_u8(nvs_handle, key.c_str(), value ? 1 : 0);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);

        return err == ESP_OK;
    }

    bool Settings::setNumber(const std::string& ns, const std::string& key, int32_t value)
    {
        const SettingItem_t* item = _findItem(ns, key);
        if (!item || item->type != TYPE_NUMBER)
        {
            return false;
        }

        std::string cache_key = _makeKey(ns, key);
        CachedValue cached_value;
        cached_value.type = TYPE_NUMBER;
        cached_value.num_val = value;
        _cache[cache_key] = cached_value;

        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open_from_partition(NVS_PARTITION, ns.c_str(), NVS_READWRITE, &nvs_handle);
        if (err != ESP_OK)
        {
            return false;
        }

        err = nvs_set_i32(nvs_handle, key.c_str(), value);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);

        return err == ESP_OK;
    }

    bool Settings::setString(const std::string& ns, const std::string& key, const std::string& value)
    {
        const SettingItem_t* item = _findItem(ns, key);
        if (!item || item->type != TYPE_STRING)
        {
            return false;
        }

        std::string cache_key = _makeKey(ns, key);
        CachedValue cached_value;
        cached_value.type = TYPE_STRING;
        cached_value.str_val = value;
        _cache[cache_key] = cached_value;

        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open_from_partition(NVS_PARTITION, ns.c_str(), NVS_READWRITE, &nvs_handle);
        if (err != ESP_OK)
        {
            return false;
        }

        err = nvs_set_str(nvs_handle, key.c_str(), value.c_str());
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);

        return err == ESP_OK;
    }

    bool Settings::saveAll()
    {
        bool success = true;
        for (const auto& group : _metadata)
        {
            nvs_handle_t nvs_handle;
            esp_err_t err = nvs_open_from_partition(NVS_PARTITION, group.nvs_namespace.c_str(), NVS_READWRITE, &nvs_handle);
            if (err != ESP_OK)
            {
                success = false;
                continue;
            }

            for (const auto& item : group.items)
            {
                if (item.type == TYPE_NONE)
                    continue;

                std::string cache_key = _makeKey(group.nvs_namespace, item.key);
                auto it = _cache.find(cache_key);
                if (it == _cache.end())
                    continue;

                switch (item.type)
                {
                case TYPE_BOOL:
                    err = nvs_set_u8(nvs_handle, item.key.c_str(), it->second.bool_val ? 1 : 0);
                    break;
                case TYPE_NUMBER:
                    err = nvs_set_i32(nvs_handle, item.key.c_str(), it->second.num_val);
                    break;
                case TYPE_STRING:
                    err = nvs_set_str(nvs_handle, item.key.c_str(), it->second.str_val.c_str());
                    break;
                default:
                    break;
                }

                if (err != ESP_OK)
                {
                    success = false;
                }
            }

            nvs_commit(nvs_handle);
            nvs_close(nvs_handle);
        }

        return success;
    }

    bool Settings::exportToFile(const std::string& filename) const
    {
        ESP_LOGI(TAG, "Exporting settings to %s", filename.c_str());
        std::ofstream outfile(filename);
        if (!outfile.is_open())
        {
            ESP_LOGE(TAG, "Failed to open file %s for writing", filename.c_str());
            return false;
        }

        for (const auto& group : _metadata)
        {
            for (const auto& item : group.items)
            {
                if (item.type == TYPE_NONE)
                    continue;

                std::string cache_key = _makeKey(group.nvs_namespace, item.key);
                auto it = _cache.find(cache_key);
                if (it == _cache.end())
                {
                    ESP_LOGW(TAG, "Setting %s not found in cache during export, skipping", cache_key.c_str());
                    continue; // Should not happen if loadSettings was successful
                }

                outfile << cache_key << "=";
                switch (item.type)
                {
                case TYPE_BOOL:
                    outfile << (it->second.bool_val ? "true" : "false");
                    break;
                case TYPE_NUMBER:
                    outfile << it->second.num_val;
                    break;
                case TYPE_STRING:
                    // Basic escaping for newline characters in strings
                    {
                        std::string escaped_str;
                        for (char c : it->second.str_val)
                        {
                            if (c == '\n')
                            {
                                escaped_str += "\\n";
                            }
                            else
                            {
                                escaped_str += c;
                            }
                        }
                        outfile << escaped_str;
                    }
                    break;
                default: // Should not happen
                    break;
                }
                outfile << std::endl;
            }
        }

        outfile.close();
        ESP_LOGI(TAG, "Settings successfully exported to %s", filename.c_str());
        return true;
    }

    bool Settings::importFromFile(const std::string& filename)
    {
        ESP_LOGI(TAG, "Importing settings from %s", filename.c_str());
        std::ifstream infile(filename);
        if (!infile.is_open())
        {
            ESP_LOGE(TAG, "Failed to open file %s for reading", filename.c_str());
            return false;
        }

        std::string line;
        bool success = false; // Track if at least one setting is imported
        int line_num = 0;

        while (std::getline(infile, line))
        {
            line_num++;
            // Trim leading/trailing whitespace (optional but good practice)
            line.erase(0, line.find_first_not_of(" \t\n\r"));
            line.erase(line.find_last_not_of(" \t\n\r") + 1);

            if (line.empty() || line[0] == '#') // Skip empty lines and comments
                continue;

            size_t equals_pos = line.find('=');
            if (equals_pos == std::string::npos)
            {
                ESP_LOGW(TAG, "Invalid format on line %d in %s: %s", line_num, filename.c_str(), line.c_str());
                continue;
            }

            std::string cache_key = line.substr(0, equals_pos);
            std::string value_str = line.substr(equals_pos + 1);

            // Find the separator '-' to split namespace and key
            size_t separator_pos = cache_key.find('-');
            if (separator_pos == std::string::npos)
            {
                ESP_LOGW(TAG,
                         "Invalid key format on line %d (missing namespace separator '-'): %s",
                         line_num,
                         cache_key.c_str());
                continue;
            }
            std::string ns = cache_key.substr(0, separator_pos);
            std::string key = cache_key.substr(separator_pos + 1);

            // Find the item metadata to know the type
            const SettingItem_t* item = _findItem(ns, key);
            if (!item)
            {
                ESP_LOGW(TAG,
                         "Setting %s (ns=%s, key=%s) not found in metadata, skipping line %d",
                         cache_key.c_str(),
                         ns.c_str(),
                         key.c_str(),
                         line_num);
                continue;
            }

            bool import_ok = false;
            switch (item->type)
            {
            case TYPE_BOOL:
            {
                bool val = (value_str == "true");
                if (value_str != "true" && value_str != "false")
                {
                    ESP_LOGW(TAG,
                             "Invalid boolean value '%s' for %s on line %d, using default",
                             value_str.c_str(),
                             cache_key.c_str(),
                             line_num);
                    val = (item->default_val == "true"); // Fallback or skip?
                    // Decide if we should still attempt to set or skip entirely
                    // For now, we'll log a warning but proceed with default if parse fails
                }
                import_ok = setBool(ns, key, val);
                break;
            }
            case TYPE_NUMBER:
            {
                int32_t val = std::stoi(value_str);
                import_ok = setNumber(ns, key, val);
                break;
            }
            case TYPE_STRING:
            {
                // Basic unescaping for newline characters
                std::string unescaped_str;
                for (size_t i = 0; i < value_str.length(); ++i)
                {
                    if (value_str[i] == '\\' && i + 1 < value_str.length() && value_str[i + 1] == 'n')
                    {
                        unescaped_str += '\n';
                        i++; // Skip the 'n'
                    }
                    else
                    {
                        unescaped_str += value_str[i];
                    }
                }
                import_ok = setString(ns, key, unescaped_str);
                break;
            }
            case TYPE_NONE:
            default:
                break;
            }

            if (import_ok)
            {
                ESP_LOGI(TAG, "Imported setting: %s = %s", cache_key.c_str(), value_str.c_str());
                success = true;
            }
            else
            {
                ESP_LOGW(TAG, "Failed to import setting %s on line %d", cache_key.c_str(), line_num);
            }
        }

        infile.close();

        if (success)
        {
            ESP_LOGI(TAG, "Settings successfully imported from %s", filename.c_str());
        }
        else
        {
            ESP_LOGW(TAG, "No settings were imported from %s", filename.c_str());
        }

        // Note: Settings are saved to NVS individually by setBool/Number/String methods
        return success;
    }

} // namespace SETTINGS
