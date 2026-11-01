#pragma once

#include <cstdint>

// Controller haptics the original game does not have: a pulse on every ball hit and a light
// tick for menu navigation. They share the pad with the game's own vibration (sub_82567090).
namespace tabletennis::haptics {

// Pad sample hook (guest input thread): ticks on menu navigation and button presses.
void OnPadSample(uint32_t user, uint16_t buttons, int16_t thumb_lx, int16_t thumb_ly);

}  // namespace tabletennis::haptics
