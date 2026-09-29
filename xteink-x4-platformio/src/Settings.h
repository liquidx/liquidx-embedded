#pragma once

#include <blat.h>

#include "Controls.h"

// User settings: the values of the controls in Controls.h, saved to NVS. The
// Settings app changes them on the device; a paired Bluetooth host can change
// them over blat while the Bluetooth app is open.
namespace settings {

// Ids of the controls the firmware reads, checked at compile time.
constexpr blat::Id kRefresh = blat::idOf(controls::kControls, "display.refresh");  // pages; 0 = never
constexpr blat::Id kClock = blat::idOf(controls::kControls, "display.clock");      // 24 or 12
constexpr blat::Id kSleep = blat::idOf(controls::kControls, "power.sleep");        // minutes; 0 = never
constexpr blat::Id kBattery = blat::idOf(controls::kControls, "power.battery");
// 1 = deep-sleep between Bluetooth frames when the sender says when the next
// is due (blit/PROTOCOL.md at the repo root); 0 = stay connected.
constexpr blat::Id kFrameSleep = blat::idOf(controls::kControls, "bluetooth.frameSleep");
constexpr blat::Id kRestart = blat::idOf(controls::kControls, "system.restart");

// Load saved values (moving any from before blat across).
void begin();

blat::Values& values();
// The current value of a bool, int or enum control.
inline int32_t value(const blat::Id id) { return values().get(id); }
// Change a value from the device itself. Hosts hear about it if it's live.
inline void set(const blat::Id id, const int32_t v) { values().set(id, v); }
// The NVS store settings live in, shared with blat's remembered hosts.
blat::Store& store();

}  // namespace settings
