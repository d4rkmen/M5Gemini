#ifndef Button_h
#define Button_h
#include <stdint.h>

class Button
{
public:
    Button(uint8_t pin, uint16_t debounce_ms = 100);
    bool read();
    bool isToggled();
    bool isPressed();
    bool isReleased();
    bool hasChanged();

    const static bool PRESSED = false;
    const static bool RELEASED = true;

private:
    uint8_t _pin;
    uint16_t _delay;
    bool _state;
    uint32_t _ignore_until;
    bool _has_changed;
};

#endif
