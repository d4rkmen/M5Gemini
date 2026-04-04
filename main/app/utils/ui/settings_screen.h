/**
 * @file settings_screen.h
 * @brief Settings screens declarations
 */

#pragma once

#include "hal.h"
#include "../../../settings/settings.h"
#include "../anim/hl_text.h"
#include "../anim/scroll_text.h"
#include <functional>
#include <string>

namespace UTILS
{
    namespace UI
    {
        namespace SETTINGS_SCREEN
        {
            bool render_groups(HAL::Hal* hal,
                               const std::vector<SETTINGS::SettingGroup_t>& groups,
                               HL_TEXT::HLTextContext_t* hint_ctx);

            bool render_items(HAL::Hal* hal, SETTINGS::SettingGroup_t& group);

            bool render_scrolling_desc(HAL::Hal* hal,
                                       const SETTINGS::SettingGroup_t& group,
                                       SCROLL_TEXT::ScrollTextContext_t* desc_scroll_ctx);

            bool
            handle_group_selection(HAL::Hal* hal, std::vector<SETTINGS::SettingGroup_t>& groups, std::function<void()> on_exit);

            bool handle_item_selection(HAL::Hal* hal,
                                       SETTINGS::SettingGroup_t& group,
                                       SCROLL_TEXT::ScrollTextContext_t* desc_scroll_ctx);

            void reset();

            bool update(HAL::Hal* hal,
                        std::vector<SETTINGS::SettingGroup_t>& groups,
                        HL_TEXT::HLTextContext_t* hint_ctx,
                        SCROLL_TEXT::ScrollTextContext_t* desc_ctx,
                        std::function<void()> on_exit);

            void handle_setting_change(HAL::Hal* hal, SETTINGS::SettingGroup_t& group, SETTINGS::SettingItem_t& item);

            void save_setting(HAL::Hal* hal, const SETTINGS::SettingGroup_t& group, const SETTINGS::SettingItem_t& item);

        } // namespace SETTINGS_SCREEN
    } // namespace UI
} // namespace UTILS
