/**
 * @file settings_screen.cpp
 * @brief Settings screens implementation
 */

#include "settings_screen.h"
#include "esp_log.h"
#include "../anim/hl_text.h"
#include "app/utils/ui/dialog.h"
#include "app/utils/common_define.h"

static const char* TAG = "SETTINGS_SCREEN";
static const char* HINT_ITEMS = "[UP][DOWN] [LEFT][RIGHT] [ENTER] [ESC]";
static const char* HINT_GROUPS = HINT_ITEMS;

// Keyboard constants
#define KEY_HOLD_MS 500
#define KEY_REPEAT_MS 50
// Scroll constants
#define DESC_SCROLL_PAUSE 1000
#define DESC_SCROLL_SPEED 20

#define HINT_HEIGHT 12
#define ITEMS_Y_OFFSET 20

static bool is_repeat = false;
static bool is_start = false;
static int selected_group = 0;
static int selected_item = 0;
static int scroll_offset = 0;
static bool in_group = false;
static bool need_render = true;

using namespace UTILS::HL_TEXT;

namespace UTILS
{
    namespace UI
    {
        namespace SETTINGS_SCREEN
        {
            bool render_groups(HAL::Hal* hal, const std::vector<SETTINGS::SettingGroup_t>& groups, HLTextContext_t* hint_ctx)
            {
                if (!need_render)
                    return false;

                hal->canvas()->fillScreen(THEME_COLOR_BG);
                hal->canvas()->setTextColor(TFT_WHITE, THEME_COLOR_BG);
                hal->canvas()->setFont(FONT_16);
                hal->canvas()->drawString("Settings", 5, 0);

                int y_offset = ITEMS_Y_OFFSET;
                int line_height = hal->canvas()->fontHeight(FONT_16) + 2 + 1;
                int max_visible_groups = (hal->canvas()->height() - ITEMS_Y_OFFSET - HINT_HEIGHT) / line_height;
                int groups_drawn = 0;

                // Render visible groups
                for (size_t i = scroll_offset; i < groups.size() && groups_drawn < max_visible_groups; i++)
                {
                    if (i == selected_group)
                    {
                        hal->canvas()->fillSmoothCircle(12, y_offset + line_height / 2 + 1, 6, TFT_GREENYELLOW);
                        hal->canvas()->setTextColor(TFT_GREENYELLOW, THEME_COLOR_BG);
                    }
                    else
                    {
                        hal->canvas()->setTextColor(TFT_ORANGE, THEME_COLOR_BG);
                    }

                    hal->canvas()->drawString(groups[i].name.c_str(), 26, y_offset + 1);
                    y_offset += line_height;
                    groups_drawn++;
                }

                // Draw scrollbar if needed
                if (groups.size() > max_visible_groups)
                {
                    int scrollbar_height = line_height * max_visible_groups;
                    int scrollbar_width = 6;
                    int scrollbar_x = hal->canvas()->width() - scrollbar_width - 1;

                    hal->canvas()->drawRect(scrollbar_x, ITEMS_Y_OFFSET, scrollbar_width, scrollbar_height, TFT_DARKGREY);

                    int thumb_height = scrollbar_height * max_visible_groups / groups.size();
                    int thumb_pos = ITEMS_Y_OFFSET +
                                    (scrollbar_height - thumb_height) * scroll_offset / (groups.size() - max_visible_groups);

                    hal->canvas()->fillRect(scrollbar_x, thumb_pos, scrollbar_width, thumb_height, TFT_ORANGE);
                }

                need_render = false;
                return true;
            }

            bool render_items(HAL::Hal* hal, SETTINGS::SettingGroup_t& group)
            {
                if (!need_render)
                    return false;

                hal->canvas()->fillScreen(THEME_COLOR_BG);
                hal->canvas()->setTextColor(TFT_ORANGE, THEME_COLOR_BG);
                hal->canvas()->setFont(FONT_16);
                hal->canvas()->drawString(group.name.c_str(), 5, 0);

                int y_offset = ITEMS_Y_OFFSET;
                int items_drawn = 0;
                const int max_width = hal->canvas()->width() - 5 - 6;

                int line_height = hal->canvas()->fontHeight(FONT_16) + 2 + 1;
                int max_visible_lines = (hal->canvas()->height() - y_offset - HINT_HEIGHT) / line_height;

                for (size_t i = scroll_offset; i < group.items.size() && items_drawn < max_visible_lines; i++)
                {
                    auto& item = group.items[i];

                    if (i == selected_item)
                    {
                        hal->canvas()->fillRect(5, y_offset + 1, max_width, line_height - 1, THEME_COLOR_BG_SELECTED);
                        hal->canvas()->setTextColor(TFT_BLACK, THEME_COLOR_BG_SELECTED);
                    }
                    else
                    {
                        hal->canvas()->setTextColor(item.type == SETTINGS::TYPE_NONE ? TFT_YELLOW : TFT_CYAN, THEME_COLOR_BG);
                    }

                    // Draw label
                    hal->canvas()->drawString(item.label.c_str(), 10, y_offset + 1);

                    // Get current value from settings
                    switch (item.type)
                    {
                    case SETTINGS::TYPE_NONE:
                        break;
                    case SETTINGS::TYPE_BOOL:
                        item.value = hal->settings()->getBool(group.nvs_namespace, item.key) ? "true" : "false";
                        break;
                    case SETTINGS::TYPE_NUMBER:
                        item.value = std::to_string(hal->settings()->getNumber(group.nvs_namespace, item.key));
                        break;
                    case SETTINGS::TYPE_STRING:
                        item.value = hal->settings()->getString(group.nvs_namespace, item.key);
                        break;
                    }
                    std::string display_value = item.key == "pass" ? "******" : item.value;

                    int value_width = hal->canvas()->textWidth(display_value.c_str());
                    int max_value_width = max_width - hal->canvas()->textWidth(item.label.c_str()) - 20;

                    if (value_width > max_value_width)
                    {
                        display_value = display_value.substr(0, max_value_width / hal->canvas()->textWidth("0")) + ">";
                    }

                    if (i == selected_item)
                    {
                        hal->canvas()->setTextColor(item.type == SETTINGS::TYPE_NONE ? TFT_DARKGRAY : TFT_BLACK,
                                                    THEME_COLOR_BG_SELECTED);
                    }
                    else
                    {
                        hal->canvas()->setTextColor(item.type == SETTINGS::TYPE_NONE ? TFT_DARKGRAY : TFT_WHITE,
                                                    THEME_COLOR_BG);
                    }
                    hal->canvas()->drawRightString(display_value.c_str(), hal->canvas()->width() - 10, y_offset + 1);

                    y_offset += line_height;
                    items_drawn++;
                }

                // Draw scrollbar if needed
                int scrollbar_height = line_height * max_visible_lines;
                int scrollbar_width = 6;
                if (group.items.size() > max_visible_lines)
                {
                    int scrollbar_x = hal->canvas()->width() - scrollbar_width - 1;

                    hal->canvas()->drawRect(scrollbar_x, ITEMS_Y_OFFSET, scrollbar_width, scrollbar_height, TFT_DARKGREY);

                    int thumb_height = scrollbar_height * max_visible_lines / group.items.size();
                    int thumb_pos = ITEMS_Y_OFFSET + (scrollbar_height - thumb_height) * scroll_offset /
                                                         (group.items.size() - max_visible_lines);

                    hal->canvas()->fillRect(scrollbar_x, thumb_pos, scrollbar_width, thumb_height, TFT_ORANGE);
                }

                need_render = false;
                return true;
            }

            bool render_scrolling_desc(HAL::Hal* hal,
                                       const SETTINGS::SettingGroup_t& group,
                                       SCROLL_TEXT::ScrollTextContext_t* desc_scroll_ctx)
            {
                // Display only after delay 3000ms
                if (millis() - hal->keyboard()->lastPressedTime() < 3000)
                {
                    return false;
                }

                std::string desc = group.items[selected_item].hint;
                // No info, use current desc
                if (desc.length() == 0)
                {
                    return false;
                }

                // Only update the canvas if the text scrolled
                return SCROLL_TEXT::scroll_text_render(desc_scroll_ctx,
                                                       desc.c_str(),
                                                       0,                                         // x position
                                                       0,                                         // y position
                                                       lgfx::v1::convert_to_rgb888(TFT_DARKGREY), // text color
                                                       THEME_COLOR_BG);                           // background color
            }

            bool handle_group_selection(HAL::Hal* hal,
                                        std::vector<SETTINGS::SettingGroup_t>& groups,
                                        std::function<void(int group_index)> on_enter)
            {
                bool selection_changed = false;
                int line_height = hal->canvas()->fontHeight(FONT_16) + 2 + 1;
                int max_visible_groups = (hal->canvas()->height() - ITEMS_Y_OFFSET - HINT_HEIGHT) / line_height;

                hal->keyboard()->updateKeyList();
                hal->keyboard()->updateKeysState();
                if (hal->keyboard()->isPressed())
                {
                    if (hal->keyboard()->isKeyPressing(KEY_NUM_DOWN))
                    {
                        if (!is_repeat ||
                            !hal->keyboard()->waitForRelease(KEY_NUM_DOWN, is_start ? KEY_HOLD_MS : KEY_REPEAT_MS))
                        {
                            is_start = !is_repeat;
                            is_repeat = true;
                            hal->playNextSound();

                            if (selected_group < groups.size() - 1)
                            {
                                selected_group++;
                                if (selected_group >= scroll_offset + max_visible_groups)
                                {
                                    scroll_offset = selected_group - max_visible_groups + 1;
                                }
                                selection_changed = true;
                            }
                        }
                    }
                    else if (hal->keyboard()->isKeyPressing(KEY_NUM_UP))
                    {
                        if (!is_repeat || !hal->keyboard()->waitForRelease(KEY_NUM_UP, is_start ? KEY_HOLD_MS : KEY_REPEAT_MS))
                        {
                            is_start = !is_repeat;
                            is_repeat = true;
                            hal->playNextSound();

                            if (selected_group > 0)
                            {
                                selected_group--;
                                if (selected_group < scroll_offset)
                                {
                                    scroll_offset = selected_group;
                                }
                                selection_changed = true;
                            }
                        }
                    }
                    else if (hal->keyboard()->isKeyPressing(KEY_NUM_LEFT))
                    {
                        if (!is_repeat ||
                            !hal->keyboard()->waitForRelease(KEY_NUM_LEFT, is_start ? KEY_HOLD_MS : KEY_REPEAT_MS))
                        {
                            is_start = !is_repeat;
                            is_repeat = true;
                            hal->playNextSound();

                            // Jump up by visible_items count (page up)
                            int jump = max_visible_groups;
                            if (selected_group > 0)
                            {
                                selected_group = std::max(0, selected_group - jump);
                                scroll_offset = std::max(0, selected_group - (max_visible_groups - 1));
                                selection_changed = true;
                            }
                        }
                    }
                    else if (hal->keyboard()->isKeyPressing(KEY_NUM_RIGHT))
                    {
                        if (!is_repeat ||
                            !hal->keyboard()->waitForRelease(KEY_NUM_RIGHT, is_start ? KEY_HOLD_MS : KEY_REPEAT_MS))
                        {
                            is_start = !is_repeat;
                            is_repeat = true;
                            hal->playNextSound();

                            int jump = max_visible_groups;
                            if (selected_group < groups.size() - 1)
                            {
                                selected_group = std::min((int)groups.size() - 1, selected_group + jump);
                                scroll_offset = std::min(std::max(0, (int)groups.size() - max_visible_groups), selected_group);
                                selection_changed = true;
                            }
                        }
                    }
                    else if (hal->keyboard()->isKeyPressing(KEY_NUM_ENTER))
                    {
                        hal->playNextSound();
                        hal->keyboard()->waitForRelease(KEY_NUM_ENTER);
                        hal->playLastSound();
                        if (groups[selected_group].items.size() > 0)
                        {
                            in_group = true;
                            selected_item = 0;
                            scroll_offset = 0;
                            selection_changed = true;
                        }
                        if (on_enter)
                        {
                            on_enter(selected_group);
                            need_render = true;
                        }
                    }
                    else if (hal->keyboard()->isKeyPressing(KEY_NUM_BACKSPACE))
                    {
                        hal->playNextSound();
                        hal->keyboard()->waitForRelease(KEY_NUM_BACKSPACE);
                        hal->playLastSound();

                        in_group = false;
                        selected_group = 0;
                        scroll_offset = 0;
                        selection_changed = true;
                        if (on_enter)
                        {
                            on_enter(-1);
                        }
                    }
                    else if (hal->keyboard()->isKeyPressing(KEY_NUM_ESC))
                    {
                        hal->playNextSound();
                        hal->keyboard()->waitForRelease(KEY_NUM_ESC);
                        hal->playLastSound();

                        in_group = false;
                        selected_group = 0;
                        scroll_offset = 0;
                        selection_changed = true;
                        if (on_enter)
                        {
                            on_enter(-1);
                        }
                    }
                }
                else
                    is_repeat = false;

                return selection_changed;
            }

            bool
            handle_item_selection(HAL::Hal* hal,
                                  SETTINGS::SettingGroup_t& group,
                                  SCROLL_TEXT::ScrollTextContext_t* desc_scroll_ctx,
                                  std::function<void(SETTINGS::SettingGroup_t&, SETTINGS::SettingItem_t&)> on_item_selected)
            {
                bool selection_changed = false;
                int line_height = hal->canvas()->fontHeight(FONT_16) + 2 + 1;
                // int y_offset = ITEMS_Y_OFFSET;
                int max_visible_lines = (hal->canvas()->height() - ITEMS_Y_OFFSET - HINT_HEIGHT) / line_height;

                hal->keyboard()->updateKeyList();
                hal->keyboard()->updateKeysState();

                if (hal->keyboard()->isPressed())
                {
                    if (hal->keyboard()->isKeyPressing(KEY_NUM_DOWN))
                    {
                        if (!is_repeat ||
                            !hal->keyboard()->waitForRelease(KEY_NUM_DOWN, is_start ? KEY_HOLD_MS : KEY_REPEAT_MS))
                        {
                            is_start = !is_repeat;
                            is_repeat = true;
                            hal->playNextSound();

                            if (selected_item < group.items.size() - 1)
                            {
                                selected_item++;
                                if (selected_item >= scroll_offset + max_visible_lines)
                                {
                                    scroll_offset = selected_item - max_visible_lines + 1;
                                }
                                selection_changed = true;
                            }
                        }
                    }
                    else if (hal->keyboard()->isKeyPressing(KEY_NUM_UP))
                    {
                        if (!is_repeat || !hal->keyboard()->waitForRelease(KEY_NUM_UP, is_start ? KEY_HOLD_MS : KEY_REPEAT_MS))
                        {
                            is_start = !is_repeat;
                            is_repeat = true;
                            hal->playNextSound();

                            if (selected_item > 0)
                            {
                                selected_item--;
                                if (selected_item < scroll_offset)
                                {
                                    scroll_offset = selected_item;
                                }
                                selection_changed = true;
                            }
                        }
                    }
                    else if (hal->keyboard()->isKeyPressing(KEY_NUM_LEFT))
                    {
                        if (!is_repeat ||
                            !hal->keyboard()->waitForRelease(KEY_NUM_LEFT, is_start ? KEY_HOLD_MS : KEY_REPEAT_MS))
                        {
                            is_start = !is_repeat;
                            is_repeat = true;
                            hal->playNextSound();

                            // Jump up by visible_items count (page up)
                            int jump = max_visible_lines;
                            if (selected_item > 0)
                            {
                                selected_item = std::max(0, selected_item - jump);
                                scroll_offset = std::max(0, selected_item - (max_visible_lines - 1));
                                selection_changed = true;
                            }
                        }
                    }
                    else if (hal->keyboard()->isKeyPressing(KEY_NUM_RIGHT))
                    {
                        if (!is_repeat ||
                            !hal->keyboard()->waitForRelease(KEY_NUM_RIGHT, is_start ? KEY_HOLD_MS : KEY_REPEAT_MS))
                        {
                            is_start = !is_repeat;
                            is_repeat = true;
                            hal->playNextSound();
                            // Jump down by visible_items count (page down)
                            int jump = max_visible_lines;
                            if (selected_item < group.items.size() - 1)
                            {
                                selected_item = std::min((int)group.items.size() - 1, selected_item + jump);
                                scroll_offset =
                                    std::min(std::max(0, (int)group.items.size() - max_visible_lines), selected_item);
                                selection_changed = true;
                            }
                        }
                    }
                    else if (hal->keyboard()->isKeyPressing(KEY_NUM_BACKSPACE))
                    {
                        hal->playNextSound();
                        hal->keyboard()->waitForRelease(KEY_NUM_BACKSPACE);
                        hal->playLastSound();

                        in_group = false;
                        selected_item = 0;
                        scroll_offset = 0;
                        selection_changed = true;
                    }
                    else if (hal->keyboard()->isKeyPressing(KEY_NUM_ESC))
                    {
                        hal->playNextSound();
                        hal->keyboard()->waitForRelease(KEY_NUM_ESC);
                        hal->playLastSound();

                        in_group = false;
                        selected_item = 0;
                        scroll_offset = 0;
                        selection_changed = true;
                    }
                    else if (hal->keyboard()->isKeyPressing(KEY_NUM_ENTER))
                    {
                        hal->playNextSound();
                        hal->keyboard()->waitForRelease(KEY_NUM_ENTER);
                        hal->playLastSound();

                        auto& item = group.items[selected_item];

                        if (item.type == SETTINGS::TYPE_NONE)
                        {
                            // Back item
                            in_group = false;
                            selected_item = 0;
                            scroll_offset = 0;
                            selection_changed = true;
                        }
                        else if (on_item_selected)
                        {
                            // Call the provided callback
                            on_item_selected(group, item);
                            selection_changed = true;
                        }
                    }
                }
                else
                    is_repeat = false;

                if (selection_changed)
                {
                    // Reset scroll context to start from the beginning
                    SCROLL_TEXT::scroll_text_reset(desc_scroll_ctx);
                }

                return selection_changed;
            }
            void reset()
            {
                in_group = false;
                selected_group = 0;
                selected_item = 0;
                scroll_offset = 0;
            }
            // Main update function that handles all settings screen functionality
            bool update(HAL::Hal* hal,
                        std::vector<SETTINGS::SettingGroup_t>& groups,
                        HLTextContext_t* hint_ctx,
                        SCROLL_TEXT::ScrollTextContext_t* desc_ctx,
                        std::function<void(int group_index)> on_enter)
            {
                bool update_needed = false;
                // Handle input based on current state
                if (in_group)
                {
                    // In a group, display items and handle item selection                    {
                    update_needed |= render_items(hal, groups[selected_group]);
                    // Render scrolling description if available
                    update_needed |= render_scrolling_desc(hal, groups[selected_group], desc_ctx);

                    // Render hint
                    update_needed |= hl_text_render(hint_ctx,
                                                    HINT_ITEMS,
                                                    0,
                                                    hal->canvas()->height() - HINT_HEIGHT,
                                                    TFT_DARKGREY,
                                                    TFT_WHITE,
                                                    THEME_COLOR_BG);
                    // Handle item selection input
                    auto& group = groups[selected_group];
                    need_render |= handle_item_selection(hal,
                                                         group,
                                                         desc_ctx,
                                                         [hal](SETTINGS::SettingGroup_t& group, SETTINGS::SettingItem_t& item)
                                                         {
                                                             // Callback when item is selected
                                                             handle_setting_change(hal, group, item);
                                                         });
                }
                else
                {
                    // At group level, display groups and handle group selection
                    update_needed |= render_groups(hal, groups, hint_ctx);

                    // Render hint
                    update_needed |= hl_text_render(hint_ctx,
                                                    HINT_GROUPS,
                                                    0,
                                                    hal->canvas()->height() - HINT_HEIGHT,
                                                    TFT_DARKGREY,
                                                    TFT_WHITE,
                                                    THEME_COLOR_BG);
                    // Handle group selection input
                    need_render |=
                        handle_group_selection(hal,
                                               groups,
                                               [hal, on_enter](int group_index)
                                               {
                                                   ESP_LOGI(TAG, "handle_group_selection: group_index=%d", group_index);
                                                   if (on_enter)
                                                   {
                                                       on_enter(group_index);
                                                   }
                                               });
                }

                return update_needed;
            }

            // Handle setting changes when an item is selected
            void handle_setting_change(HAL::Hal* hal, SETTINGS::SettingGroup_t& group, SETTINGS::SettingItem_t& item)
            {
                ESP_LOGI(TAG, "handle_setting_change: group=%s, item=%s", group.nvs_namespace.c_str(), item.key.c_str());
                bool value_changed = false;

                switch (item.type)
                {
                case SETTINGS::TYPE_NONE:
                    break;
                case SETTINGS::TYPE_BOOL:
                    // Toggle the boolean value
                    item.value = (item.value == "true") ? "false" : "true";
                    value_changed = true;
                    break;

                case SETTINGS::TYPE_NUMBER:
                {
                    // Edit number using dialog
                    int value = std::stoi(item.value);
                    int min_value = item.min_val.empty() ? 0 : std::stoi(item.min_val);
                    int max_value = item.max_val.empty() ? 999 : std::stoi(item.max_val);

                    if (UTILS::UI::show_edit_number_dialog(hal, item.label, value, min_value, max_value))
                    {
                        item.value = std::to_string(value);
                        value_changed = true;
                    }
                    break;
                }

                case SETTINGS::TYPE_STRING:
                {
                    // Special handling for specific settings
                    if (!item.min_val.empty() && !hal->keyboard()->keysState().fn)
                    {
                        // draw list selection dialog
                        std::vector<std::string> options;
                        // split min_val with ; to get option. use find
                        size_t pos = 0;
                        std::string val = item.min_val;
                        while ((pos = val.find(';')) != std::string::npos)
                        {
                            options.push_back(val.substr(0, pos));
                            val.erase(0, pos + 1);
                        }
                        options.push_back(val);
                        // find index of current value
                        int current_index = 0;
                        for (int i = 0; i < options.size(); i++)
                        {
                            if (options[i] == item.value)
                            {
                                current_index = i;
                                break;
                            }
                        }
                        int res = UTILS::UI::show_select_dialog(hal, item.label, options, current_index);
                        if (res != -1)
                        {
                            item.value = options[res];
                            value_changed = true;
                        }
                    }
                    else if (group.nvs_namespace == "wifi" && item.key == "ssid" && !hal->keyboard()->keysState().fn)
                    {
                        // Show scanning dialog
                        UTILS::UI::show_progress(hal, "Scanning WiFi", -1, "Please wait");
                        // Scan for networks
                        std::vector<std::string> networks = hal->wifi()->scan();

                        if (networks.empty())
                        {
                            UTILS::UI::show_error_dialog(hal, "WiFi", "No networks found");
                            break;
                        }
                        // find index of current value
                        int current_index = 0;
                        for (int i = 0; i < networks.size(); i++)
                        {
                            if (networks[i] == item.value)
                            {
                                current_index = i;
                                break;
                            }
                        }

                        int res = UTILS::UI::show_select_dialog(hal, item.label, networks, current_index);
                        if (res != -1)
                        {
                            item.value = networks[res];
                            value_changed = true;
                        }
                    }
                    else
                    {
                        std::string value = item.value;
                        bool is_password = (item.key == "pass");
                        if (UTILS::UI::show_edit_string_dialog(hal, item.label, value, is_password, 50))
                        {
                            item.value = value;
                            value_changed = true;
                        }
                    }
                    break;
                }
                }
                // Save the setting if value changed
                if (value_changed)
                {
                    save_setting(hal, group, item);

                    // if namespace == wifi - reinit wifi
                    if (group.nvs_namespace == "wifi")
                    {
                        hal->wifi()->deinit();
                        delay(100);
                        if (hal->wifi()->init())
                        {
                            hal->wifi()->connect();
                        }
                        else
                        {
                            UTILS::UI::show_error_dialog(hal, "WiFi", "Failed to init WiFi");
                        }
                    }
                    else if (group.nvs_namespace == "system")
                    {
                        if (item.key == "brightness")
                        {
                            hal->display()->setBrightness(std::stoi(item.value));
                        }
                        else if (item.key == "volume")
                        {
                            // hal->speaker()->setVolume(std::stoi(item.value));
                        }
                    }
                }
            }

            // Save a single setting item
            void save_setting(HAL::Hal* hal, const SETTINGS::SettingGroup_t& group, const SETTINGS::SettingItem_t& item)
            {
                bool success = false;

                switch (item.type)
                {
                case SETTINGS::TYPE_NONE:
                    break;
                case SETTINGS::TYPE_BOOL:
                    success = hal->settings()->setBool(group.nvs_namespace, item.key, item.value == "true");
                    break;

                case SETTINGS::TYPE_NUMBER:
                    success = hal->settings()->setNumber(group.nvs_namespace, item.key, std::stoi(item.value));
                    break;

                case SETTINGS::TYPE_STRING:
                    success = hal->settings()->setString(group.nvs_namespace, item.key, item.value);
                    break;

                default:
                    break;
                }

                if (!success)
                {
                    ESP_LOGE(TAG, "Failed to save setting %s.%s", group.nvs_namespace.c_str(), item.key.c_str());
                    UTILS::UI::show_error_dialog(hal, "Save Error", "Failed to save setting", "OK");
                }
            }
        } // namespace SETTINGS_SCREEN
    } // namespace UI
} // namespace UTILS