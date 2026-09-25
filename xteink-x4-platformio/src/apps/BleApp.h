#pragma once

#include "../ble/CastServer.h"
#include "../shell/App.h"

// A remote screen over Bluetooth LE (blit/PROTOCOL.md at the repo root). While open,
// the device advertises and shows the last frame it received, or "Listening".
// The title pill shows the link status. Frames are saved to /images unless the
// sender turns persist off. Leaving the app turns the radio off.
class BleApp : public App {
 public:
  const char* name() const override { return "Bluetooth"; }
  ui::Icon icon() const override { return ui::Icon::Bluetooth; }
  void onOpen() override;
  void onClose() override;
  void render(GfxRenderer& r, const layout::Rect& area, bool chrome) override;
  Result handle(Action) override { return Result::Ignored; }
  Result tick() override;
  bool takeActivity() override;
  uint32_t takeSleepRequest() override;
  // While a frame is on screen, a badge shows the app is still listening.
  Badge badge() const override { return showingFrame_ ? Badge::Listening : Badge::None; }

 private:
  enum class Status : uint8_t { Unavailable, Listening, Connected, Receiving, Waiting, Failed };

  Status status() const;
  void statusText(char* out, size_t size) const;
  bool loadLastFrame();
  void drawFrame(GfxRenderer& r, const layout::Rect& area) const;
  bool persist(const cast::FrameHeader& header, char* path, size_t pathSize);

  cast::Buffer frame_;         // the last frame received, raw1
  cast::FrameHeader header_;   // ...and its header
  bool haveFrame_ = false;
  bool showingFrame_ = false;  // the last render drew a frame, not "Listening"
  Status shownStatus_ = Status::Unavailable;  // what the screen shows now
  uint32_t nextSeconds_ = 0;   // the last frame's next-frame hint
  char lastPath_[96] = "";     // last persisted frame, shown after a restart
  uint32_t sleepRequest_ = 0;  // seconds
};
