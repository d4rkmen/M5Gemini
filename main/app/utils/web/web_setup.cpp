/**
 * @file web_setup.cpp
 * @brief Browser based settings editor for boards without a keyboard
 */
#include "web_setup.h"
#include "hal_config.h"
#if defined(CONFIG_BOARD_STICKS3) && HAL_USE_WIFI && HAL_USE_KEYBOARD && HAL_USE_DISPLAY
#include "../ui/dialog.h"
#include "../theme/theme_define.h"
#include "common_define.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

static const char* TAG = "WEB_SETUP";

#define MAX_BODY_SIZE (16 * 1024)
#define AP_URL "http://192.168.4.1/"

namespace
{
    using Fields = std::vector<std::pair<std::string, std::string>>;

    struct
    {
        HAL::Hal* hal = nullptr;
        std::vector<std::string> scanned;
        volatile bool saved = false;
    } s_ctx;

    const char PAGE_HEAD[] =
        "<!doctype html><html><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>M5Gemini setup</title><style>"
        "body{font-family:system-ui,sans-serif;margin:0;padding:16px;background:#101418;color:#e6e6e6}"
        "form,.box{max-width:520px;margin:0 auto}"
        "h1{font-size:22px;color:#2d97dd}h2{font-size:17px;margin:26px 0 8px;border-bottom:1px solid #333;padding-bottom:4px}"
        "label{display:block;margin:12px 0 4px}"
        "input,select,textarea{width:100%;box-sizing:border-box;padding:9px;font-size:16px;border:1px solid #444;"
        "border-radius:6px;background:#1c2228;color:#e6e6e6}"
        "textarea{height:130px}input[type=checkbox]{width:auto;margin-right:8px}"
        "small{display:block;color:#8a949e;margin-top:3px}"
        ".n{display:flex;justify-content:space-between;align-items:center;padding:8px 0;border-bottom:1px solid #252b31}"
        ".n label{margin:0}button{width:100%;margin:28px 0;padding:13px;font-size:17px;border:0;border-radius:6px;"
        "background:#2d97dd;color:#fff}"
        "</style></head><body>";

    std::string html_escape(const std::string& in)
    {
        std::string out;
        out.reserve(in.size());
        for (char c : in)
        {
            switch (c)
            {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\'':
                out += "&#39;";
                break;
            default:
                out += c;
            }
        }
        return out;
    }

    int hex_value(char c)
    {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    }

    std::string url_decode(const std::string& in)
    {
        std::string out;
        out.reserve(in.size());
        for (size_t i = 0; i < in.size(); i++)
        {
            if (in[i] == '+')
            {
                out += ' ';
            }
            else if (in[i] == '%' && i + 2 < in.size() && hex_value(in[i + 1]) >= 0 && hex_value(in[i + 2]) >= 0)
            {
                out += (char)(hex_value(in[i + 1]) * 16 + hex_value(in[i + 2]));
                i += 2;
            }
            else
            {
                out += in[i];
            }
        }
        return out;
    }

    Fields parse_form(const std::string& body)
    {
        Fields fields;
        size_t pos = 0;
        while (pos < body.size())
        {
            size_t end = body.find('&', pos);
            if (end == std::string::npos)
                end = body.size();
            std::string pair = body.substr(pos, end - pos);
            pos = end + 1;

            size_t eq = pair.find('=');
            if (eq == std::string::npos)
                fields.push_back({url_decode(pair), ""});
            else
                fields.push_back({url_decode(pair.substr(0, eq)), url_decode(pair.substr(eq + 1))});
        }
        return fields;
    }

    bool form_get(const Fields& fields, const std::string& name, std::string* value = nullptr)
    {
        for (const auto& field : fields)
        {
            if (field.first == name)
            {
                if (value)
                    *value = field.second;
                return true;
            }
        }
        return false;
    }

    std::string trim(const std::string& in)
    {
        size_t first = in.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return "";
        size_t last = in.find_last_not_of(" \t\r\n");
        return in.substr(first, last - first + 1);
    }

    // Settings that are written but never sent back to the browser
    bool is_secret(const SETTINGS::SettingItem_t& item) { return item.key == "api_key" || item.key == "pass"; }

    // WiFi name and password are edited through the saved networks list instead
    bool is_network_item(const SETTINGS::SettingGroup_t& group, const SETTINGS::SettingItem_t& item)
    {
        return group.nvs_namespace == "wifi" && (item.key == "ssid" || item.key == "pass");
    }

    bool is_editable(const SETTINGS::SettingGroup_t& group, const SETTINGS::SettingItem_t& item)
    {
        if (group.nvs_namespace.empty() || is_network_item(group, item))
            return false;
        return item.type == SETTINGS::TYPE_BOOL || item.type == SETTINGS::TYPE_NUMBER || item.type == SETTINGS::TYPE_STRING;
    }

    std::vector<std::string> split_options(const std::string& options)
    {
        std::vector<std::string> result;
        size_t pos = 0;
        while (pos <= options.size())
        {
            size_t end = options.find(';', pos);
            if (end == std::string::npos)
                end = options.size();
            result.push_back(options.substr(pos, end - pos));
            pos = end + 1;
        }
        return result;
    }

    int string_max_length(const SETTINGS::SettingItem_t& item)
    {
        // max_val carries the length limit for strings, as in the on-device editor
        return item.max_val.empty() ? 64 : atoi(item.max_val.c_str());
    }

    void append_networks(std::string& page)
    {
        auto networks = s_ctx.hal->settings()->getWifiNetworks();
        if (networks.empty())
            page += "<small>No networks saved yet.</small>";
        for (size_t i = 0; i < networks.size(); i++)
        {
            page += "<div class=n><b>" + html_escape(networks[i].ssid) + "</b><label><input type=checkbox name=del value=" +
                    std::to_string(i) + ">remove</label></div>";
        }

        page += "<label>Add a network<input name=new_ssid list=scan maxlength=32 placeholder='Network name' "
                "autocapitalize=off autocorrect=off></label><datalist id=scan>";
        for (const auto& ssid : s_ctx.scanned)
            page += "<option value=\"" + html_escape(ssid) + "\">";
        page += "</datalist><label>Password<input name=new_pass type=password maxlength=64></label>";
        page += "<small>Up to " + std::to_string(SETTINGS::Settings::MAX_WIFI_NETWORKS) +
                " networks. The strongest one in range is used. To change a password, add the network again.</small>";
    }

    void append_item(std::string& page, const SETTINGS::SettingGroup_t& group, const SETTINGS::SettingItem_t& item)
    {
        auto* settings = s_ctx.hal->settings();
        const std::string name = group.nvs_namespace + "-" + item.key;
        const std::string label = html_escape(item.label);

        switch (item.type)
        {
        case SETTINGS::TYPE_BOOL:
            page += "<label><input type=checkbox name=" + name + " value=1";
            if (settings->getBool(group.nvs_namespace, item.key))
                page += " checked";
            page += ">" + label + "</label>";
            break;

        case SETTINGS::TYPE_NUMBER:
            if (item.percent)
            {
                page += "<label>" + label + " (%)<input type=number name=" + name + " min=0 max=100 value=" +
                        std::to_string(SETTINGS::number_to_percent(item, settings->getNumber(group.nvs_namespace, item.key))) +
                        "></label>";
                break;
            }
            page += "<label>" + label + "<input type=number name=" + name + " min=" + html_escape(item.min_val) +
                    " max=" + html_escape(item.max_val) +
                    " value=" + std::to_string(settings->getNumber(group.nvs_namespace, item.key)) + "></label>";
            break;

        case SETTINGS::TYPE_STRING:
        {
            const std::string value = settings->getString(group.nvs_namespace, item.key);
            if (is_secret(item))
            {
                page += "<label>" + label + "<input type=password name=" + name + " autocomplete=off placeholder='" +
                        (value.empty() ? "not set" : "saved - leave blank to keep") + "'></label>";
            }
            else if (!item.min_val.empty())
            {
                // min_val carries the list of choices for strings
                auto options = split_options(item.min_val);
                if (std::find(options.begin(), options.end(), value) == options.end())
                    options.insert(options.begin(), value);
                page += "<label>" + label + "<select name=" + name + ">";
                for (const auto& option : options)
                {
                    page += "<option";
                    if (option == value)
                        page += " selected";
                    page += ">" + html_escape(option) + "</option>";
                }
                page += "</select></label>";
            }
            else if (string_max_length(item) > 100)
            {
                page += "<label>" + label + "<textarea name=" + name + " maxlength=" +
                        std::to_string(string_max_length(item)) + ">" + html_escape(value) + "</textarea></label>";
            }
            else
            {
                page += "<label>" + label + "<input name=" + name + " maxlength=" +
                        std::to_string(string_max_length(item)) + " value=\"" + html_escape(value) + "\"></label>";
            }
            break;
        }
        default:
            break;
        }
    }

    esp_err_t handle_index(httpd_req_t* req)
    {
        std::string page;
        page.reserve(8192);
        page += PAGE_HEAD;
        page += "<form method=post action=/save><h1>M5Gemini setup</h1>";

        for (const auto& group : s_ctx.hal->settings()->getMetadata())
        {
            if (group.nvs_namespace.empty())
                continue;
            page += "<h2>" + html_escape(group.name) + "</h2>";
            if (group.nvs_namespace == "wifi")
                append_networks(page);
            for (const auto& item : group.items)
            {
                if (is_editable(group, item))
                    append_item(page, group, item);
            }
        }
        page += "<button>Save</button></form></body></html>";

        httpd_resp_set_type(req, "text/html");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, page.c_str(), page.size());
    }

    void save_networks(const Fields& fields)
    {
        auto* settings = s_ctx.hal->settings();
        auto networks = settings->getWifiNetworks();

        std::vector<SETTINGS::WifiNetwork_t> kept;
        for (size_t i = 0; i < networks.size(); i++)
        {
            bool remove = false;
            for (const auto& field : fields)
                remove |= (field.first == "del" && field.second == std::to_string(i));
            if (!remove)
            {
                kept.push_back(networks[i]);
            }
            else if (settings->getString("wifi", "ssid") == networks[i].ssid)
            {
                // getWifiNetworks() would bring it straight back from here
                settings->setString("wifi", "ssid", "");
                settings->setString("wifi", "pass", "");
            }
        }

        std::string ssid, pass;
        form_get(fields, "new_ssid", &ssid);
        form_get(fields, "new_pass", &pass);
        ssid = trim(ssid).substr(0, 32);
        pass = pass.substr(0, 64);
        if (!ssid.empty())
        {
            auto it = std::find_if(kept.begin(), kept.end(), [&](const auto& network) { return network.ssid == ssid; });
            if (it != kept.end())
            {
                it->pass = pass;
            }
            else if (kept.size() < SETTINGS::Settings::MAX_WIFI_NETWORKS)
            {
                kept.push_back({ssid, pass});
            }
            // keep the single-network settings in step when that is the entry being changed
            if (settings->getString("wifi", "ssid") == ssid)
                settings->setString("wifi", "pass", pass);
            // a network was just entered, so the intent is clearly to use WiFi
            settings->setBool("wifi", "enabled", true);
        }
        settings->setWifiNetworks(kept);
    }

    void save_items(const Fields& fields)
    {
        auto* settings = s_ctx.hal->settings();
        std::string ssid;
        const bool network_added = form_get(fields, "new_ssid", &ssid) && !trim(ssid).empty();

        for (const auto& group : settings->getMetadata())
        {
            for (const auto& item : group.items)
            {
                if (!is_editable(group, item))
                    continue;

                const std::string name = group.nvs_namespace + "-" + item.key;
                std::string value;
                const bool present = form_get(fields, name, &value);

                switch (item.type)
                {
                case SETTINGS::TYPE_BOOL:
                    // an unchecked box is simply missing from the form
                    if (group.nvs_namespace == "wifi" && item.key == "enabled" && network_added)
                        break;
                    settings->setBool(group.nvs_namespace, item.key, present);
                    break;

                case SETTINGS::TYPE_NUMBER:
                {
                    if (!present || trim(value).empty())
                        break;
                    int number = atoi(value.c_str());
                    if (item.percent)
                        number = SETTINGS::percent_to_number(item, number);
                    if (!item.min_val.empty())
                        number = std::max(number, atoi(item.min_val.c_str()));
                    if (!item.max_val.empty())
                        number = std::min(number, atoi(item.max_val.c_str()));
                    settings->setNumber(group.nvs_namespace, item.key, number);
                    if (group.nvs_namespace == "system" && item.key == "brightness")
                        s_ctx.hal->display()->setBrightness(number);
                    if (group.nvs_namespace == "system" && item.key == "volume")
                        s_ctx.hal->setSystemVolume(number);
                    break;
                }

                case SETTINGS::TYPE_STRING:
                {
                    if (!present)
                        break;
                    std::string text;
                    for (char c : value)
                    {
                        if (c != '\r')
                            text += c;
                    }
                    text = trim(text);
                    if (is_secret(item) && text.empty())
                        break;
                    if (item.min_val.empty())
                        text = text.substr(0, string_max_length(item));
                    settings->setString(group.nvs_namespace, item.key, text);
                    break;
                }
                default:
                    break;
                }
            }
        }
    }

    esp_err_t handle_save(httpd_req_t* req)
    {
        if (req->content_len > MAX_BODY_SIZE)
        {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Form too large");
            return ESP_FAIL;
        }

        std::string body(req->content_len, '\0');
        size_t received = 0;
        while (received < body.size())
        {
            int ret = httpd_req_recv(req, &body[received], body.size() - received);
            if (ret == HTTPD_SOCK_ERR_TIMEOUT)
                continue;
            if (ret <= 0)
                return ESP_FAIL;
            received += ret;
        }

        const Fields fields = parse_form(body);
        save_networks(fields);
        save_items(fields);
        s_ctx.saved = true;
        ESP_LOGI(TAG, "Settings saved from browser");

        std::string page = PAGE_HEAD;
        page += "<div class=box><h1>Saved</h1><p>Hold button B on the device to leave setup. "
                "It then reconnects with the new settings.</p><p><a href=/ style='color:#2d97dd'>Back to settings</a></p>"
                "</div></body></html>";
        httpd_resp_set_type(req, "text/html");
        return httpd_resp_send(req, page.c_str(), page.size());
    }

    httpd_handle_t start_server()
    {
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.stack_size = 8192;
        config.lru_purge_enable = true;

        httpd_handle_t server = nullptr;
        if (httpd_start(&server, &config) != ESP_OK)
            return nullptr;

        httpd_uri_t index_uri = {};
        index_uri.uri = "/";
        index_uri.method = HTTP_GET;
        index_uri.handler = handle_index;
        httpd_register_uri_handler(server, &index_uri);

        httpd_uri_t save_uri = {};
        save_uri.uri = "/save";
        save_uri.method = HTTP_POST;
        save_uri.handler = handle_save;
        httpd_register_uri_handler(server, &save_uri);

        return server;
    }

    void restart_wifi(HAL::Hal* hal)
    {
        if (hal->wifi()->is_ap_active())
            hal->wifi()->stop_ap();
        else
            hal->wifi()->deinit();
        delay(100);
        if (hal->wifi()->init() && hal->settings()->getBool("wifi", "enabled"))
            hal->wifi()->connect();
    }

    struct Screen_t
    {
        bool use_ap;
        std::string ap_ssid;
        std::string ap_pass;
        std::string url;
        std::string host;
    };

    void draw_screen(HAL::Hal* hal, const Screen_t& screen, bool show_url_qr)
    {
        auto* canvas = hal->canvas();
        const int qr_size = 112;
        const int qr_margin = 4;
        const int text_x = qr_size + qr_margin * 2 + 5;
        const int line_height = 16;

        canvas->fillScreen(THEME_COLOR_BG);

        const bool wifi_qr = screen.use_ap && !show_url_qr;
        const std::string qr_text =
            wifi_qr ? "WIFI:T:WPA;S:" + screen.ap_ssid + ";P:" + screen.ap_pass + ";;" : screen.url;
        canvas->fillRect(0, 0, qr_size + qr_margin * 2, qr_size + qr_margin * 2, TFT_WHITE);
        canvas->qrcode(qr_text.c_str(), qr_margin, qr_margin, qr_size);

        int y = 2;
        canvas->setFont(FONT_14);
        canvas->setTextColor(THEME_COLOR_TITLE);
        canvas->drawString("Web setup", text_x, y);
        y += line_height + 4;

        if (screen.use_ap)
        {
            canvas->setTextColor(wifi_qr ? TFT_GREENYELLOW : TFT_DARKGREY);
            canvas->drawString("1 Join WiFi", text_x, y);
            y += line_height;
            canvas->setTextColor(TFT_WHITE);
            canvas->drawString(screen.ap_ssid.c_str(), text_x, y);
            y += line_height;
            canvas->drawString(("pw " + screen.ap_pass).c_str(), text_x, y);
            y += line_height + 2;
            canvas->setTextColor(wifi_qr ? TFT_DARKGREY : TFT_GREENYELLOW);
            canvas->drawString("2 Open page", text_x, y);
            y += line_height;
        }
        else
        {
            canvas->setTextColor(TFT_GREENYELLOW);
            canvas->drawString("Open page", text_x, y);
            y += line_height;
            canvas->setTextColor(TFT_DARKGREY);
            canvas->drawString("on same WiFi", text_x, y);
            y += line_height + 2;
        }
        canvas->setTextColor(TFT_WHITE);
        canvas->drawString(screen.host.c_str(), text_x, y);
        y += line_height;

        if (s_ctx.saved)
        {
            canvas->setTextColor(TFT_GREEN);
            canvas->drawString("Saved!", text_x, y);
        }

        canvas->setFont(FONT_10);
        canvas->setTextColor(TFT_DARKGREY);
        canvas->drawString(screen.use_ap ? "[A] switch QR code   [hold B] done" : "[hold B] done",
                           2,
                           canvas->height() - 11);
        hal->canvas_update();
    }
} // namespace

namespace UTILS
{
    namespace WEB_SETUP
    {
        void run(HAL::Hal* hal)
        {
            s_ctx.hal = hal;
            s_ctx.saved = false;
            s_ctx.scanned.clear();

            Screen_t screen;
            screen.use_ap = !hal->wifi()->is_connected();

            UTILS::UI::show_progress(hal, "Web setup", -1, "Starting...");
            if (screen.use_ap)
            {
                uint8_t mac[6] = {0};
                esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
                char buf[24];
                snprintf(buf, sizeof(buf), "M5Gemini-%02X%02X", mac[4], mac[5]);
                screen.ap_ssid = buf;
                // one-off password, so the API key and WiFi passwords are not sent over an open network
                snprintf(buf, sizeof(buf), "%08lu", (unsigned long)(esp_random() % 100000000UL));
                screen.ap_pass = buf;

                if (!hal->wifi()->start_ap(screen.ap_ssid, screen.ap_pass))
                {
                    UTILS::UI::show_error_dialog(hal, "Web setup", "Failed to start access point");
                    restart_wifi(hal);
                    return;
                }
                screen.url = AP_URL;
                screen.host = "192.168.4.1";
            }
            else
            {
                screen.host = hal->wifi()->get_ip();
                screen.url = "http://" + screen.host + "/";
            }
            s_ctx.scanned = hal->wifi()->scan();

            httpd_handle_t server = start_server();
            if (!server)
            {
                UTILS::UI::show_error_dialog(hal, "Web setup", "Failed to start web server");
                restart_wifi(hal);
                return;
            }

            bool show_url_qr = false;
            bool drawn_saved = false;
            bool need_draw = true;
            while (true)
            {
                if (drawn_saved != s_ctx.saved)
                    need_draw = true;
                if (need_draw)
                {
                    drawn_saved = s_ctx.saved;
                    draw_screen(hal, screen, show_url_qr);
                    need_draw = false;
                }

                hal->keyboard()->updateKeyList();
                if (hal->keyboard()->isKeyPressing(KEY_NUM_ENTER))
                {
                    hal->keyboard()->waitForRelease(KEY_NUM_ENTER);
                    if (screen.use_ap)
                    {
                        show_url_qr = !show_url_qr;
                        need_draw = true;
                    }
                }
                else if (hal->keyboard()->isKeyPressing(KEY_NUM_ESC) || hal->keyboard()->isKeyPressing(KEY_NUM_BACKSPACE))
                {
                    hal->keyboard()->waitForRelease(KEY_NUM_ESC);
                    break;
                }
                delay(20);
            }

            httpd_stop(server);
            UTILS::UI::show_progress(hal, "Web setup", -1, s_ctx.saved ? "Applying settings..." : "Closing...");
            restart_wifi(hal);
            s_ctx.scanned.clear();
        }
    } // namespace WEB_SETUP
} // namespace UTILS
#endif
