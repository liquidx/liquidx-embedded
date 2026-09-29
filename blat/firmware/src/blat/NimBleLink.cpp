#if __has_include(<NimBLEDevice.h>)

#include "NimBleLink.h"

#include <algorithm>
#include <cstring>

namespace blat {

namespace {

constexpr size_t kAttOverhead = 3;
constexpr size_t kMaxAttrValue = 512;
constexpr int kNotifyTries = 40;
constexpr uint32_t kNotifyRetryMs = 5;

class InfoCallbacks : public NimBLECharacteristicCallbacks {
 public:
  explicit InfoCallbacks(NimBleLink& link) : link_(link) {}
  void onRead(NimBLECharacteristic* chr, NimBLEConnInfo&) override {
    uint8_t info[160];
    chr->setValue(info, link_.readInfo(info, sizeof(info)));
  }

 private:
  NimBleLink& link_;
};

class WriteCallbacks : public NimBLECharacteristicCallbacks {
 public:
  WriteCallbacks(NimBleLink& link, const uint8_t channel) : link_(link), channel_(channel) {}
  void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo&) override {
    const auto& v = chr->getValue();
    link_.onWrite(channel_, v.data(), v.length());
  }

 private:
  NimBleLink& link_;
  uint8_t channel_;
};

}  // namespace

void NimBleLink::addService(NimBLEServer* server) {
  // NimBLE keeps pointers to callbacks, so they live as long as the link.
  if (callbacks_[0] == nullptr) {
    callbacks_[0] = new InfoCallbacks(*this);
    callbacks_[1] = new WriteCallbacks(*this, kChannelRequest);
    callbacks_[2] = new WriteCallbacks(*this, kChannelData);
  }

  NimBLEService* service = server->createService(kServiceUuid);
  service->createCharacteristic(kInfoUuid, NIMBLE_PROPERTY::READ)->setCallbacks(callbacks_[0]);
  service->createCharacteristic(kRequestUuid, NIMBLE_PROPERTY::WRITE)->setCallbacks(callbacks_[1]);
  reply_ = service->createCharacteristic(kReplyUuid, NIMBLE_PROPERTY::NOTIFY);
  service->createCharacteristic(kDataUuid, NIMBLE_PROPERTY::WRITE_NR)->setCallbacks(callbacks_[2]);
  dataOut_ = service->createCharacteristic(kDataOutUuid, NIMBLE_PROPERTY::NOTIFY);
  event_ = service->createCharacteristic(kEventUuid, NIMBLE_PROPERTY::NOTIFY);
  poll();  // fill Info
}

void NimBleLink::reset() {
  reply_ = dataOut_ = event_ = nullptr;
  portENTER_CRITICAL(&lock_);
  head_ = used_ = 0;
  portEXIT_CRITICAL(&lock_);
  if (connected_) device_.disconnected();
  connected_ = false;
}

void NimBleLink::onConnect(const uint16_t connHandle) {
  connHandle_ = connHandle;
  push(kConnect, nullptr, 0);
}

void NimBleLink::onDisconnect() { push(kDisconnect, nullptr, 0); }

void NimBleLink::onWrite(const uint8_t channel, const uint8_t* data, const size_t length) {
  push(channel, data, length);
}

size_t NimBleLink::readInfo(uint8_t* out, const size_t size) {
  portENTER_CRITICAL(&lock_);
  const size_t n = std::min(infoLength_, size);
  memcpy(out, info_, n);
  portEXIT_CRITICAL(&lock_);
  return n;
}

// A full queue drops the message: the host times out and tries again.
bool NimBleLink::push(const uint8_t kind, const uint8_t* data, const size_t length) {
  const size_t need = 3 + length;
  bool ok = false;
  portENTER_CRITICAL(&lock_);
  if (used_ + need <= kQueueBytes) {
    const uint8_t header[3] = {kind, static_cast<uint8_t>(length), static_cast<uint8_t>(length >> 8)};
    size_t tail = (head_ + used_) % kQueueBytes;
    for (size_t i = 0; i < need; i++) {
      queue_[tail] = i < 3 ? header[i] : data[i - 3];
      tail = (tail + 1) % kQueueBytes;
    }
    used_ += need;
    ok = true;
  }
  portEXIT_CRITICAL(&lock_);
  return ok;
}

bool NimBleLink::pop(uint8_t& kind, uint8_t* data, size_t& length) {
  bool ok = false;
  portENTER_CRITICAL(&lock_);
  if (used_ >= 3) {
    kind = queue_[head_];
    length = queue_[(head_ + 1) % kQueueBytes] | (queue_[(head_ + 2) % kQueueBytes] << 8);
    for (size_t i = 0; i < length; i++) data[i] = queue_[(head_ + 3 + i) % kQueueBytes];
    head_ = (head_ + 3 + length) % kQueueBytes;
    used_ -= 3 + length;
    ok = true;
  }
  portEXIT_CRITICAL(&lock_);
  return ok;
}

void NimBleLink::poll() {
  uint8_t kind;
  uint8_t* data = rxBuffer_;
  size_t length;
  while (pop(kind, data, length)) {
    if (kind == kConnect) {
      connected_ = true;
      device_.connected(*this);
    } else if (kind == kDisconnect) {
      connected_ = false;
      device_.disconnected();
    } else if (connected_) {
      device_.receive(kind, data, length);
    }
  }
  device_.poll();

  uint8_t info[sizeof(info_)];
  const size_t n = device_.info(info, sizeof(info));
  portENTER_CRITICAL(&lock_);
  memcpy(info_, info, n);
  infoLength_ = n;
  portEXIT_CRITICAL(&lock_);
}

void NimBleLink::send(const uint8_t channel, const uint8_t* data, const size_t length) {
  NimBLECharacteristic* chr = channel == kChannelReply     ? reply_
                              : channel == kChannelDataOut ? dataOut_
                              : channel == kChannelEvent   ? event_
                                                           : nullptr;
  if (chr == nullptr || !connected_) return;
  // notify() fails when NimBLE is out of buffers (a few KB on ESP32, shared
  // with everything else): wait for sent packets to free some, rather than
  // drop a reply or a chunk the host would then wait for.
  for (int tries = 0; !chr->notify(data, length) && tries < kNotifyTries; tries++) {
    if (!connected_) return;
    vTaskDelay(pdMS_TO_TICKS(kNotifyRetryMs));
  }
}

size_t NimBleLink::maxMessage() const {
  NimBLEServer* server = NimBLEDevice::getServer();
  const uint16_t mtu = server != nullptr ? server->getPeerMTU(connHandle_.load()) : 23;
  return std::min<size_t>(mtu - kAttOverhead, kMaxAttrValue);
}

}  // namespace blat

#endif
