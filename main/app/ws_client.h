#pragma once

bool deepgram_streaming_start(HAL::Hal* hal, EventGroupHandle_t control_event_group);
void deepgram_streaming_stop(void);

std::string get_transcript(void);
