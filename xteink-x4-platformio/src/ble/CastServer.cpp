#include "CastServer.h"

#include <Logging.h>
#include <NimBLEDevice.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

namespace cast {

CastServer server;

namespace {

// blit/PROTOCOL.md#transport-bluetooth-le-gatt (repo root)
constexpr const char* kServiceUuid = "b1ec0000-5f3a-4e62-9a47-0c3d8e5f2a10";
constexpr const char* kInfoUuid = "b1ec0001-5f3a-4e62-9a47-0c3d8e5f2a10";
constexpr const char* kControlUuid = "b1ec0002-5f3a-4e62-9a47-0c3d8e5f2a10";
constexpr const char* kDataUuid = "b1ec0003-5f3a-4e62-9a47-0c3d8e5f2a10";
constexpr const char* kStatusUuid = "b1ec0004-5f3a-4e62-9a47-0c3d8e5f2a10";

constexpr uint8_t kVersion = 1;
constexpr uint8_t kOpBegin = 0x01, kOpCommit = 0x02, kOpCancel = 0x03;
constexpr uint8_t kEventReady = 1, kEventDone = 2, kEventError = 3, kEventAck = 4;
// Write-without-response has no flow control: a sender that runs ahead
// overflows the buffers on the way and chunks are silently dropped. So the
// device acks after every kAckWindow Data writes (and the last), and senders
// wait for each ack before sending more.
constexpr uint32_t kAckWindow = 16;
constexpr size_t kHeaderBytes = 21;
constexpr uint16_t kMaxSide = 2048;
constexpr uint16_t kPreferredMtu = 517;
constexpr size_t kAttOverhead = 3;   // ATT write opcode + handle
constexpr size_t kOffsetBytes = 4;   // Data write prefix

portMUX_TYPE handoffLock = portMUX_INITIALIZER_UNLOCKED;

NimBLECharacteristic* statusChar = nullptr;

uint16_t le16(const uint8_t* p) { return p[0] | (p[1] << 8); }
uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

constexpr size_t kMaxAttrValue = 512;  // BLE caps any attribute value at 512 bytes

// Payload bytes per Data write: the write must fit both the MTU and the
// attribute limit, less its offset prefix.
uint16_t chunkFor(const uint16_t mtu) {
  return std::min<size_t>(mtu - kAttOverhead, kMaxAttrValue) - kOffsetBytes;
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo& info) override { server.onConnect(info.getConnHandle()); }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
    LOG_INF("BLE", "Disconnected (reason %d)", reason);
    server.onDisconnect();
  }
  void onMTUChange(uint16_t mtu, NimBLEConnInfo&) override { LOG_INF("BLE", "MTU %u", mtu); }
  void onConnParamsUpdate(NimBLEConnInfo& info) override {
    LOG_INF("BLE", "Connection interval %.2f ms", info.getConnInterval() * 1.25f);
  }
};

class InfoCallbacks : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* chr, NimBLEConnInfo& info) override {
    char json[200];
    server.fillInfo(json, sizeof(json), info.getMTU());
    chr->setValue(reinterpret_cast<const uint8_t*>(json), strlen(json));
  }
};

class ControlCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo&) override {
    const auto& v = chr->getValue();
    server.onControl(v.data(), v.length());
  }
};

class DataCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo&) override {
    const auto& v = chr->getValue();
    server.onData(v.data(), v.length());
  }
};

ServerCallbacks serverCallbacks;
InfoCallbacks infoCallbacks;
ControlCallbacks controlCallbacks;
DataCallbacks dataCallbacks;

}  // namespace

// --- Buffer ------------------------------------------------------------------

Buffer::~Buffer() { heap_caps_free(data_); }

bool Buffer::reserve(const size_t bytes) {
  if (bytes <= capacity_) return true;
  heap_caps_free(data_);
  data_ = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (data_ == nullptr) data_ = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_8BIT));
  capacity_ = data_ ? bytes : 0;
  return data_ != nullptr;
}

void Buffer::swap(Buffer& other) {
  std::swap(data_, other.data_);
  std::swap(capacity_, other.capacity_);
}

uint32_t crc32(const uint8_t* data, const size_t length) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < length; i++) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
  }
  return ~crc;
}

// --- Lifecycle (main loop) -----------------------------------------------------

bool CastServer::begin() {
  if (running_) return true;
  if (!NimBLEDevice::init("")) return false;
  const uint8_t* addr = NimBLEDevice::getAddress().getVal();  // little-endian
  snprintf(name_, sizeof(name_), "X4-%02X%02X", addr[1], addr[0]);
  NimBLEDevice::setDeviceName(name_);
  NimBLEDevice::setMTU(kPreferredMtu);

  NimBLEServer* gatt = NimBLEDevice::createServer();
  gatt->setCallbacks(&serverCallbacks, false);
  NimBLEService* service = gatt->createService(kServiceUuid);
  service->createCharacteristic(kInfoUuid, NIMBLE_PROPERTY::READ)->setCallbacks(&infoCallbacks);
  service->createCharacteristic(kControlUuid, NIMBLE_PROPERTY::WRITE)->setCallbacks(&controlCallbacks);
  service->createCharacteristic(kDataUuid, NIMBLE_PROPERTY::WRITE_NR)->setCallbacks(&dataCallbacks);
  statusChar = service->createCharacteristic(kStatusUuid, NIMBLE_PROPERTY::NOTIFY);
  gatt->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(kServiceUuid);
  adv->setName(name_);
  adv->enableScanResponse(true);
  if (!adv->start()) {
    LOG_ERR("BLE", "Advertising failed to start");
    NimBLEDevice::deinit(true);
    return false;
  }
  running_ = true;
  LOG_INF("BLE", "Advertising as %s", name_);
  return true;
}

void CastServer::end() {
  if (!running_) return;
  NimBLEDevice::deinit(true);  // disconnects, stops advertising, frees the stack
  statusChar = nullptr;
  running_ = false;
  connected_ = false;
  receiving_ = false;
  portENTER_CRITICAL(&handoffLock);
  ready_ = false;
  portEXIT_CRITICAL(&handoffLock);
  LOG_INF("BLE", "Stopped");
}

bool CastServer::takeFrame(FrameHeader& header, Buffer& buffer) {
  portENTER_CRITICAL(&handoffLock);
  const bool ready = ready_;
  if (ready) {
    header = rxHeader_;
    rx_.swap(buffer);
    ready_ = false;
  }
  portEXIT_CRITICAL(&handoffLock);
  return ready;
}

void CastServer::notifyDone(const uint32_t sleepSeconds) { notify(kEventDone, 0, sleepSeconds); }

void CastServer::notifyError(const Error code, const uint32_t detail) {
  failed_ = true;
  receiving_ = false;
  notify(kEventError, static_cast<uint8_t>(code), detail);
}

void CastServer::disconnect() {
  if (!running_ || !connected_) return;
  NimBLEDevice::getServer()->disconnect(connHandle_.load());
}

void CastServer::notify(const uint8_t event, const uint8_t code, const uint32_t value) {
  if (statusChar == nullptr || !connected_) return;
  const uint8_t msg[6] = {event, code, static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8),
                          static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)};
  statusChar->notify(msg, sizeof(msg));
}

// --- NimBLE callbacks (BLE task) --------------------------------------------

void CastServer::onConnect(const uint16_t connHandle) {
  // Transfer rate is set by the link: ask for the shortest connection interval
  // Apple hosts accept (15–30 ms; they require max ≥ min + 15 ms, in 1.25 ms
  // units) and full-size link-layer packets (Data Length Extension, else each
  // 508-byte write goes out as ~19 small packets). macOS still picks 30 ms.
  // Don't request the 2M PHY: macOS drops the link (supervision timeout).
  NimBLEServer* gatt = NimBLEDevice::getServer();
  gatt->updateConnParams(connHandle, 12, 24, 0, 400);
  gatt->setDataLen(connHandle, 251);
  connHandle_ = connHandle;
  connected_ = true;
  activity_ = true;
  LOG_INF("BLE", "Connected");
}

void CastServer::onDisconnect() {
  connected_ = false;
  receiving_ = false;
  activity_ = true;
  NimBLEDevice::startAdvertising();
}

void CastServer::fillInfo(char* out, const size_t size, const uint16_t mtu) const {
  snprintf(out, size,
           "{\"v\":%u,\"name\":\"%s\",\"w\":%d,\"h\":%d,\"fullW\":800,\"fullH\":480,\"chunk\":%u,"
           "\"window\":%u,\"maxBytes\":%u,\"formats\":[%u],\"frameSleep\":%s}",
           kVersion, name_, areaW_.load(), areaH_.load(), chunkFor(mtu), static_cast<unsigned>(kAckWindow),
           static_cast<unsigned>(kMaxBytes),
           kFormatRaw1, frameSleep_ ? "true" : "false");
}

void CastServer::onControl(const uint8_t* data, const size_t length) {
  activity_ = true;
  if (length == 0) return notifyError(Error::BadHeader);
  switch (data[0]) {
    case kOpBegin:
      return beginFrame(data, length);
    case kOpCommit:
      return commitFrame();
    case kOpCancel:
      receiving_ = false;
      return;
    default:
      return notifyError(Error::BadHeader, data[0]);
  }
}

void CastServer::beginFrame(const uint8_t* d, const size_t length) {
  receiving_ = false;
  if (length < kHeaderBytes || length < kHeaderBytes + d[20]) return notifyError(Error::BadHeader);

  FrameHeader h;
  h.format = d[2];
  h.flags = d[3];
  h.width = le16(d + 4);
  h.height = le16(d + 6);
  h.byteLength = le32(d + 8);
  h.crc32 = le32(d + 12);
  h.nextFrameSeconds = le32(d + 16);
  const size_t nameLength = std::min<size_t>(d[20], sizeof(h.name) - 1);
  memcpy(h.name, d + kHeaderBytes, nameLength);
  h.name[nameLength] = '\0';

  if (d[1] != kVersion || h.format != kFormatRaw1) return notifyError(Error::Unsupported, h.format);
  if (h.width == 0 || h.height == 0 || h.width > kMaxSide || h.height > kMaxSide) {
    return notifyError(Error::BadHeader);
  }
  const uint32_t expectedBytes = static_cast<uint32_t>((h.width + 7) / 8) * h.height;
  if (h.byteLength != expectedBytes || h.byteLength > kMaxBytes) return notifyError(Error::TooLarge, expectedBytes);

  portENTER_CRITICAL(&handoffLock);
  const bool busy = ready_;
  portEXIT_CRITICAL(&handoffLock);
  if (busy) return notifyError(Error::Busy);
  if (!rx_.reserve(h.byteLength)) return notifyError(Error::TooLarge);

  rxHeader_ = h;
  failed_ = false;
  expected_ = 0;
  unacked_ = 0;
  receiving_ = true;
  notify(kEventReady, 0, chunkFor(NimBLEDevice::getServer()->getPeerMTU(connHandle_.load())));
}

void CastServer::onData(const uint8_t* data, const size_t length) {
  if (!receiving_ || length < kOffsetBytes) return;
  const uint32_t offset = le32(data);
  const size_t n = length - kOffsetBytes;
  if (offset != expected_) {
    receiving_ = false;
    return notifyError(Error::OutOfOrder, expected_);
  }
  if (expected_ + n > rxHeader_.byteLength) {
    receiving_ = false;
    return notifyError(Error::TooLarge, rxHeader_.byteLength);
  }
  memcpy(rx_.data() + expected_, data + kOffsetBytes, n);
  expected_ += n;
  if (++unacked_ >= kAckWindow || expected_ == rxHeader_.byteLength) {
    unacked_ = 0;
    notify(kEventAck, 0, expected_);
  }
}

void CastServer::commitFrame() {
  if (!receiving_) return notifyError(Error::Incomplete, 0);
  receiving_ = false;
  if (expected_ != rxHeader_.byteLength) return notifyError(Error::Incomplete, expected_);
  if (crc32(rx_.data(), rxHeader_.byteLength) != rxHeader_.crc32) return notifyError(Error::BadCrc);
  portENTER_CRITICAL(&handoffLock);
  ready_ = true;
  portEXIT_CRITICAL(&handoffLock);
  LOG_INF("BLE", "Frame %ux%u received (%u bytes)", rxHeader_.width, rxHeader_.height, rxHeader_.byteLength);
}

}  // namespace cast
