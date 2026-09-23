#include "MicDriver.h"
#include <M5Unified.h>
#include <math.h>

extern "C" {
  #include "lua/lua.h"
  #include "lua/lauxlib.h"
}

void MicDriver::begin() {
  M5.Mic.begin();
}

// Called every loop iteration. Attempts a non-blocking drain of whatever
// DMA samples are ready, then recomputes the cached RMS level.
// wait_ticks=0 means return immediately if no data is available yet —
// _level retains its previous value until the next chunk arrives.
void MicDriver::update() {
  if (!M5.Mic.isEnabled()) return;
  if (!M5.Mic.record(_buf, SAMPLE_COUNT, SAMPLE_RATE, false)) return;

  float sum = 0.0f;
  for (size_t i = 0; i < SAMPLE_COUNT; i++) {
    float s = _buf[i] / 32768.0f;
    sum += s * s;
  }
  float rms = sqrtf(sum / SAMPLE_COUNT);
  // Clamp to [0, 1] — RMS of a full-scale sine is ~0.707, so headroom exists.
  _level = rms < 1.0f ? rms : 1.0f;
}

// mic.level() -> float [0.0, 1.0]
int MicDriver::level(lua_State* L) {
  lua_pushnumber(L, _level);
  return 1;
}
