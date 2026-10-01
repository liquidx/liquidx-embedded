#pragma once

#include <cstdint>

// What the stand-in board and radio report (SimBoard.cpp, SimBle.cpp), for
// main.cpp to set a scene up.
namespace sim {

extern uint8_t heldKeys;         // one bit per gutter button
extern int battery;              // percent
extern bool charging;
extern const char* pairingCode;  // six digits, or nullptr

}  // namespace sim
