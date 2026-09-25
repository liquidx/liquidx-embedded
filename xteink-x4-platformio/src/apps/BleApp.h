#pragma once

#include "../ble/CastServer.h"
#include "../shell/App.h"

// A remote screen over Bluetooth LE (blit/PROTOCOL.md at the repo root). While open,
// the device advertises and shows the last frame it received, or "Listening".
// The title pill shows the link status. Frames are saved to /images unless the
// sender turns persist off. Up / Down / Select go to the host; Back leaves the
// app, which turns the radio off.
class BleApp : public App {
 public:
  const char* name() const override { return "Bluetooth"; }
  ui::Icon icon() const override { return ui::Icon::Bluetooth; }
  void onOpen() override;
  void onClose() override;
  void render(GfxRenderer& r, const layout::Rect& area, bool chrome) override;
  Result handle(Action action) override;
  Result tick() override;
  bool hasGray() const override { return grayShown_; }
  void drawGray(uint8_t* lsb, uint8_t* msb, const layout::Rect& area) const override;
  void presented() override;
  bool takeActivity() override;
  uint32_t takeSleepRequest() override;
  // While a frame is on screen, a badge shows the app is still listening.
  Badge badge() const override { return showingFrame_ ? Badge::Listening : Badge::None; }

 private:
  enum class Status : uint8_t { Unavailable, Listening, Connected, Receiving, Waiting, Failed };

  Status status() const;
  void statusText(char* out, size_t size) const;
  Result showFrame(const cast::FrameHeader& header);
  void pollBattery();
  bool loadLastFrame();
  void drawFrame(GfxRenderer& r, const layout::Rect& area) const;
  bool persist(const cast::FrameHeader& header, char* path, size_t pathSize);

  cast::Buffer frame_;         // the last full frame received, pixels as sent
  cast::Buffer region_;        // the last region received
  cast::FrameHeader header_;   // ...the full frame's header
  bool haveFrame_ = false;
  bool showingFrame_ = false;  // the last render drew a frame, not "Listening"
  bool grayShown_ = false;     // ...and it has greys
  bool chrome_ = true;         // the last render had chrome
  layout::Rect pill_{};        // the title pill over the frame, if any
  Status shownStatus_ = Status::Unavailable;  // what the screen shows now
  uint32_t nextSeconds_ = 0;   // the last frame's next-frame hint
  char lastPath_[96] = "";     // last persisted frame, shown after a restart
  uint32_t sleepRequest_ = 0;  // seconds
  bool donePending_ = false;   // send `done` once the frame is on the panel
  uint32_t lastBatteryPollMs_ = 0;
  uint8_t sentBattery_ = 255;  // the level last sent to the host
  bool sentUsb_ = false;
};
