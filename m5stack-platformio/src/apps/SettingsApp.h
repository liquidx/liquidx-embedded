#pragma once

#include "../Settings.h"
#include "../shell/App.h"

// Settings: a list of rows with their current values. Select opens a row's
// page: a vertical choice (Brightness, Accent, Sleep after, Clock) or About.
// Choice pages are drawn from the control's declaration in Controls.h: its
// label and options.
class SettingsApp : public App {
 public:
  const char* name() const override { return "Settings"; }
  ui::Icon icon() const override { return ui::Icon::Sliders; }
  void onOpen() override;
  void render(Gfx& g, const layout::Rect& area, bool chrome) override;
  Result handle(Action action) override;
  int depth() const override { return page_ == Page::List ? 0 : 1; }

 private:
  enum class Page : uint8_t { List, Choice, About };

  static const char* rowLabel(const void* ctx, int index);
  static const char* rowValue(const void* ctx, int index);
  void renderChoice(Gfx& g, const layout::Rect& area, bool chrome) const;
  void renderAbout(Gfx& g, const layout::Rect& area, bool chrome);

  Page page_ = Page::List;
  blat::Id choice_ = 0;  // open on Page::Choice
  ui::ListView list_;
  ui::InfoView info_;
  char version_[24] = "";
};
