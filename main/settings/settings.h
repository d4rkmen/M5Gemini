/**
 * @file settings.h
 * @brief Settings management system with NVS caching
 */

#pragma once

#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include "nvs_flash.h"
#include "esp_log.h"

namespace HAL
{
    class Hal;
}

namespace SETTINGS
{

    enum SettingType
    {
        TYPE_NONE,
        TYPE_BOOL,
        TYPE_NUMBER,
        TYPE_STRING,
        TYPE_CALLBACK
    };

    struct SettingItem_t
    {
        std::string key;
        std::string label;
        SettingType type;
        std::string default_val;
        std::string value;
        std::string min_val; // For TYPE_NUMBER
        std::string max_val; // For TYPE_NUMBER
        std::string hint;
        std::function<void(SettingItem_t& item)> callback;
        // TYPE_NUMBER shown and edited as 0-100%, stored in its own min_val..max_val range
        bool percent = false;
    };

    inline int number_to_percent(const SettingItem_t& item, int number)
    {
        const int min_value = item.min_val.empty() ? 0 : std::stoi(item.min_val);
        const int max_value = item.max_val.empty() ? 100 : std::stoi(item.max_val);
        if (max_value <= min_value || number <= min_value)
            return 0;
        if (number >= max_value)
            return 100;
        return ((number - min_value) * 100 + (max_value - min_value) / 2) / (max_value - min_value);
    }

    inline int percent_to_number(const SettingItem_t& item, int percent)
    {
        const int min_value = item.min_val.empty() ? 0 : std::stoi(item.min_val);
        const int max_value = item.max_val.empty() ? 100 : std::stoi(item.max_val);
        if (percent <= 0)
            return min_value;
        if (percent >= 100)
            return max_value;
        return min_value + ((max_value - min_value) * percent + 50) / 100;
    }

    struct SettingGroup_t
    {
        std::string name;
        std::string nvs_namespace;
        std::vector<SettingItem_t> items;
        std::function<void(SettingGroup_t& group)> callback;
    };

    struct WifiNetwork_t
    {
        std::string ssid;
        std::string pass;
    };

    class Settings
    {
    public:
        static constexpr size_t MAX_WIFI_NETWORKS = 8;

        Settings();
        ~Settings();

        /**
         * @brief Set HAL pointer and initialize callbacks that require HAL access
         * @param hal Pointer to HAL instance
         */
        void setHal(HAL::Hal* hal);

        /**
         * @brief Initialize settings system and load values from NVS
         * @return true if successful
         */
        bool init();

        /**
         * @brief Get all setting groups metadata
         * @return Vector of setting groups
         */
        std::vector<SettingGroup_t> getMetadata() const;

        /**
         * @brief Get boolean setting value
         * @param ns Namespace
         * @param key Setting key
         * @return Boolean value
         */
        bool getBool(const std::string& ns, const std::string& key);

        /**
         * @brief Get number setting value
         * @param ns Namespace
         * @param key Setting key
         * @return Integer value
         */
        int32_t getNumber(const std::string& ns, const std::string& key);

        /**
         * @brief Get string setting value
         * @param ns Namespace
         * @param key Setting key
         * @return String value
         */
        std::string getString(const std::string& ns, const std::string& key);

        /**
         * @brief Set boolean setting value
         * @param ns Namespace
         * @param key Setting key
         * @param value Boolean value
         * @return true if successful
         */
        bool setBool(const std::string& ns, const std::string& key, bool value);

        /**
         * @brief Set number setting value
         * @param ns Namespace
         * @param key Setting key
         * @param value Integer value
         * @return true if successful
         */
        bool setNumber(const std::string& ns, const std::string& key, int32_t value);

        /**
         * @brief Set string setting value
         * @param ns Namespace
         * @param key Setting key
         * @param value String value
         * @return true if successful
         */
        bool setString(const std::string& ns, const std::string& key, const std::string& value);

        /**
         * @brief Get the saved WiFi networks
         * @details The wifi/ssid + wifi/pass pair is always part of the list,
         *          so a network entered through the settings menu is included
         * @return Saved networks, at most MAX_WIFI_NETWORKS
         */
        std::vector<WifiNetwork_t> getWifiNetworks();

        /**
         * @brief Replace the saved WiFi networks
         * @param networks Networks to store, truncated to MAX_WIFI_NETWORKS
         * @return true if successful
         */
        bool setWifiNetworks(const std::vector<WifiNetwork_t>& networks);

        /**
         * @brief Save all modified settings to NVS
         * @return true if successful
         */
        bool saveAll();

        /**
         * @brief Export all settings to a file
         * @param filename The name of the file to export to
         * @return true if successful
         */
        bool exportToFile(const std::string& filename) const;

        /**
         * @brief Import settings from a file
         * @param filename The name of the file to import from
         * @return true if successful
         */
        bool importFromFile(const std::string& filename);

    private:
        static const char* const NVS_PARTITIONS[];
        const char* _active_partition = nullptr;

        // Cache storage
        struct CachedValue
        {
            SettingType type;
            union
            {
                bool bool_val;
                int32_t num_val;
            };
            std::string str_val;
        };

        HAL::Hal* _hal = nullptr;
        std::unordered_map<std::string, CachedValue> _cache;
        std::vector<SettingGroup_t> _metadata;
        bool _initialized = false;

        bool _initNvs();
        void _deinitNvs();
        void _loadSettings();
        std::string _makeKey(const std::string& ns, const std::string& key) const;
        const SettingItem_t* _findItem(const std::string& ns, const std::string& key) const;
    };

} // namespace SETTINGS