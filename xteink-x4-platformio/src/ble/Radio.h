#pragma once

#include <cstdint>

// The Bluetooth LE stack, on while the Bluetooth app is open: one NimBLE GATT
// server carrying two services, blit (Cast: frames in, keys out) and
// blat (Remote: settings). One host connects at a time and may use either or
// both.
namespace radio {

// Bring the stack up, add both services and advertise. False on failure.
bool begin();
// Disconnect and shut the stack down (frees its memory, radio off).
void end();
bool running();
// "X4-1A2B", from the last two bytes of the Bluetooth address. Valid before
// begin().
const char* name();
// The Bluetooth address, most significant byte first.
const uint8_t* address();

}  // namespace radio
