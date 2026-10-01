#include "Radio.h"

#include <NimBLEDevice.h>
#include <esp_mac.h>

#include <cstdio>

#include "../Log.h"
#include "Links.h"

namespace radio {

namespace {

constexpr uint16_t kPreferredMtu = 517;

bool running_ = false;
char name_[12] = "";
uint8_t address_[6] = {};

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* gatt, NimBLEConnInfo& info) override {
    // Transfer rate is set by the link: ask for the shortest connection
    // interval Apple hosts accept (15–30 ms; they require max ≥ min + 15 ms,
    // in 1.25 ms units) and full-size link-layer packets (Data Length
    // Extension, else each 508-byte write goes out as ~19 small packets).
    // macOS still picks 30 ms. Don't request the 2M PHY: macOS drops the link
    // (supervision timeout).
    const uint16_t conn = info.getConnHandle();
    gatt->updateConnParams(conn, 12, 24, 0, 400);
    gatt->setDataLen(conn, 251);
    cast::link().onConnect(conn);
    remote::link().onConnect(conn);
    LOG_INF("BLE", "Connected");
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
    LOG_INF("BLE", "Disconnected (reason %d)", reason);
    cast::link().onDisconnect();
    remote::link().onDisconnect();
    NimBLEDevice::startAdvertising();
  }
  void onMTUChange(uint16_t mtu, NimBLEConnInfo&) override { LOG_INF("BLE", "MTU %u", mtu); }
  void onConnParamsUpdate(NimBLEConnInfo& info) override {
    LOG_INF("BLE", "Connection interval %.2f ms", info.getConnInterval() * 1.25f);
  }
};

ServerCallbacks serverCallbacks;

void readAddress() {
  if (name_[0] != '\0') return;
  esp_read_mac(address_, ESP_MAC_BT);
  snprintf(name_, sizeof(name_), "M5-%02X%02X", address_[4], address_[5]);
}

}  // namespace

const char* name() {
  readAddress();
  return name_;
}

const uint8_t* address() {
  readAddress();
  return address_;
}

bool running() { return running_; }

bool begin() {
  if (running_) return true;
  if (!NimBLEDevice::init("")) return false;
  NimBLEDevice::setDeviceName(name());
  NimBLEDevice::setMTU(kPreferredMtu);

  NimBLEServer* gatt = NimBLEDevice::createServer();
  gatt->setCallbacks(&serverCallbacks, false);
  cast::link().addService(gatt);
  remote::link().addService(gatt);
  gatt->start();

  // Advertisement: flags, the blit UUID and the name (30 of 31 bytes, so the
  // name can't grow). The blat UUID doesn't fit, so NimBLE puts it in the scan
  // response, which hosts filtering on it still see (they scan actively).
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(blit::kServiceUuid);
  adv->setName(name());
  adv->addServiceUUID(blat::kServiceUuid);
  adv->enableScanResponse(true);
  if (!adv->start()) {
    LOG_ERR("BLE", "Advertising failed to start");
    end();
    return false;
  }
  running_ = true;
  LOG_INF("BLE", "Advertising as %s", name());
  return true;
}

void end() {
  NimBLEDevice::deinit(true);  // disconnects, stops advertising, frees the stack
  cast::link().reset();
  remote::link().reset();
  if (running_) LOG_INF("BLE", "Stopped");
  running_ = false;
}

}  // namespace radio
