/**
 * @file wifi.cpp
 * @brief WiFi module implementation for M5Cardputer
 * @version 0.1
 * @date 2023-07-01
 */

#include "hal_config.h"
#if HAL_USE_WIFI
#include <algorithm>
#include "wifi.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/dns.h"
#include "lwip/ip4_addr.h"

static const char* TAG = "WIFI";

namespace HAL
{

    // Static instance pointer for event handler
    static WiFi* s_wifi_instance = nullptr;

    WiFi::WiFi(SETTINGS::Settings* settings)
        : _settings(settings), _status(WIFI_STATUS_IDLE), _initialized(false), _rssi(0), _last_status_check(0)
    {
        s_wifi_instance = this;
    }

    WiFi::~WiFi()
    {
        deinit();
        s_wifi_instance = nullptr;
    }

    void WiFi::_wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
    {
        if (s_wifi_instance == nullptr)
            return;

        if (event_base == WIFI_EVENT)
        {
            switch (event_id)
            {
            case WIFI_EVENT_STA_START:
                ESP_LOGD(TAG, "WiFi station started, connecting");
                s_wifi_instance->_retry_count = 0;
                s_wifi_instance->_begin_connect();
                break;

            case WIFI_EVENT_SCAN_DONE:
                s_wifi_instance->_handle_scan_done();
                break;

            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "WiFi connected");
                s_wifi_instance->_retry_count = 0;
                s_wifi_instance->_status = WIFI_STATUS_CONNECTED_WEAK;
                // Status will be updated based on RSSI in update()
                break;

            case WIFI_EVENT_STA_DISCONNECTED:
                ESP_LOGI(TAG, "WiFi disconnected");
                s_wifi_instance->_status = WIFI_STATUS_DISCONNECTED;
                if (s_wifi_instance->_status_callback)
                {
                    s_wifi_instance->_status_callback(s_wifi_instance->_status);
                }
                // Try to reconnect if enabled
                if (s_wifi_instance->_settings->getBool("wifi", "enabled"))
                {
                    if (s_wifi_instance->_networks.size() > 1 && ++s_wifi_instance->_retry_count >= CONNECT_RETRIES)
                    {
                        // this network keeps failing, look for another saved one
                        ESP_LOGI(TAG, "WiFi looking for another saved network...");
                        s_wifi_instance->_retry_count = 0;
                        s_wifi_instance->_begin_connect();
                    }
                    else
                    {
                        ESP_LOGI(TAG, "WiFi reconnecting...");
                        esp_wifi_connect();
                        s_wifi_instance->_status = WIFI_STATUS_CONNECTING;
                    }
                }
                break;
            }
        }
        else if (event_base == IP_EVENT)
        {
            if (event_id == IP_EVENT_STA_GOT_IP)
            {
                ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
                ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
                // set IP, mask and gateway to settings
                s_wifi_instance->_settings->setString("wifi", "ip", ip4addr_ntoa((ip4_addr_t*)&event->ip_info.ip));
                s_wifi_instance->_settings->setString("wifi", "mask", ip4addr_ntoa((ip4_addr_t*)&event->ip_info.netmask));
                s_wifi_instance->_settings->setString("wifi", "gateway", ip4addr_ntoa((ip4_addr_t*)&event->ip_info.gw));
                // show the network we ended up on in the settings menu
                if (s_wifi_instance->_current_network < (int)s_wifi_instance->_networks.size())
                {
                    const auto& network = s_wifi_instance->_networks[s_wifi_instance->_current_network];
                    if (s_wifi_instance->_settings->getString("wifi", "ssid") != network.ssid)
                    {
                        s_wifi_instance->_settings->setString("wifi", "ssid", network.ssid);
                        s_wifi_instance->_settings->setString("wifi", "pass", network.pass);
                    }
                }
                // Update status based on RSSI
                s_wifi_instance->_update_status_from_rssi();
                if (s_wifi_instance->_status_callback)
                {
                    s_wifi_instance->_status_callback(s_wifi_instance->_status);
                }
            }
        }
    }

    bool WiFi::init()
    {
        if (_initialized)
        {
            deinit();
        }
        if (!_settings->getBool("wifi", "enabled"))
        {
            _status = WIFI_STATUS_IDLE;
            ESP_LOGD(TAG, "WiFi is disabled by settings");
            return true;
        }
        _networks = _settings->getWifiNetworks();
        _current_network = 0;
        _retry_count = 0;
        _auto_scan = false;
        if (!_networks.empty())
        {
            _wifi_settings.ssid = _networks[0].ssid;
            _wifi_settings.password = _networks[0].pass;
        }
        else
        {
            _wifi_settings.ssid.clear();
            _wifi_settings.password.clear();
        }
        _wifi_settings.static_ip = _settings->getBool("wifi", "static_ip");
        _wifi_settings.ip = _settings->getString("wifi", "ip");
        _wifi_settings.mask = _settings->getString("wifi", "mask");
        _wifi_settings.gateway = _settings->getString("wifi", "gateway");
        _wifi_settings.dns = _settings->getString("wifi", "dns");

        ESP_LOGI(TAG, "Initializing WiFi with %d saved network(s), first SSID: %s", (int)_networks.size(), _wifi_settings.ssid.c_str());

        // Initialize TCP/IP adapter
        esp_err_t err = esp_netif_init();
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to initialize TCP/IP adapter: %s", esp_err_to_name(err));
            return false;
        }

        // Create default event loop if not already created
        err = esp_event_loop_create_default();
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to create default event loop: %s", esp_err_to_name(err));
            return false;
        }

        // Create default netif instance
        _sta_netif = esp_netif_create_default_wifi_sta();

        // Configure static IP if enabled
        if (_wifi_settings.static_ip)
        {
            ESP_LOGI(TAG, "Configuring static IP: %s", _wifi_settings.ip.c_str());

            esp_netif_dhcpc_stop(_sta_netif);

            esp_netif_ip_info_t ip_info;
            memset(&ip_info, 0, sizeof(esp_netif_ip_info_t));

            ip4addr_aton(_wifi_settings.ip.c_str(), (ip4_addr_t*)&ip_info.ip);
            ip4addr_aton(_wifi_settings.mask.c_str(), (ip4_addr_t*)&ip_info.netmask);
            ip4addr_aton(_wifi_settings.gateway.c_str(), (ip4_addr_t*)&ip_info.gw);

            err = esp_netif_set_ip_info(_sta_netif, &ip_info);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to set static IP: %s", esp_err_to_name(err));
                return false;
            }

            // Set DNS server
            ip_addr_t dns_server;
            ip4addr_aton(_wifi_settings.dns.c_str(), (ip4_addr_t*)&dns_server);
            dns_setserver(0, &dns_server);
        }

        // Initialize WiFi
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&cfg);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to initialize WiFi: %s", esp_err_to_name(err));
            return false;
        }

        // Register event handlers
        err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &_wifi_event_handler, NULL);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to register WiFi event handler: %s", esp_err_to_name(err));
            return false;
        }
        err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &_wifi_event_handler, NULL);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to register IP event handler: %s", esp_err_to_name(err));
            return false;
        }

        // Configure WiFi station
        wifi_config_t wifi_config = {};
        memset(&wifi_config, 0, sizeof(wifi_config_t));

        strncpy((char*)wifi_config.sta.ssid, _wifi_settings.ssid.c_str(), sizeof(wifi_config.sta.ssid) - 1);
        strncpy((char*)wifi_config.sta.password, _wifi_settings.password.c_str(), sizeof(wifi_config.sta.password) - 1);

        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to set WiFi mode: %s", esp_err_to_name(err));
            return false;
        }
        err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to set WiFi config: %s", esp_err_to_name(err));
            return false;
        }

        if (_rescan_timer == nullptr)
        {
            esp_timer_create_args_t timer_args = {};
            timer_args.callback = [](void* arg) { static_cast<WiFi*>(arg)->_begin_connect(); };
            timer_args.arg = this;
            timer_args.name = "wifi_rescan";
            esp_timer_create(&timer_args, &_rescan_timer);
        }

        _initialized = true;
        _status = WIFI_STATUS_DISCONNECTED;

        return true;
    }

    void WiFi::_set_status(wifi_status_t status)
    {
        _status = status;
        if (_status_callback)
        {
            _status_callback(_status);
        }
    }

    void WiFi::_connect_to(int index)
    {
        if (index < 0 || index >= (int)_networks.size())
            return;

        _current_network = index;
        const auto& network = _networks[index];

        wifi_config_t wifi_config = {};
        strncpy((char*)wifi_config.sta.ssid, network.ssid.c_str(), sizeof(wifi_config.sta.ssid) - 1);
        strncpy((char*)wifi_config.sta.password, network.pass.c_str(), sizeof(wifi_config.sta.password) - 1);
        esp_wifi_set_config(WIFI_IF_STA, &wifi_config);

        ESP_LOGI(TAG, "Connecting to %s", network.ssid.c_str());
        _status = WIFI_STATUS_CONNECTING;
        esp_wifi_connect();
    }

    void WiFi::_begin_connect()
    {
        if (!_initialized || _ap_active || _networks.empty() || is_connected())
            return;

        if (_networks.size() == 1)
        {
            // nothing to choose from, and a direct connect also reaches a hidden SSID
            _connect_to(0);
            return;
        }

        _auto_scan = true;
        _status = WIFI_STATUS_CONNECTING;
        if (esp_wifi_scan_start(nullptr, false) != ESP_OK)
        {
            _auto_scan = false;
            _connect_to(_current_network);
        }
    }

    void WiFi::_handle_scan_done()
    {
        // scan() blocks and collects its own results
        if (!_auto_scan)
            return;
        _auto_scan = false;

        uint16_t ap_count = 20;
        wifi_ap_record_t* ap_records = (wifi_ap_record_t*)malloc(sizeof(wifi_ap_record_t) * ap_count);
        if (!ap_records || esp_wifi_scan_get_ap_records(&ap_count, ap_records) != ESP_OK)
        {
            free(ap_records);
            esp_wifi_clear_ap_list();
            _connect_to(_current_network);
            return;
        }

        int best = -1;
        int best_rssi = -128;
        for (int i = 0; i < ap_count; i++)
        {
            for (int n = 0; n < (int)_networks.size(); n++)
            {
                if (_networks[n].ssid == (const char*)ap_records[i].ssid && ap_records[i].rssi > best_rssi)
                {
                    best = n;
                    best_rssi = ap_records[i].rssi;
                }
            }
        }
        free(ap_records);

        if (best >= 0)
        {
            _connect_to(best);
            return;
        }

        ESP_LOGI(TAG, "No saved network in range, retrying later");
        _set_status(WIFI_STATUS_DISCONNECTED);
        if (_rescan_timer)
        {
            esp_timer_stop(_rescan_timer);
            esp_timer_start_once(_rescan_timer, RESCAN_DELAY_US);
        }
    }

    void WiFi::deinit()
    {
        if (!_initialized)
            return;

        if (_rescan_timer)
        {
            esp_timer_stop(_rescan_timer);
            esp_timer_delete(_rescan_timer);
            _rescan_timer = nullptr;
        }
        _auto_scan = false;

        disconnect();

        esp_err_t err = esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &_wifi_event_handler);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to unregister IP event handler: %s", esp_err_to_name(err));
        }
        err = esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &_wifi_event_handler);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to unregister WiFi event handler: %s", esp_err_to_name(err));
        }
        esp_netif_destroy_default_wifi(_sta_netif);
        _sta_netif = nullptr;
        err = esp_event_loop_delete_default();
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to delete default event loop: %s", esp_err_to_name(err));
        }
        err = esp_wifi_deinit();
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to deinitialize WiFi: %s", esp_err_to_name(err));
        }

        _initialized = false;
        _status = WIFI_STATUS_IDLE;

        if (_status_callback)
        {
            _status_callback(_status);
        }
    }

    bool WiFi::connect()
    {
        if (!_initialized)
        {
            ESP_LOGE(TAG, "WiFi not initialized");
            return false;
        }

        if (_status == WIFI_STATUS_CONNECTING || is_connected())
        {
            ESP_LOGI(TAG, "WiFi already connecting or connected");
            return true;
        }

        esp_err_t err = esp_wifi_start();
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to start WiFi: %s", esp_err_to_name(err));
            return false;
        }

        _status = WIFI_STATUS_CONNECTING;

        if (_status_callback)
        {
            _status_callback(_status);
        }

        return true;
    }

    void WiFi::disconnect()
    {
        if (!_initialized)
            return;

        ESP_LOGD(TAG, "Disconnecting WiFi");
        esp_wifi_disconnect();
        esp_wifi_stop();

        _status = WIFI_STATUS_DISCONNECTED;
        _rssi = 0;

        if (_status_callback)
        {
            _status_callback(_status);
        }
    }

    wifi_status_t WiFi::get_status() const { return _status; }

    int8_t WiFi::get_rssi() const { return _rssi; }

    bool WiFi::is_connected() const
    {
        return _status == WIFI_STATUS_CONNECTED_WEAK || _status == WIFI_STATUS_CONNECTED_GOOD ||
               _status == WIFI_STATUS_CONNECTED_STRONG;
    }

    void WiFi::set_status_callback(std::function<void(wifi_status_t)> callback) { _status_callback = callback; }

    void WiFi::update()
    {
        if (!_initialized || !is_connected())
            return;

        // Check RSSI periodically (every 5 seconds)
        uint32_t now = esp_log_timestamp();
        if (now - _last_status_check > 5000)
        {
            _last_status_check = now;

            wifi_ap_record_t ap_info;
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK)
            {
                _rssi = ap_info.rssi;
                ESP_LOGD(TAG, "RSSI: %d", _rssi);
                _update_status_from_rssi();
            }
            else
            {
                ESP_LOGE(TAG, "Failed to get AP info");
            }
        }
    }

    void WiFi::_update_status_from_rssi()
    {
        wifi_status_t old_status = _status;

        // Update status based on RSSI
        if (_rssi == 0)
        {
            _status = WIFI_STATUS_CONNECTED_WEAK;
        }
        else if (_rssi < -80)
        {
            _status = WIFI_STATUS_CONNECTED_WEAK;
        }
        else if (_rssi < -67)
        {
            _status = WIFI_STATUS_CONNECTED_GOOD;
        }
        else
        {
            _status = WIFI_STATUS_CONNECTED_STRONG;
        }

        // Notify if status changed
        if (old_status != _status && _status_callback)
        {
            _status_callback(_status);
        }
    }

    std::vector<std::string> WiFi::scan()
    {
        std::vector<std::string> networks;
        bool temp_init = false;
        esp_netif_t* temp_sta_netif = nullptr;
        uint16_t ap_count = 0;
        uint16_t num_aps = 0;
        wifi_ap_record_t* ap_records = nullptr;

        // If WiFi is not initialized, perform temporary initialization
        // (in access point mode the driver is already running)
        if (!_initialized && !_ap_active)
        {
            ESP_LOGI(TAG, "Performing temporary WiFi initialization for scanning");

            // Initialize TCP/IP adapter
            esp_err_t err = esp_netif_init();
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to initialize TCP/IP adapter: %s", esp_err_to_name(err));
                return networks;
            }

            // Create default event loop
            err = esp_event_loop_create_default();
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to create default event loop: %s", esp_err_to_name(err));
                return networks;
            }

            // Create default netif instance
            temp_sta_netif = esp_netif_create_default_wifi_sta();
            if (!temp_sta_netif)
            {
                ESP_LOGE(TAG, "Failed to create default WiFi STA netif");
                return networks;
            }

            // Initialize WiFi with default config
            wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
            err = esp_wifi_init(&cfg);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to initialize WiFi: %s", esp_err_to_name(err));
                esp_netif_destroy_default_wifi(temp_sta_netif);
                return networks;
            }

            // Set WiFi mode and start it
            err = esp_wifi_set_mode(WIFI_MODE_STA);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to set WiFi mode: %s", esp_err_to_name(err));
                esp_wifi_deinit();
                esp_netif_destroy_default_wifi(temp_sta_netif);
                return networks;
            }

            err = esp_wifi_start();
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to start WiFi: %s", esp_err_to_name(err));
                esp_wifi_deinit();
                esp_netif_destroy_default_wifi(temp_sta_netif);
                return networks;
            }

            temp_init = true;
        }

        // Configure scan parameters
        wifi_scan_config_t scan_config = {.ssid = NULL,
                                          .bssid = NULL,
                                          .channel = 0,
                                          .show_hidden = false,
                                          .scan_type = WIFI_SCAN_TYPE_ACTIVE,
                                          .scan_time = {.active = {.min = 0, .max = 0}}};

        // Start scan
        esp_err_t err = esp_wifi_scan_start(&scan_config, true); // true = block until scan completes
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to start WiFi scan: %s", esp_err_to_name(err));
            goto cleanup;
        }

        // Get scan results
        esp_wifi_scan_get_ap_num(&ap_count);

        if (ap_count == 0)
        {
            ESP_LOGI(TAG, "No networks found");
            goto cleanup;
        }

        // Cap the number of networks to display
        if (ap_count > 20)
            ap_count = 20;

        // Allocate memory for AP records
        ap_records = (wifi_ap_record_t*)malloc(sizeof(wifi_ap_record_t) * ap_count);
        if (!ap_records)
        {
            ESP_LOGE(TAG, "Failed to allocate memory for AP records");
            goto cleanup;
        }

        // Get AP records
        num_aps = ap_count;
        err = esp_wifi_scan_get_ap_records(&num_aps, ap_records);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to get AP records: %s", esp_err_to_name(err));
            free(ap_records);
            goto cleanup;
        }

        // Sort APs by signal strength (RSSI)
        for (int i = 0; i < num_aps - 1; i++)
        {
            for (int j = i + 1; j < num_aps; j++)
            {
                if (ap_records[j].rssi > ap_records[i].rssi)
                {
                    wifi_ap_record_t temp = ap_records[i];
                    ap_records[i] = ap_records[j];
                    ap_records[j] = temp;
                }
            }
        }

        // Create list of networks with signal strength indicators
        for (int i = 0; i < num_aps; i++)
        {
            std::string ssid((char*)ap_records[i].ssid);
            // add only unique networks
            if (!ssid.empty() && std::find(networks.begin(), networks.end(), ssid) == networks.end())
            {
                networks.push_back(ssid);
            }
        }

        free(ap_records);
    cleanup:
        // Clean up temporary initialization if needed
        if (temp_init)
        {
            esp_wifi_stop();
            esp_wifi_deinit();
            esp_netif_destroy_default_wifi(temp_sta_netif);
            esp_event_loop_delete_default();
        }

        return networks;
    }

    bool WiFi::start_ap(const std::string& ssid, const std::string& password)
    {
#ifndef CONFIG_ESP_WIFI_SOFTAP_SUPPORT
        ESP_LOGE(TAG, "Access point support is disabled (CONFIG_ESP_WIFI_SOFTAP_SUPPORT)");
        return false;
#else
        if (_ap_active)
            return true;
        if (_initialized)
            deinit();

        if (esp_netif_init() != ESP_OK || esp_event_loop_create_default() != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to initialize TCP/IP for access point");
            return false;
        }

        _ap_netif = esp_netif_create_default_wifi_ap();
        _sta_netif = esp_netif_create_default_wifi_sta();

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_err_t err = esp_wifi_init(&cfg);
        if (err == ESP_OK)
        {
            wifi_config_t ap_config = {};
            strncpy((char*)ap_config.ap.ssid, ssid.c_str(), sizeof(ap_config.ap.ssid) - 1);
            strncpy((char*)ap_config.ap.password, password.c_str(), sizeof(ap_config.ap.password) - 1);
            ap_config.ap.ssid_len = strlen((char*)ap_config.ap.ssid);
            ap_config.ap.channel = 1;
            ap_config.ap.max_connection = 2;
            ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

            // APSTA rather than AP: the station side is what lets us scan
            err = esp_wifi_set_mode(WIFI_MODE_APSTA);
            if (err == ESP_OK)
                err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
            if (err == ESP_OK)
                err = esp_wifi_start();
            if (err != ESP_OK)
                esp_wifi_deinit();
        }

        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to start access point: %s", esp_err_to_name(err));
            esp_netif_destroy_default_wifi(_ap_netif);
            esp_netif_destroy_default_wifi(_sta_netif);
            _ap_netif = nullptr;
            _sta_netif = nullptr;
            esp_event_loop_delete_default();
            return false;
        }

        _ap_active = true;
        ESP_LOGI(TAG, "Access point %s started", ssid.c_str());
        return true;
#endif
    }

    void WiFi::stop_ap()
    {
        if (!_ap_active)
            return;

        esp_wifi_stop();
        esp_wifi_deinit();
        esp_netif_destroy_default_wifi(_ap_netif);
        esp_netif_destroy_default_wifi(_sta_netif);
        _ap_netif = nullptr;
        _sta_netif = nullptr;
        esp_event_loop_delete_default();
        _ap_active = false;
        ESP_LOGI(TAG, "Access point stopped");
    }

    std::string WiFi::get_ip() const
    {
        esp_netif_ip_info_t ip_info;
        if (!_initialized || !is_connected() || !_sta_netif || esp_netif_get_ip_info(_sta_netif, &ip_info) != ESP_OK)
            return "";
        char buf[16];
        snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip_info.ip));
        return buf;
    }

} // namespace HAL
#endif