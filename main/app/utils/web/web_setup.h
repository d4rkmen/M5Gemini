/**
 * @file web_setup.h
 * @brief Browser based settings editor for boards without a keyboard
 */
#pragma once

#include "hal.h"

namespace UTILS
{
    namespace WEB_SETUP
    {
        /**
         * @brief Serve the settings page until the user leaves the screen
         *
         * Uses the current WiFi connection when there is one, otherwise starts a
         * password protected access point. Blocks, then restarts WiFi with the
         * saved settings.
         */
        void run(HAL::Hal* hal);
    } // namespace WEB_SETUP
} // namespace UTILS
