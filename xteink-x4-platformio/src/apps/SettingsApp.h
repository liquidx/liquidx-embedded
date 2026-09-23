#pragma once

#include "../shell/App.h"
#include "../ui/Widgets.h"

// Settings: a list of pages (Wi-Fi, About). Select opens a page, Up/Down
// scroll it, Back returns to the list.
class SettingsApp : public App {
 public:
  enum class Page : uint8_t { List, Wifi, About };

  const char* name() const override { return "Settings"; }
  void onOpen() override;
  void render(GfxRenderer& r, const layout::Rect& area, bool chrome) override;
  Result handle(Action action) override;
  bool canGoBack() const override { return page_ != Page::List; }
  KeyHints hints() const override;

 private:
  void renderWifi(GfxRenderer& r, const layout::Rect& area, bool chrome);
  void renderAbout(GfxRenderer& r, const layout::Rect& area, bool chrome);

  Page page_ = Page::List;
  ui::ListView list_;
  ui::InfoView info_;
};
