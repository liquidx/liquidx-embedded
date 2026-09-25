#include "SettingsApp.h"

#include <BoardConfig.h>
#include <HalGPIO.h>
#include <HalMemory.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <SDCardManager.h>
#include <esp_mac.h>

#include <cstdio>
#include <cstring>

#include "../Fonts.h"

namespace {

// A settings row: either a choice, or (choice == nullptr) a fixed row.
enum class Row : uint8_t { Refresh, Sleep, Clock, SdCard, About, Count };
constexpr int kRowCount = static_cast<int>(Row::Count);
constexpr const char* kRowLabels[kRowCount] = {"Refresh", "Sleep after", "Clock", "Storage", "About"};

const settings::Choice* rowChoice(const int index) {
  switch (static_cast<Row>(index)) {
    case Row::Refresh:
      return &settings::kRefresh;
    case Row::Sleep:
      return &settings::kSleep;
    case Row::Clock:
      return &settings::kClock;
    default:
      return nullptr;
  }
}

// Choice page geometry (docs/design/04-settings-refresh.png).
constexpr int kHeadingBaseline = 224;
constexpr int kValueBaseline = 363;
constexpr int kUnitGap = 12;
constexpr int kDescBaseline = 410;
constexpr int kDescPitch = 19;
constexpr int kDescWidth = 290;
constexpr int kOptionsW = 220;
constexpr int kOptionsRight = 33;   // from the card edge
constexpr int kOptionsBottom = 28;  // from the card bottom

void formatMac(char* out, const size_t size) {
  uint8_t m[6] = {0};
  esp_read_mac(m, ESP_MAC_WIFI_STA);
  snprintf(out, size, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

}  // namespace

const char* SettingsApp::rowLabel(const void*, const int index) { return kRowLabels[index]; }

const char* SettingsApp::rowValue(const void* ctx, const int index) {
  const auto* self = static_cast<const SettingsApp*>(ctx);
  if (const auto* choice = rowChoice(index)) return choice->options[settings::index(*choice)].list;
  return static_cast<Row>(index) == Row::SdCard ? self->storage_ : self->version_;
}

void SettingsApp::onOpen() {
  page_ = Page::List;
  list_.reset();

  // "0.1.0+abc1234" -> "v0.1.0"
  snprintf(version_, sizeof(version_), "v%s", FW_VERSION);
  if (char* plus = strchr(version_, '+')) *plus = '\0';

  // Counting free clusters walks the FAT, so do it once per open.
  if (Storage.ready()) {
    constexpr double kGB = 1024.0 * 1024.0 * 1024.0;
    auto& sd = SDCardManager::getInstance();
    snprintf(storage_, sizeof(storage_), "%.1f / %.0f GB", sd.sdUsedBytes() / kGB, sd.sdTotalBytes() / kGB);
  } else {
    snprintf(storage_, sizeof(storage_), "No card");
  }
}

void SettingsApp::render(GfxRenderer& r, const layout::Rect& area, const bool chrome) {
  switch (page_) {
    case Page::List:
      list_.render(r, area, chrome ? "Settings" : nullptr, kRowCount, rowLabel, rowValue, this);
      break;
    case Page::Choice:
      renderChoice(r, area, chrome);
      break;
    case Page::About:
      renderAbout(r, area, chrome);
      break;
  }
}

void SettingsApp::renderChoice(GfxRenderer& r, const layout::Rect& area, const bool chrome) const {
  const auto& c = *choice_;
  char title[48];
  snprintf(title, sizeof(title), "Settings / %s", c.title);
  ui::drawTitle(r, area, chrome ? title : nullptr);

  // Left: heading, the current value large, and what the setting does.
  const int x = area.x + layout::kMarginLeft;
  const int current = settings::index(c);
  const auto& opt = c.options[current];
  ui::drawTextAt(r, fonts::MEDIUM_22, x, area.y + kHeadingBaseline, c.heading);
  ui::drawTextAt(r, fonts::DISPLAY_136, x, area.y + kValueBaseline, opt.big);
  if (opt.unit[0] != '\0') {
    const int unitX = x + r.getTextWidth(fonts::DISPLAY_136, opt.big) + kUnitGap;
    ui::drawTextAt(r, fonts::MEDIUM_22, unitX, area.y + kValueBaseline, opt.unit);
  }
  int baseline = area.y + kDescBaseline;
  for (const auto& line : r.wrappedText(fonts::SMALL_15, c.description, kDescWidth, 3)) {
    ui::drawTextAt(r, fonts::SMALL_15, x, baseline, line.c_str());
    baseline += kDescPitch;
  }

  // Right: the options, bottom-aligned, current one selected.
  const auto& style = ui::kListRow;
  const int optionsX = area.x + area.w - kOptionsRight - kOptionsW;
  const int optionsY = area.y + area.h - kOptionsBottom - (c.count * style.pitch - (style.pitch - style.height));
  for (int i = 0; i < c.count; i++) {
    const layout::Rect row{optionsX, optionsY + i * style.pitch, kOptionsW, style.height};
    ui::drawRow(r, row, style, ui::Icon::None, c.options[i].big, nullptr, i == current);
  }
}

void SettingsApp::renderAbout(GfxRenderer& r, const layout::Rect& area, const bool chrome) {
  char chip[32], flash[16], res[16], mac[24], battery[16], heap[32];
  snprintf(chip, sizeof(chip), "%s rev %u", ESP.getChipModel(), static_cast<unsigned>(ESP.getChipRevision()));
  snprintf(flash, sizeof(flash), "%u MB", static_cast<unsigned>(ESP.getFlashChipSize() / (1024u * 1024u)));
  snprintf(res, sizeof(res), "%ux%u", static_cast<unsigned>(BoardConfig::ACTIVE.displayWidth),
           static_cast<unsigned>(BoardConfig::ACTIVE.displayHeight));
  formatMac(mac, sizeof(mac));
  snprintf(battery, sizeof(battery), "%u%%%s", powerManager.getBatteryPercentage(),
           gpio.isUsbConnected() ? " (USB)" : "");
  const auto h = HalMemory::getInternalHeap();
  snprintf(heap, sizeof(heap), "%u KB free", static_cast<unsigned>(h.freeBytes / 1024));

  const ui::InfoView::Row rows[] = {
      {"Device", BoardConfig::ACTIVE.name},
      {"Firmware", FW_VERSION},
      {"Chip", chip},
      {"Flash", flash},
      {"Display", res},
      {"MAC", mac},
      {"Battery", battery},
      {"Memory", heap},
      {"SD card", Storage.ready() ? "Mounted" : "Not found"},
  };
  info_.render(r, area, chrome ? "Settings / About" : nullptr, rows, sizeof(rows) / sizeof(rows[0]));
}

Result SettingsApp::handle(const Action action) {
  switch (page_) {
    case Page::List:
      switch (action) {
        case Action::Up:
        case Action::Down:
          return list_.move(action == Action::Up ? -1 : 1, kRowCount) ? Result::Redraw : Result::Ignored;
        case Action::Select:
          if ((choice_ = rowChoice(list_.selected()))) {
            page_ = Page::Choice;
          } else if (static_cast<Row>(list_.selected()) == Row::About) {
            page_ = Page::About;
            info_.reset();
          } else {
            return Result::Ignored;
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
          const int next = settings::index(*choice_) + (action == Action::Up ? -1 : 1);
          if (next < 0 || next >= choice_->count) return Result::Ignored;
          settings::set(*choice_, next);
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
        case Action::Back:
          page_ = Page::List;
          return Result::Redraw;
        default:
          return Result::Ignored;
      }
  }
  return Result::Ignored;
}
