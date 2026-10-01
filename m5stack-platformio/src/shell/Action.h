#pragma once

#include <cstdint>

// Semantic actions. Apps only ever see these, never raw buttons, so the
// physical mapping lives in exactly one place (Input.cpp).
//
// The stick has two keys where the X4 has four:
//
//   front key (A)   press: Down (next; lists wrap round)   double press: Select
//   top key (B)     press: Back                            double press: show / hide chrome
//
// Nothing on the device sends Up; it remains for the serial port
// (scripts/devctl.py) and for apps shared with a device that has the key.
enum class Action : uint8_t {
  None,
  Back,          // Home at an app's top level, Back once drilled in
  Select,
  Up,
  Down,
  ToggleChrome,  // show/hide the gutter and titles
};
