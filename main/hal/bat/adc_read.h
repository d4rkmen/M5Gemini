#pragma once
#ifdef HAVE_BATTERY
#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>

    void adc_read_init();
    void adc_read_deinit();
    uint32_t adc_read_get_value();

#ifdef __cplusplus
}
#endif
#endif