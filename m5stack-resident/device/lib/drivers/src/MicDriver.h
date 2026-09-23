#ifndef MIC_DRIVER_H
#define MIC_DRIVER_H

#include <cstdint>
#include <ResidentDriver.h>
#include <ResidentLuaModule.h>

// Resident driver for the SPM1423 PDM microphone via M5Unified.
// Lua API: mic.level() -> normalised volume, 0.0 (silent) – 1.0 (loud)
//
// Sampling runs in update() at full loop rate; level() returns the most
// recent RMS value so Lua apps pay no I2S cost in on_tick.
class MicDriver : public Resident::Driver {
public:
  const char* name() const override { return "mic"; }
  void begin() override;
  void update() override;
  void onAppReset() override {}
  void registerModule(Resident::LuaModule& m) override {
    m.method<MicDriver, &MicDriver::level>("level");
  }

  int level(lua_State* L);

private:
  static constexpr int      SAMPLE_RATE  = 16000;
  static constexpr size_t   SAMPLE_COUNT = 256;

  int16_t _buf[SAMPLE_COUNT];
  float   _level = 0.0f;
};

#endif // MIC_DRIVER_H
