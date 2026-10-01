#include "SettingsApp.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "../Fonts.h"
#include "../hal/Board.h"

namespace {

// A settings row: either a choice (a control from Controls.h), or (choice 0)
// a fixed row.
enum class Row : uint8_t { Brightness, Accent, Sleep, Clock, About, Count };
constexpr int kRowCount = static_cast<int>(Row::Count);
constexpr const char* kRowLabels[kRowCount] = {"Brightness", "Accent", "Sleep after", "Clock", "About"};
constexpr blat::Id kRowChoices[kRowCount] = {settings::kBrightness, settings::kAccent, settings::kSleep,
                                             settings::kClock, 0};

blat::Id rowChoice(const int index) { return kRowChoices[index]; }

// A choice page lists a control's options: an enum's, or Off / On for a bool.
constexpr blat::Option kBoolOptions[] = {{0, "Off"}, {1, "On"}};

const blat::Option* optionsOf(const blat::Control& c, int& count) {
  if (c.type_ == blat::Type::Bool) {
    count = 2;
    return kBoolOptions;
  }
  count = c.optionCount_;
  return c.options_;
}

// Where the current value sits in the options (0 if it isn't one).
int currentIndex(const blat::Id id) {
  int count;
  const blat::Option* options = optionsOf(settings::values().control(id), count);
  const int32_t v = settings::value(id);
  for (int i = 0; i < count; i++) {
    if (options[i].value == v) return i;
  }
  return 0;
}

// "10 min" -> big "10", unit "min"; "Never" -> big "Never", no unit.
void splitLabel(const char* label, char* big, const size_t bigSize, const char** unit) {
  const char* space = strchr(label, ' ');
  const size_t n = space ? static_cast<size_t>(space - label) : strlen(label);
  snprintf(big, bigSize, "%.*s", static_cast<int>(n), label);
  *unit = space ? space + 1 : "";
}

// Choice page geometry: the setting's name and current value on the left, its
// options stacked on the right.
constexpr int kHeadingBaseline = 62;
constexpr int kValueBaseline = 98;
constexpr int kUnitBaseline = 116;
constexpr int kOptionsW = 80;
constexpr int kOptionsRight = 9;  // from the card edge
constexpr int kOptionsTop = 29;
constexpr int kOptionsVisible = 4;

}  // namespace

const char* SettingsApp::rowLabel(const void*, const int index) { return kRowLabels[index]; }

const char* SettingsApp::rowValue(const void* ctx, const int index) {
  const auto* self = static_cast<const SettingsApp*>(ctx);
  if (const blat::Id id = rowChoice(index)) {
    int count;
    return optionsOf(settings::values().control(id), count)[currentIndex(id)].label;
  }
  return self->version_;
}

void SettingsApp::onOpen() {
  page_ = Page::List;
  list_.reset();

  // "0.1.0+abc1234" -> "v0.1.0"
  snprintf(version_, sizeof(version_), "v%s", FW_VERSION);
  if (char* plus = strchr(version_, '+')) *plus = '\0';
}

void SettingsApp::render(Gfx& g, const layout::Rect& area, const bool chrome) {
  switch (page_) {
    case Page::List:
      list_.render(g, area, chrome ? "Settings" : nullptr, kRowCount, rowLabel, rowValue, this);
      break;
    case Page::Choice:
      renderChoice(g, area, chrome);
      break;
    case Page::About:
      renderAbout(g, area, chrome);
      break;
  }
}

void SettingsApp::renderChoice(Gfx& g, const layout::Rect& area, const bool chrome) const {
  const auto& c = settings::values().control(choice_);
  char title[48];
  snprintf(title, sizeof(title), "Settings / %s", kRowLabels[list_.selected()]);
  ui::drawTitle(g, area, chrome ? title : nullptr);

  // Left: the setting's name, and its current value large.
  int count;
  const blat::Option* options = optionsOf(c, count);
  const int x = area.x + layout::kMarginLeft;
  const int current = currentIndex(choice_);
  char big[24];
  const char* unit;
  splitLabel(options[current].label, big, sizeof(big), &unit);
  ui::drawTextAt(g, fonts::SMALL_12, x, area.y + kHeadingBaseline, c.label_, theme::kTextDim);
  ui::drawTextAt(g, fonts::LARGE_30, x, area.y + kValueBaseline, big);
  ui::drawTextAt(g, fonts::SMALL_12, x, area.y + kUnitBaseline, unit, theme::kTextDim);

  // Right: the options, the current one selected. Moving the selection
  // scrolls them to keep a neighbour either side in view.
  const auto& style = ui::kListRow;
  const int top = std::clamp(current - (kOptionsVisible - 2), 0, std::max(0, count - kOptionsVisible));
  const int optionsX = area.x + area.w - kOptionsRight - kOptionsW;
  for (int i = top; i < std::min(count, top + kOptionsVisible); i++) {
    const layout::Rect row{optionsX, area.y + kOptionsTop + (i - top) * style.pitch, kOptionsW, style.height};
    splitLabel(options[i].label, big, sizeof(big), &unit);
    ui::drawRow(g, row, style, ui::Icon::None, big, nullptr, i == current);
  }
  ui::drawScrollbar(g, area, top, kOptionsVisible, count);
}

void SettingsApp::renderAbout(Gfx& g, const layout::Rect& area, const bool chrome) {
  board::About about;
  board::about(about);
  char res[16], battery[24];
  snprintf(res, sizeof(res), "%dx%d", layout::kScreenW, layout::kScreenH);
  snprintf(battery, sizeof(battery), "%d%%%s", board::batteryPercent(),
           board::charging() ? " charging" : (board::onUsb() ? " (USB)" : ""));

  const ui::InfoView::Row rows[] = {
      {"Device", "M5StickS3"}, {"Firmware", FW_VERSION}, {"Chip", about.chip},     {"Flash", about.flash},
      {"Display", res},        {"MAC", about.mac},       {"Battery", battery},     {"Memory", about.memory},
  };
  info_.render(g, area, chrome ? "Settings / About" : nullptr, rows, sizeof(rows) / sizeof(rows[0]));
}

Result SettingsApp::handle(const Action action) {
  switch (page_) {
    case Page::List:
      switch (action) {
        case Action::Up:
        case Action::Down:
          return list_.move(action == Action::Up ? -1 : 1, kRowCount) ? Result::Redraw : Result::Ignored;
        case Action::Select:
          if ((choice_ = rowChoice(list_.selected())) != 0) {
            page_ = Page::Choice;
          } else {
            page_ = Page::About;
            info_.reset();
          }
          return Result::Redraw;
        default:
          return Result::Ignored;
      }

    case Page::Choice:
      switch (action) {
        case Action::Up:
        case Action::Down: {
          // Moving the selection applies it: the big value updates in place.
          // Down is the easy key (a tap), so it wraps round.
          int count;
          const blat::Option* options = optionsOf(settings::values().control(choice_), count);
          const int next = (currentIndex(choice_) + (action == Action::Up ? -1 : 1) + count) % count;
          settings::set(choice_, options[next].value);
          return Result::Redraw;
        }
        case Action::Select:
        case Action::Back:
          page_ = Page::List;
          return Result::Redraw;
        default:
          return Result::Ignored;
      }

    case Page::About:
      switch (action) {
        case Action::Up:
          return info_.scroll(-1) ? Result::Redraw : Result::Ignored;
        case Action::Down:
          return info_.scroll(1) ? Result::Redraw : Result::Ignored;
        case Action::Select:
        case Action::Back:
          page_ = Page::List;
          return Result::Redraw;
        default:
          return Result::Ignored;
      }
  }
  return Result::Ignored;
}
