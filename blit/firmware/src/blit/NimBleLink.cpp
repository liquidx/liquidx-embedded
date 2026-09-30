#if __has_include(<NimBLEDevice.h>)

#include "NimBleLink.h"

#include <algorithm>

namespace blit {

namespace {

constexpr size_t kAttOverhead = 3;     // ATT opcode + handle
constexpr size_t kMaxAttrValue = 512;  // BLE caps any attribute value at 512 bytes

class InfoCallbacks : public NimBLECharacteristicCallbacks {
 public:
  explicit InfoCallbacks(Receiver& receiver) : receiver_(receiver) {}
  void onRead(NimBLECharacteristic* chr, NimBLEConnInfo&) override {
    uint8_t caps[Receiver::kMaxCaps];
    chr->setValue(caps, receiver_.fillCaps(caps, sizeof(caps)));
  }

 private:
  Receiver& receiver_;
};

class WriteCallbacks : public NimBLECharacteristicCallbacks {
 public:
  WriteCallbacks(Receiver& receiver, const uint8_t channel) : receiver_(receiver), channel_(channel) {}
  void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo&) override {
    const auto& v = chr->getValue();
    if (channel_ == kChannelControl) {
      receiver_.onControl(v.data(), v.length());
    } else {
      receiver_.onData(v.data(), v.length());
    }
  }

 private:
  Receiver& receiver_;
  uint8_t channel_;
};

}  // namespace

void NimBleLink::addService(NimBLEServer* server) {
  // NimBLE keeps pointers to callbacks, so they live as long as the link.
  if (callbacks_[0] == nullptr) {
    callbacks_[0] = new InfoCallbacks(receiver_);
    callbacks_[1] = new WriteCallbacks(receiver_, kChannelControl);
    callbacks_[2] = new WriteCallbacks(receiver_, kChannelData);
  }
  NimBLEService* service = server->createService(kServiceUuid);
  service->createCharacteristic(kInfoUuid, NIMBLE_PROPERTY::READ)->setCallbacks(callbacks_[0]);
  service->createCharacteristic(kControlUuid, NIMBLE_PROPERTY::WRITE)->setCallbacks(callbacks_[1]);
  service->createCharacteristic(kDataUuid, NIMBLE_PROPERTY::WRITE_NR)->setCallbacks(callbacks_[2]);
  status_ = service->createCharacteristic(kStatusUuid, NIMBLE_PROPERTY::NOTIFY);
  event_ = service->createCharacteristic(kEventUuid, NIMBLE_PROPERTY::NOTIFY);
}

void NimBleLink::reset() {
  connected_ = false;
  status_ = event_ = nullptr;
  receiver_.reset();
}

void NimBleLink::onConnect(const uint16_t connHandle) {
  connHandle_ = connHandle;
  connected_ = true;
  receiver_.onConnect(*this);
}

void NimBleLink::onDisconnect() {
  connected_ = false;
  receiver_.onDisconnect();
}

void NimBleLink::send(const uint8_t channel, const uint8_t* data, const size_t length) {
  NimBLECharacteristic* chr = channel == kChannelStatus ? status_ : channel == kChannelEvent ? event_ : nullptr;
  if (chr != nullptr && connected_) chr->notify(data, length);
}

size_t NimBleLink::maxMessage() const {
  NimBLEServer* server = NimBLEDevice::getServer();
  const uint16_t mtu = server != nullptr ? server->getPeerMTU(connHandle_.load()) : 23;
  return std::min<size_t>(mtu - kAttOverhead, kMaxAttrValue);
}

void NimBleLink::disconnect() {
  NimBLEServer* server = NimBLEDevice::getServer();
  if (server != nullptr && connected_) server->disconnect(connHandle_.load());
}

}  // namespace blit

#endif
