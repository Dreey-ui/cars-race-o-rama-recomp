#pragma once
#include <cstdint>

// State of one controller in XINPUT_GAMEPAD format (same on 360 and PC)
struct HostPadState
{
    uint16_t buttons = 0;
    uint8_t leftTrigger = 0, rightTrigger = 0;
    int16_t thumbLX = 0, thumbLY = 0, thumbRX = 0, thumbRY = 0;
};

// false = controller disconnected
bool HostInputGetState(uint32_t user, HostPadState& out);
void HostInputSetVibration(uint32_t user, uint16_t left, uint16_t right);
