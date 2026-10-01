#pragma once

#include <blit.h>

// The stick as a blit display (blit/PROTOCOL.md at the repo root): the
// Bluetooth app's frames in, its keys out. The protocol is the blit library's
// (blit/firmware); this sets up what the stick is. The BLE stack is Radio's
// (Radio.h), which adds the link's service to its server (Links.h).
namespace cast {

// Largest payload as sent, and largest frame once decoded: rgb565 of the
// whole panel.
constexpr size_t kMaxBytes = 240 * 135 * 2;

// At boot.
void begin();
blit::Receiver& receiver();

}  // namespace cast
