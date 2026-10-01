#pragma once

#include <blat.h>

// Everything the stick can be told to change, in one table: each setting's
// type, options, default and where it's saved, and what a paired host may do
// with it over Bluetooth (blat/PROTOCOL.md at the repo root). The Settings app
// draws its pages from this table too, so a setting added here shows up on the
// device and on a host alike.
//
// Format: blat/firmware/README.md. Ids for the firmware are in Settings.h.
namespace controls {

inline constexpr blat::Option kBrightnessOptions[] = {{25, "25%"}, {50, "50%"}, {75, "75%"}, {100, "100%"}};
// Values index theme::kAccents (Theme.h).
inline constexpr blat::Option kAccentOptions[] = {{0, "Orange"}, {1, "Blue"}, {2, "Green"}, {3, "Pink"}, {4, "White"}};
inline constexpr blat::Option kSleepOptions[] = {
    {1, "1 min"}, {5, "5 min"}, {10, "10 min"}, {30, "30 min"}, {0, "Never"}};
inline constexpr blat::Option kClockOptions[] = {{24, "24 hour"}, {12, "12 hour"}};

inline constexpr blat::Control kControls[] = {
    blat::group("display", "Display"),
    blat::choice("display.brightness", kBrightnessOptions)
        .in("display")
        .label("Brightness")
        .help("How bright the screen is. Lower lasts longer on battery.")
        .initial(75)
        .saveAs("bright")
        .live(),
    blat::choice("display.accent", kAccentOptions)
        .in("display")
        .label("Accent")
        .help("The colour of the selected row, titles and lit keys.")
        .initial(0)
        .saveAs("accent")
        .live(),
    blat::choice("display.clock", kClockOptions)
        .in("display")
        .label("Clock shows")
        .help("How the home screen shows the time.")
        .initial(24)
        .saveAs("clock")
        .live(),

    blat::group("power", "Power"),
    blat::choice("power.sleep", kSleepOptions)
        .in("power")
        .label("Sleep after")
        .help("Time on battery without a key press before the device sleeps. The front key wakes it.")
        .initial(5)
        .saveAs("sleep")
        .live(),
    blat::number("power.battery").in("power").label("Battery").unit("%").range(0, 100).readOnly().live(),

    blat::group("system", "System"),
    blat::text("system.version").in("system").label("Firmware").length(0, 48).initial(FW_VERSION).readOnly(),
    // Needs someone at the device: a code entered on this connection.
    blat::action("system.restart")
        .in("system")
        .label("Restart")
        .help("Restarts the stick. Bluetooth hosts need to reconnect afterwards.")
        .confirm()
        .write(blat::kLevelPresent),
};
static_assert(blat::check(kControls));

}  // namespace controls
