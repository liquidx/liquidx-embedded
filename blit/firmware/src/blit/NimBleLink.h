#pragma once

#if __has_include(<NimBLEDevice.h>)

#include <NimBLEDevice.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "Receiver.h"

namespace blit {

// The blit GATT service (PROTOCOL.md#transport-bluetooth-le-gatt) on a NimBLE
// server, feeding a Receiver. Unlike blat's link, writes go to the Receiver
// straight from NimBLE's task: frames are the bulk of the traffic, and
// decoding them as they arrive means no queue and no second copy.
//
// The firmware owns the NimBLE server: it adds this service before starting
// it, advertises kServiceUuid, and forwards connects and disconnects from its
// server callbacks.
class NimBleLink : public Transport {
 public:
  explicit NimBleLink(Receiver& receiver) : receiver_(receiver) {}

  // Before server->start().
  void addService(NimBLEServer* server);
  // From NimBLEServerCallbacks (BLE task).
  void onConnect(uint16_t connHandle);
  void onDisconnect();
  // After NimBLEDevice::deinit(): the characteristics are gone.
  void reset();

  // Transport.
  void send(uint8_t channel, const uint8_t* data, size_t length) override;
  size_t maxMessage() const override;
  void disconnect() override;

 private:
  Receiver& receiver_;
  NimBLECharacteristicCallbacks* callbacks_[3] = {};  // Info, Control, Data
  NimBLECharacteristic* status_ = nullptr;
  NimBLECharacteristic* event_ = nullptr;
  std::atomic<uint16_t> connHandle_{0};
  std::atomic<bool> connected_{false};
};

}  // namespace blit

#endif
