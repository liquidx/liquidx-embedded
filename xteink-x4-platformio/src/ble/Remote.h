#pragma once

#include <blat.h>

// The X4 as a blat device (blat/PROTOCOL.md at the repo root): the controls in
// Controls.h, readable and changeable by a paired host while the Bluetooth app
// is open. Pairing shows a code on screen, which the host's user types in.
namespace remote {

// At boot, after settings::begin().
void begin();
// Radio adds its GATT service and forwards connects and disconnects.
blat::NimBleLink& link();
// Main loop, while the radio is on.
void poll();

// The pairing code to show (six digits), or nullptr.
const char* code();
// True (once) when the code appeared or went away: time to redraw.
bool takeCodeChange();
// True (once) when a host sent anything, for the idle-sleep timer.
bool takeActivity();

}  // namespace remote
