#pragma once

#include <blit.h>

// The X4 as a blit display (blit/PROTOCOL.md at the repo root): the Bluetooth
// app's frames in, its keys out. The protocol is the blit library's
// (blit/firmware); this sets up what the X4 is. The BLE stack is Radio's
// (Radio.h), which adds the link's service to its server.
namespace cast {

// Largest payload as sent, and largest frame once decoded: a gray2 frame of
// the whole panel on the X4C; the original X4 has no PSRAM and takes mono1.
#if FREEINK_MCU_C3
constexpr size_t kMaxBytes = 64 * 1024;
#else
constexpr size_t kMaxBytes = 96000;
#endif

// At boot.
void begin();
blit::Receiver& receiver();
// Radio adds its GATT service and forwards connects and disconnects.
blit::NimBleLink& link();

}  // namespace cast
