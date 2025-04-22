/**
 * @file keyboard.h
 * @author Forairaaaaa
 * @brief
 * @version 0.1
 * @date 2023-09-22
 *
 * @copyright Copyright (c) 2023
 *
 */
#pragma once
#include <iostream>
#include <vector>
#include "keymap.h"

#define KEY_NUM_ESC 1
#define KEY_NUM_1 2
#define KEY_NUM_2 3
#define KEY_NUM_3 4
#define KEY_NUM_4 5
#define KEY_NUM_5 6
#define KEY_NUM_6 7
#define KEY_NUM_7 8
#define KEY_NUM_8 9
#define KEY_NUM_9 10
#define KEY_NUM_0 11
#define KEY_NUM_UNDERSCORE 12
#define KEY_NUM_EQUAL 13
#define KEY_NUM_BACKSPACE 14
#define KEY_NUM_TAB 15
#define KEY_NUM_Q 16
#define KEY_NUM_W 17
#define KEY_NUM_E 18
#define KEY_NUM_R 19
#define KEY_NUM_T 20
#define KEY_NUM_Y 21
#define KEY_NUM_U 22
#define KEY_NUM_I 23
#define KEY_NUM_O 24
#define KEY_NUM_P 25
#define KEY_NUM_LEFTBRACE 26
#define KEY_NUM_RIGHTBRACE 27
#define KEY_NUM_BACKSLASH 28
#define KEY_NUM_FN 29
#define KEY_NUM_SHIFT 30
#define KEY_NUM_A 31
#define KEY_NUM_S 32
#define KEY_NUM_D 33
#define KEY_NUM_F 34
#define KEY_NUM_G 35
#define KEY_NUM_H 36
#define KEY_NUM_J 37
#define KEY_NUM_K 38
#define KEY_NUM_L 39
#define KEY_NUM_UP 40
#define KEY_NUM_APOSTROPHE 41
#define KEY_NUM_ENTER 42
#define KEY_NUM_CTRL 43
#define KEY_NUM_OPT 44
#define KEY_NUM_ALT 45
#define KEY_NUM_Z 46
#define KEY_NUM_X 47
#define KEY_NUM_C 48
#define KEY_NUM_V 49
#define KEY_NUM_B 50
#define KEY_NUM_N 51
#define KEY_NUM_M 52
#define KEY_NUM_LEFT 53
#define KEY_NUM_DOWN 54
#define KEY_NUM_RIGHT 55
#define KEY_NUM_SPACE 56

namespace KEYBOARD
{
    struct Chart_t
    {
        uint8_t value;
        uint8_t x_1;
        uint8_t x_2;
    };

    struct Point2D_t
    {
        int x;
        int y;
    };

    const std::vector<int> output_list = {8, 9, 11};
    const std::vector<int> input_list = {13, 15, 3, 4, 5, 6, 7};
    // const std::vector<int> input_list = {1, 2, 3, 4, 5, 6, 7};

    const Chart_t X_map_chart[7] = {{1, 0, 1}, {2, 2, 3}, {4, 4, 5}, {8, 6, 7}, {16, 8, 9}, {32, 10, 11}, {64, 12, 13}};

    struct KeyValue_t
    {
        const char* value_first;
        const int value_num_first;
        const char* value_second;
        const int value_num_second;
    };

    const KeyValue_t _key_value_map[4][14] = {{{"`", KEY_GRAVE, "~", KEY_GRAVE},
                                               {"1", KEY_1, "!", KEY_1},
                                               {"2", KEY_2, "@", KEY_2},
                                               {"3", KEY_3, "#", KEY_3},
                                               {"4", KEY_4, "$", KEY_4},
                                               {"5", KEY_5, "%", KEY_5},
                                               {"6", KEY_6, "^", KEY_6},
                                               {"7", KEY_7, "&", KEY_7},
                                               {"8", KEY_8, "*", KEY_KPASTERISK},
                                               {"9", KEY_9, "(", KEY_KPLEFTPAREN},
                                               {"0", KEY_0, ")", KEY_KPRIGHTPAREN},
                                               {"-", KEY_MINUS, "_", KEY_KPMINUS},
                                               {"=", KEY_EQUAL, "+", KEY_KPPLUS},
                                               {"del", KEY_BACKSPACE, "del", KEY_BACKSPACE}},
                                              {{"tab", KEY_TAB, "tab", KEY_TAB},
                                               {"q", KEY_Q, "Q", KEY_Q},
                                               {"w", KEY_W, "W", KEY_W},
                                               {"e", KEY_E, "E", KEY_E},
                                               {"r", KEY_R, "R", KEY_R},
                                               {"t", KEY_T, "T", KEY_T},
                                               {"y", KEY_Y, "Y", KEY_Y},
                                               {"u", KEY_U, "U", KEY_U},
                                               {"i", KEY_I, "I", KEY_I},
                                               {"o", KEY_O, "O", KEY_O},
                                               {"p", KEY_P, "P", KEY_P},
                                               {"[", KEY_LEFTBRACE, "{", KEY_LEFTBRACE},
                                               {"]", KEY_RIGHTBRACE, "}", KEY_RIGHTBRACE},
                                               {"\\", KEY_BACKSLASH, "|", KEY_BACKSLASH}},
                                              {{"fn", 0, "fn", 0},
                                               {"shift", 0, "shift", 0},
                                               {"a", KEY_A, "A", KEY_A},
                                               {"s", KEY_S, "S", KEY_S},
                                               {"d", KEY_D, "D", KEY_D},
                                               {"f", KEY_F, "F", KEY_F},
                                               {"g", KEY_G, "G", KEY_G},
                                               {"h", KEY_H, "H", KEY_H},
                                               {"j", KEY_J, "J", KEY_J},
                                               {"k", KEY_K, "K", KEY_K},
                                               {"l", KEY_L, "L", KEY_L},
                                               {";", KEY_SEMICOLON, ":", KEY_SEMICOLON},
                                               {"'", KEY_APOSTROPHE, "\"", KEY_APOSTROPHE},
                                               {"enter", KEY_ENTER, "enter", KEY_ENTER}},
                                              {{"ctrl", KEY_LEFTCTRL, "ctrl", KEY_LEFTCTRL},
                                               {"opt", 0, "opt", 0},
                                               {"alt", KEY_LEFTALT, "alt", KEY_LEFTALT},
                                               {"z", KEY_Z, "Z", KEY_Z},
                                               {"x", KEY_X, "X", KEY_X},
                                               {"c", KEY_C, "C", KEY_C},
                                               {"v", KEY_V, "V", KEY_V},
                                               {"b", KEY_B, "B", KEY_B},
                                               {"n", KEY_N, "N", KEY_N},
                                               {"m", KEY_M, "M", KEY_M},
                                               {",", KEY_COMMA, "<", KEY_COMMA},
                                               {".", KEY_DOT, ">", KEY_DOT},
                                               {"/", KEY_KPSLASH, "?", KEY_KPSLASH},
                                               {"space", KEY_SPACE, "space", KEY_SPACE}}};

    class Keyboard
    {
    public:
        struct KeysState
        {
            bool tab = false;
            bool fn = false;
            bool shift = false;
            bool ctrl = false;
            bool opt = false;
            bool alt = false;
            bool del = false;
            bool enter = false;
            bool space = false;

            std::vector<char> values;
            std::vector<int> hidKey;

            void reset()
            {
                tab = false;
                fn = false;
                shift = false;
                ctrl = false;
                opt = false;
                alt = false;
                del = false;
                enter = false;
                space = false;

                values.clear();
                hidKey.clear();
            }
        };

    private:
        std::vector<Point2D_t> _key_list_buffer;

        std::vector<Point2D_t> _key_values_without_special_keys;
        KeysState _keys_state_buffer;

        bool _is_caps_locked;
        uint8_t _last_key_size;
        uint32_t _last_pressed_time;

        void _set_output(const std::vector<int>& pinList, uint8_t output);
        uint8_t _get_input(const std::vector<int>& pinList);

    public:
        Keyboard() : _is_caps_locked(false), _last_key_size(0) {}

        void init();

        Point2D_t getKey();

        uint8_t getKeyNum(Point2D_t keyCoor);

        void updateKeyList();
        inline std::vector<KEYBOARD::Point2D_t>& keyList() { return _key_list_buffer; }

        inline KeyValue_t getKeyValue(const Point2D_t& keyCoor) { return _key_value_map[keyCoor.y][keyCoor.x]; }

        bool isKeyPressing(int keyNum);
        bool waitForRelease(int keyNum, int timeout_ms = 30000);
        uint32_t lastPressedTime();

        void updateKeysState();
        inline KeysState& keysState() { return _keys_state_buffer; }

        inline bool capslocked(void) { return _is_caps_locked; }
        inline void setCapsLocked(bool isLocked) { _is_caps_locked = isLocked; }

        bool isChanged();
        inline uint8_t isPressed() { return _key_list_buffer.size(); }
    };
} // namespace KEYBOARD
