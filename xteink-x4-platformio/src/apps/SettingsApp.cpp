#include "SettingsApp.h"

#include <BoardConfig.h>
#include <HalGPIO.h>
#include <HalMemory.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <esp_mac.h>

#include <cstdio>

namespace {

struct Entry {
  const char* label;
  SettingsApp::Page page;
};

constexpr Entry kEntries[] = {
    {"Wi-Fi", SettingsApp::Page::Wifi},
    {"About", SettingsApp::Page::About},
};
constexpr int kEntryCount = sizeof(kEntries) / sizeof(kEntries[0]);

const char* entryLabel(const void*, const int index) { return kEntries[index].label; }

void formatMac(char* out, const size_t size) {
  uint8_t m[6] = {0};
  esp_read_mac(m, ESP_MAC_WIFI_STA);
  snprintf(out, size, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

}  // namespace

void SettingsApp::onOpen() {
  page_ = Page::List;
  list_.reset();
}

void SettingsApp::render(GfxRenderer& r, const layout::Rect& area, const bool chrome) {
  switch (page_) {
    case Page::List:
      list_.render(r, area, chrome ? "Settings" : nullptr, kEntryCount, entryLabel, nullptr);
      break;
    case Page::Wifi:
      renderWifi(r, area, chrome);
      break;
    case Page::About:
      renderAbout(r, area, chrome);
      break;
  }
}

void SettingsApp::renderWifi(GfxRenderer& r, const layout::Rect& area, const bool chrome) {
  char mac[24];
  formatMac(mac, sizeof(mac));
  const ui::InfoView::Row rows[] = {
      {"Status", "Not configured"},
      {"MAC", mac},
  };
  info_.render(r, area, chrome ? "Wi-Fi" : nullptr, rows, sizeof(rows) / sizeof(rows[0]));
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
  info_.render(r, area, chrome ? "About" : nullptr, rows, sizeof(rows) / sizeof(rows[0]));
}

KeyHints SettingsApp::hints() const {
  if (page_ == Page::List) return {nullptr, "Open", "Up", "Down"};
  return {nullptr, nullptr, info_.canScrollUp() ? "Up" : nullptr, info_.canScrollDown() ? "Down" : nullptr};
}

Result SettingsApp::handle(const Action action) {
  if (page_ != Page::List) {
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

  switch (action) {
    case Action::Up:
    case Action::Down:
      return list_.move(action == Action::Up ? -1 : 1, kEntryCount) ? Result::Redraw : Result::Ignored;
    case Action::Select:
      page_ = kEntries[list_.selected()].page;
      info_.reset();
      return Result::Redraw;
    default:
      return Result::Ignored;
  }
}
