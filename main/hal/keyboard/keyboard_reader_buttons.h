/**
 * @file keyboard_reader_buttons.h
 * @brief Two-button keyboard reader for M5StickS3
 */
#pragma once
#include "keyboard_reader.h"

#define KEYBOARD_BUTTON_A_PIN 11 // KEY1, front button
#define KEYBOARD_BUTTON_B_PIN 12 // KEY2, side button

namespace KEYBOARD
{
    /**
     * @brief Maps the two StickS3 buttons onto Cardputer key positions
     *
     * The app only navigates with ENTER / ESC / UP / DOWN, so each button
     * gets a short-press key and a long-press key:
     *   A short = ENTER, A hold = UP (repeats while held)
     *   B short = DOWN,  B hold = ESC
     * A short press is reported on release, as a brief key pulse.
     */
    class ButtonsKeyboardReader : public KeyboardReader
    {
    public:
        ButtonsKeyboardReader() = default;
        virtual ~ButtonsKeyboardReader() = default;

        void init() override;
        void update() override;

    private:
        static constexpr uint32_t DEBOUNCE_MS = 20;
        static constexpr uint32_t LONG_PRESS_MS = 500;
        static constexpr uint32_t SHORT_PULSE_MS = 40;

        struct Button_t
        {
            int pin;
            Point2D_t short_key;
            Point2D_t long_key;
            bool down = false;
            bool long_fired = false;
            uint32_t down_since = 0;
            uint32_t ignore_until = 0;
        };

        // Key positions in the Cardputer key map (see _key_value_map)
        Button_t _buttons[2] = {{KEYBOARD_BUTTON_A_PIN, {13, 2}, {11, 2}},  // ENTER, UP
                                {KEYBOARD_BUTTON_B_PIN, {11, 3}, {0, 0}}}; // DOWN, ESC

        Point2D_t _pulse_key = {-1, -1};
        uint32_t _pulse_until = 0;
    };

} // namespace KEYBOARD
