#pragma once

#if __has_include(<NimBLEDevice.h>)

#include <NimBLEDevice.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "Device.h"

namespace blat {

// The blat GATT service (PROTOCOL.md#transport-bluetooth-le-gatt) on a
// NimBLE server, feeding a Device. NimBLE calls back on its own task, which
// only queues what arrives; poll() on the main loop hands it to the Device,
// which is where all protocol work (and all the crypto) happens.
//
// The firmware owns the NimBLE server: it adds this service before starting
// it, and forwards connects and disconnects from its server callbacks.
class NimBleLink : public Transport {
 public:
  explicit NimBleLink(Device& device) : device_(device) {}

  // Before server->start().
  void addService(NimBLEServer* server);
  // From NimBLEServerCallbacks (BLE task).
  void onConnect(uint16_t connHandle);
  void onDisconnect();
  // After NimBLEDevice::deinit(): the characteristics are gone.
  void reset();

  // Main loop.
  void poll();

  // Transport, used by the Device from the main loop.
  void send(uint8_t channel, const uint8_t* data, size_t length) override;
  size_t maxMessage() const override;

  // NimBLE callbacks (BLE task).
  void onWrite(uint8_t channel, const uint8_t* data, size_t length);
  size_t readInfo(uint8_t* out, size_t size);

 private:
  static constexpr size_t kQueueBytes = 4096;
  static constexpr uint8_t kConnect = 0xF0;
  static constexpr uint8_t kDisconnect = 0xF1;

  bool push(uint8_t kind, const uint8_t* data, size_t length);
  bool pop(uint8_t& kind, uint8_t* data, size_t& length);

  Device& device_;
  NimBLECharacteristicCallbacks* callbacks_[3] = {};  // Info, Request, Data
  NimBLECharacteristic* reply_ = nullptr;
  NimBLECharacteristic* dataOut_ = nullptr;
  NimBLECharacteristic* event_ = nullptr;
  std::atomic<uint16_t> connHandle_{0};
  bool connected_ = false;  // main loop's view

  // BLE task → main loop, under lock_: [kind, length lo, length hi, bytes...]
  portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
  uint8_t queue_[kQueueBytes];
  uint8_t rxBuffer_[512];  // poll(): one message out of the queue
  size_t head_ = 0;  // next byte to read
  size_t used_ = 0;

  // Info, rebuilt by poll() and read by the BLE task, under lock_.
  uint8_t info_[160];
  size_t infoLength_ = 0;
};

}  // namespace blat

#endif
