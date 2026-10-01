#pragma once

#include <blat.h>

#include "Controls.h"

// User settings: the values of the controls in Controls.h, saved to NVS. The
// Settings app changes them on the device; a paired Bluetooth host can change
// them over blat while the Bluetooth app is open.
namespace settings {

// Ids of the controls the firmware reads, checked at compile time.
constexpr blat::Id kBrightness = blat::idOf(controls::kControls, "display.brightness");  // percent
constexpr blat::Id kAccent = blat::idOf(controls::kControls, "display.accent");          // index, see Theme.h
constexpr blat::Id kClock = blat::idOf(controls::kControls, "display.clock");            // 24 or 12
constexpr blat::Id kSleep = blat::idOf(controls::kControls, "power.sleep");              // minutes; 0 = never
constexpr blat::Id kBattery = blat::idOf(controls::kControls, "power.battery");
constexpr blat::Id kRestart = blat::idOf(controls::kControls, "system.restart");

// Load saved values.
void begin();

blat::Values& values();
// The current value of a bool, int or enum control.
inline int32_t value(const blat::Id id) { return values().get(id); }
// Change a value from the device itself. Hosts hear about it if it's live.
inline void set(const blat::Id id, const int32_t v) { values().set(id, v); }
// The NVS store settings live in, shared with blat's remembered hosts. Null
// where there's no NVS (sim/): nothing is saved.
blat::Store* store();

}  // namespace settings
