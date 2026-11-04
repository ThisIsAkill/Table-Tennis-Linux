#pragma once

#include <cstdint>

// MOTION SWING: with a controller that has a gyro (DualSense, DualShock 4, Switch Pro, Steam
// Deck with Steam Input off), a flick of the wrist plays a shot during a match. The flick picks
// the spin like the face buttons' diamond: tip the top of the controller forward for topspin
// (A), back for backspin (Y), turn it left for leftspin (X) or right for rightspin (B).
namespace tabletennis::motion {

// Pad poll hook (game input thread, player one): returns the buttons with any flick added.
uint16_t AddSwingButtons(uint32_t user, uint16_t buttons);

// Settings page status: whether a gyro is available on player one's controller.
bool GyroAvailable();

}  // namespace tabletennis::motion
