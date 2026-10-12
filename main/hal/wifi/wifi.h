#pragma once
#include "hal_config.h"
#if HAL_USE_WIFI
#include <string>
#include <vector>
#include <functional>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "settings/settings.h"

namespace HAL
{

    /**
     * @brief WiFi status enum
     */
    enum wifi_status_t
    {
        WIFI_STATUS_IDLE = 0,        // Not initialized
        WIFI_STATUS_DISCONNECTED,    // Initialized but not connected
        WIFI_STATUS_CONNECTING,      // Connecting to AP
        WIFI_STATUS_CONNECTED_WEAK,  // Connected with weak signal
        WIFI_STATUS_CONNECTED_GOOD,  // Connected with good signal
        WIFI_STATUS_CONNECTED_STRONG // Connected with strong signal
    };

    /**
     * @brief WiFi configuration structure
     */
    struct wifi_settings_t
    {
        std::string ssid;
        std::string password;
        bool static_ip = false;
        std::string ip;
        std::string mask;
        std::string gateway;
        std::string dns;
    };

    /**
     * @brief WiFi module class
     */
    class WiFi
    {
    public:
        WiFi(SETTINGS::Settings* settings);
        ~WiFi();

        /**
         * @brief Initialize WiFi module
         * @param config WiFi configuration
         * @return true if successful
         */
        bool init();

        /**
         * @brief Deinitialize WiFi module
         */
        void deinit();

        /**
         * @brief Connect to WiFi network
         * @return true if connection started
         */
        bool connect();

        /**
         * @brief Disconnect from WiFi network
         */
        void disconnect();

        /**
         * @brief Get current WiFi status
         * @return wifi_status_t enum value
         */
        wifi_status_t get_status() const;

        /**
         * @brief Get current RSSI (signal strength)
         * @return RSSI value in dBm, or 0 if not connected
         */
        int8_t get_rssi() const;

        /**
         * @brief Check if WiFi is connected
         * @return true if connected
         */
        bool is_connected() const;

        /**
         * @brief Set connection status callback
         * @param callback Function to call when connection status changes
         */
        void set_status_callback(std::function<void(wifi_status_t)> callback);

        /**
         * @brief Update WiFi status (call periodically)
         */
        void update();

        /**
         * @brief Scan for available WiFi networks
         * @return Vector of network SSIDs
         */
        std::vector<std::string> scan();

        /**
         * @brief Start a WPA2 access point (station is kept for scanning only)
         * @details Deinitializes the station connection first. Used by the web setup
         * @return true if the access point is up
         */
        bool start_ap(const std::string& ssid, const std::string& password);

        /**
         * @brief Stop the access point started with start_ap()
         */
        void stop_ap();

        bool is_ap_active() const { return _ap_active; }

        /**
         * @brief Get the station IP address
         * @return Dotted IP string, empty if not connected
         */
        std::string get_ip() const;

    private:
        SETTINGS::Settings* _settings;
        wifi_settings_t _wifi_settings;
        wifi_status_t _status;
        bool _initialized;
        int8_t _rssi;
        uint32_t _last_status_check;
        esp_netif_t* _sta_netif;
        esp_netif_t* _ap_netif = nullptr;
        bool _ap_active = false;
        std::function<void(wifi_status_t)> _status_callback;

        // Saved networks: with more than one, a scan picks the strongest that is in range
        static constexpr int CONNECT_RETRIES = 3;
        static constexpr uint64_t RESCAN_DELAY_US = 10 * 1000 * 1000;
        std::vector<SETTINGS::WifiNetwork_t> _networks;
        int _current_network = 0;
        int _retry_count = 0;
        bool _auto_scan = false;
        esp_timer_handle_t _rescan_timer = nullptr;

        void _begin_connect();
        void _connect_to(int index);
        void _handle_scan_done();
        void _set_status(wifi_status_t status);

        static void _wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
        void _update_status_from_rssi();
    };

} // namespace HAL
#endif