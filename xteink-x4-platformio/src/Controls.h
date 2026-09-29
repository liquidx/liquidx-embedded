#pragma once

#include <blat.h>

// Everything the X4 can be told to change, in one table: each setting's
// type, options, default and where it's saved, and what a paired host may do
// with it over Bluetooth (blat/PROTOCOL.md at the repo root). The Settings app
// draws its pages from this table too, so a setting added here shows up on the
// device and on a host alike.
//
// Format: blat/firmware/README.md. Ids for the firmware are in Settings.h.
namespace controls {

inline constexpr blat::Option kRefreshOptions[] = {{6, "6 pages"}, {12, "12 pages"}, {24, "24 pages"}, {0, "Never"}};
inline constexpr blat::Option kSleepOptions[] = {
    {1, "1 min"}, {5, "5 min"}, {10, "10 min"}, {30, "30 min"}, {0, "Never"}};
inline constexpr blat::Option kClockOptions[] = {{24, "24 hour"}, {12, "12 hour"}};

inline constexpr blat::Control kControls[] = {
    blat::group("display", "Display"),
    blat::choice("display.refresh", kRefreshOptions)
        .in("display")
        .label("Full refresh every")
        .help("Clears ghosting left by fast partial refreshes. Higher is faster; lower is cleaner.")
        .initial(12)
        .saveAs("refresh")
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
        .help("Time without a key press before the device sleeps. Hold Power to sleep at any time.")
        .initial(10)
        .saveAs("sleep")
        .live(),
    blat::number("power.battery").in("power").label("Battery").unit("%").range(0, 100).readOnly().live(),

    blat::group("bluetooth", "Bluetooth"),
    blat::toggle("bluetooth.frameSleep")
        .in("bluetooth")
        .label("Sleep between frames")
        .help("When a Bluetooth sender says when its next frame is due, sleep until just before it.")
        .saveAs("framesleep")
        .live(),

    blat::group("system", "System"),
    blat::text("system.version").in("system").label("Firmware").length(0, 48).initial(FW_VERSION).readOnly(),
    // Needs someone at the device: a code entered on this connection.
    blat::action("system.restart")
        .in("system")
        .label("Restart")
        .help("Restarts the X4. Bluetooth hosts need to reconnect afterwards.")
        .confirm()
        .write(blat::kLevelPresent),
};
static_assert(blat::check(kControls));

}  // namespace controls
