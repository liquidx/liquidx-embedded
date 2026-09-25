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
constexpr const char* kEventUuid = "b1ec0005-5f3a-4e62-9a47-0c3d8e5f2a10";

constexpr uint8_t kVersion = 2;
constexpr uint8_t kOpBegin = 0x01, kOpCommit = 0x02, kOpCancel = 0x03, kOpHello = 0x04;
constexpr uint8_t kStatusReady = 1, kStatusDone = 2, kStatusError = 3, kStatusAck = 4;
constexpr uint8_t kEventCaps = 0x01, kEventKey = 0x02, kEventPower = 0x04;
constexpr uint8_t kKeyPress = 1;
constexpr uint8_t kHelloKeys = 0x01;

// Caps TLV tags and feature bits (blit/PROTOCOL.md#caps).
constexpr uint8_t kTagName = 0x01, kTagPanel = 0x02, kTagArea = 0x03, kTagFormats = 0x04, kTagEncodings = 0x05,
                  kTagLimits = 0x06, kTagFeatures = 0x07, kTagRegions = 0x08, kTagKeys = 0x09, kTagPacing = 0x0A,
                  kTagPower = 0x0B;
constexpr uint32_t kFeaturePersist = 1 << 0, kFeatureFrameSleep = 1 << 1, kFeatureFastRefresh = 1 << 2;

// Regions start on a byte in every format we take (gray2 needs x % 4).
constexpr uint8_t kRegionAlign = 8;
constexpr uint8_t kKeys[] = {static_cast<uint8_t>(Key::Up), static_cast<uint8_t>(Key::Down),
                             static_cast<uint8_t>(Key::Select)};
constexpr uint32_t kRefreshMs = 1500;  // a half refresh, about

// Write-without-response has no flow control: a sender that runs ahead
// overflows the buffers on the way and chunks are silently dropped. So the
// device acks after every kAckWindow Data writes (and the last), and senders
// wait for each ack before sending more.
constexpr uint32_t kAckWindow = 16;
constexpr size_t kV1HeaderBytes = 21;
constexpr size_t kV2HeaderBytes = 29;
constexpr uint16_t kMaxSide = 2048;
constexpr uint16_t kPanelW = 800, kPanelH = 480;
constexpr uint16_t kPreferredMtu = 517;
constexpr size_t kAttOverhead = 3;   // ATT write opcode + handle
constexpr size_t kOffsetBytes = 4;   // Data write prefix

portMUX_TYPE handoffLock = portMUX_INITIALIZER_UNLOCKED;

NimBLECharacteristic* statusChar = nullptr;
NimBLECharacteristic* eventChar = nullptr;

uint16_t le16(const uint8_t* p) { return p[0] | (p[1] << 8); }
uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

uint8_t* put16(uint8_t* p, const uint16_t v) {
  p[0] = v;
  p[1] = v >> 8;
  return p + 2;
}
uint8_t* put32(uint8_t* p, const uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = v >> (8 * i);
  return p + 4;
}

constexpr size_t kMaxAttrValue = 512;  // BLE caps any attribute value at 512 bytes

// Payload bytes per Data write: the write must fit both the MTU and the
// attribute limit, less its offset prefix.
uint16_t chunkFor(const uint16_t mtu) {
  return std::min<size_t>(mtu - kAttOverhead, kMaxAttrValue) - kOffsetBytes;
}

uint16_t peerMtu(const uint16_t connHandle) { return NimBLEDevice::getServer()->getPeerMTU(connHandle); }

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
    uint8_t caps[128];
    chr->setValue(caps, server.fillCaps(caps, sizeof(caps), info.getMTU()));
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

uint32_t crc32(const uint8_t* data, const size_t length, uint32_t crc) {
  crc = ~crc;
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
  eventChar = service->createCharacteristic(kEventUuid, NIMBLE_PROPERTY::NOTIFY);
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
  eventChar = nullptr;
  running_ = false;
  connected_ = false;
  receiving_ = false;
  portENTER_CRITICAL(&handoffLock);
  ready_ = false;
  base_.link = 0;
  portEXIT_CRITICAL(&handoffLock);
  LOG_INF("BLE", "Stopped");
}

void CastServer::hostName(char* out, const size_t size) const {
  portENTER_CRITICAL(&handoffLock);
  snprintf(out, size, "%s", hostName_);
  portEXIT_CRITICAL(&handoffLock);
}

void CastServer::setArea(const int width, const int height) {
  const bool changed = areaW_.exchange(width) != width;
  if ((areaH_.exchange(height) != height || changed) && connected_) sendCaps();
}

void CastServer::setBattery(const uint8_t percent, const bool usb) {
  battery_ = percent;
  usb_ = usb;
}

void CastServer::setRegionBase(const FrameHeader& header) {
  portENTER_CRITICAL(&handoffLock);
  base_.link = header.link;
  base_.format = header.format;
  base_.width = header.width;
  base_.height = header.height;
  portEXIT_CRITICAL(&handoffLock);
}

bool CastServer::takeFrame(FrameHeader& header, Buffer& full, Buffer& region) {
  portENTER_CRITICAL(&handoffLock);
  const bool ready = ready_;
  if (ready) {
    header = rxHeader_;
    rx_.swap(header.region() ? region : full);
    ready_ = false;
  }
  portEXIT_CRITICAL(&handoffLock);
  return ready;
}

void CastServer::notifyDone(const uint32_t sleepSeconds) { notify(kStatusDone, 0, sleepSeconds); }

void CastServer::notifyError(const Error code, const uint32_t detail) {
  failed_ = true;
  receiving_ = false;
  notify(kStatusError, static_cast<uint8_t>(code), detail);
}

void CastServer::sendKey(const Key key) {
  if (eventChar == nullptr || !connected_ || !wantsKeys_) return;
  uint8_t msg[8] = {kEventKey, static_cast<uint8_t>(key), kKeyPress, 0};
  put32(msg + 4, millis());
  eventChar->notify(msg, sizeof(msg));
}

void CastServer::sendPower() {
  if (eventChar == nullptr || !connected_) return;
  const uint8_t msg[3] = {kEventPower, battery_.load(), static_cast<uint8_t>(usb_ ? 0x02 : 0)};
  eventChar->notify(msg, sizeof(msg));
}

void CastServer::disconnect() {
  if (!running_ || !connected_) return;
  NimBLEDevice::getServer()->disconnect(connHandle_.load());
}

void CastServer::notify(const uint8_t event, const uint8_t code, const uint32_t value) {
  if (statusChar == nullptr || !connected_) return;
  uint8_t msg[6] = {event, code};
  put32(msg + 2, value);
  statusChar->notify(msg, sizeof(msg));
}

// Caps changed (or a hello): the whole caps if they fit in a notification,
// otherwise an empty caps event, which tells the host to read Info.
void CastServer::sendCaps() {
  if (eventChar == nullptr || !connected_) return;
  const uint16_t mtu = peerMtu(connHandle_.load());
  uint8_t msg[129] = {kEventCaps};
  size_t length = 1 + fillCaps(msg + 1, sizeof(msg) - 1, mtu);
  if (length > static_cast<size_t>(mtu - kAttOverhead)) length = 1;
  eventChar->notify(msg, length);
}

size_t CastServer::fillCaps(uint8_t* out, const size_t size, const uint16_t mtu) const {
  uint8_t caps[128];
  uint8_t* p = caps;
  *p++ = kVersion;
  const auto tag = [&](const uint8_t t, const uint8_t length) -> uint8_t* {
    *p++ = t;
    *p++ = length;
    return p;
  };

  const size_t nameLength = strlen(name_);
  memcpy(tag(kTagName, nameLength), name_, nameLength);
  p += nameLength;
  put16(put16(tag(kTagPanel, 4), kPanelW), kPanelH);
  p += 4;
  put16(put16(tag(kTagArea, 4), areaW_.load()), areaH_.load());
  p += 4;
  // mono1 first: it refreshes fast and keeps the gutter usable. Hosts that
  // want greys pick gray2 explicitly.
  const bool gray = grayscale_;
  uint8_t* formats = tag(kTagFormats, gray ? 2 : 1);
  formats[0] = kFormatMono1;
  if (gray) formats[1] = kFormatGray2;
  p += gray ? 2 : 1;
  *tag(kTagEncodings, 1) = kEncodingPackBits;
  p += 1;
  put16(put16(put32(tag(kTagLimits, 8), kMaxBytes), chunkFor(mtu)), kAckWindow);
  p += 8;
  put32(tag(kTagFeatures, 4), kFeaturePersist | kFeatureFastRefresh | (frameSleep_ ? kFeatureFrameSleep : 0));
  p += 4;
  *tag(kTagRegions, 1) = kRegionAlign;
  p += 1;
  memcpy(tag(kTagKeys, sizeof(kKeys)), kKeys, sizeof(kKeys));
  p += sizeof(kKeys);
  put32(put32(tag(kTagPacing, 8), 0), kRefreshMs);
  p += 8;
  uint8_t* power = tag(kTagPower, 2);
  power[0] = battery_;
  power[1] = usb_ ? 0x02 : 0;
  p += 2;

  const size_t length = std::min<size_t>(p - caps, size);
  memcpy(out, caps, length);
  return length;
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
  link_++;
  wantsKeys_ = false;
  portENTER_CRITICAL(&handoffLock);
  hostName_[0] = '\0';
  portEXIT_CRITICAL(&handoffLock);
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
    case kOpHello:
      return hello(data, length);
    default:
      return notifyError(Error::BadHeader, data[0]);
  }
}

void CastServer::hello(const uint8_t* d, const size_t length) {
  if (length < 4) return notifyError(Error::BadHeader, kOpHello);
  wantsKeys_ = d[2] & kHelloKeys;
  const size_t nameLength = std::min<size_t>({d[3], length - 4, sizeof(hostName_) - 1});
  portENTER_CRITICAL(&handoffLock);
  memcpy(hostName_, d + 4, nameLength);
  hostName_[nameLength] = '\0';
  portEXIT_CRITICAL(&handoffLock);
  LOG_INF("BLE", "Hello from %.*s (v%u)", static_cast<int>(nameLength), d + 4, d[1]);
  sendCaps();
}

bool CastServer::parseHeader(const uint8_t* d, const size_t length, FrameHeader& h) {
  size_t nameAt, nameLength;
  if (length >= 2 && d[1] == 1) {
    if (length < kV1HeaderBytes || length < kV1HeaderBytes + d[20]) return false;
    h.version = 1;
    h.format = d[2];
    h.flags = d[3] & kFlagPersist;
    h.width = le16(d + 4);
    h.height = le16(d + 6);
    h.byteLength = le32(d + 8);
    h.crc32 = le32(d + 12);
    h.nextFrameSeconds = le32(d + 16);
    nameAt = kV1HeaderBytes;
    nameLength = d[20];
  } else {
    if (length < kV2HeaderBytes || length < kV2HeaderBytes + d[28]) return false;
    h.version = d[1];
    h.format = d[2];
    h.encoding = d[3];
    h.flags = d[4];
    h.refresh = d[5] <= 2 ? static_cast<Refresh>(d[5]) : Refresh::Auto;
    h.width = le16(d + 6);
    h.height = le16(d + 8);
    if (h.region()) {
      h.x = le16(d + 10);
      h.y = le16(d + 12);
    }
    h.byteLength = le32(d + 16);
    h.crc32 = le32(d + 20);
    h.nextFrameSeconds = le32(d + 24);
    nameAt = kV2HeaderBytes;
    nameLength = d[28];
  }
  nameLength = std::min(nameLength, sizeof(h.name) - 1);
  memcpy(h.name, d + nameAt, nameLength);
  h.name[nameLength] = '\0';
  return true;
}

// A region patches the full frame on screen, so it has to match it: received
// on this connection, the same format, and exactly the current area (after the
// area changes the host re-renders at the new size anyway).
bool CastServer::regionFits(const FrameHeader& h) const {
  portENTER_CRITICAL(&handoffLock);
  const auto base = base_;
  portEXIT_CRITICAL(&handoffLock);
  const int areaW = areaW_, areaH = areaH_;
  const uint32_t right = h.x + h.width;
  return base.link == link_ && base.format == h.format && base.width == areaW && base.height == areaH &&
         right <= static_cast<uint32_t>(areaW) && h.y + h.height <= areaH && h.x % kRegionAlign == 0 &&
         (h.width % kRegionAlign == 0 || right == static_cast<uint32_t>(areaW));
}

void CastServer::beginFrame(const uint8_t* d, const size_t length) {
  receiving_ = false;
  FrameHeader h;
  if (!parseHeader(d, length, h)) return notifyError(Error::BadHeader);
  h.link = link_;

  if (h.version < 1 || h.version > kVersion) return notifyError(Error::Unsupported, h.version);
  if (h.format != kFormatMono1 && !(h.format == kFormatGray2 && grayscale_)) {
    return notifyError(Error::Unsupported, h.format);
  }
  if (h.encoding != kEncodingNone && h.encoding != kEncodingPackBits) {
    return notifyError(Error::Unsupported, h.encoding);
  }
  if (h.width == 0 || h.height == 0 || h.width > kMaxSide || h.height > kMaxSide) {
    return notifyError(Error::BadHeader);
  }
  // Both the payload as sent and the frame it decodes to must fit.
  const uint32_t frameBytes = h.frameBytes();
  if (h.byteLength > kMaxBytes || frameBytes > kMaxBytes ||
      (h.encoding == kEncodingNone && h.byteLength != frameBytes)) {
    return notifyError(Error::TooLarge, frameBytes);
  }
  if (h.region() && !regionFits(h)) return notifyError(Error::BadRegion);

  portENTER_CRITICAL(&handoffLock);
  const bool busy = ready_;
  portEXIT_CRITICAL(&handoffLock);
  if (busy) return notifyError(Error::Busy);
  if (!rx_.reserve(frameBytes)) return notifyError(Error::TooLarge, frameBytes);

  rxHeader_ = h;
  failed_ = false;
  received_ = decoded_ = crc_ = unacked_ = 0;
  literal_ = 0;
  repeat_ = 0;
  receiving_ = true;
  notify(kStatusReady, 0, chunkFor(peerMtu(connHandle_.load())));
}

void CastServer::fail(const Error code, const uint32_t detail) {
  receiving_ = false;
  notifyError(code, detail);
}

void CastServer::onData(const uint8_t* data, const size_t length) {
  if (!receiving_ || length < kOffsetBytes) return;
  const uint32_t offset = le32(data);
  const size_t n = length - kOffsetBytes;
  if (offset != received_) return fail(Error::OutOfOrder, received_);
  if (received_ + n > rxHeader_.byteLength) return fail(Error::TooLarge, rxHeader_.byteLength);
  crc_ = crc32(data + kOffsetBytes, n, crc_);
  decode(data + kOffsetBytes, n);
  received_ += n;
  if (++unacked_ >= kAckWindow || received_ == rxHeader_.byteLength) {
    unacked_ = 0;
    notify(kStatusAck, 0, received_);
  }
}

// Straight into the frame buffer as bytes arrive. PackBits runs may span
// Data writes, so the decoder's state lives between calls.
void CastServer::decode(const uint8_t* data, const size_t length) {
  if (rxHeader_.encoding == kEncodingNone) {
    memcpy(rx_.data() + decoded_, data, length);
    decoded_ += length;
    return;
  }
  for (size_t i = 0; i < length; i++) {
    const uint8_t b = data[i];
    if (literal_ > 0) {
      put(b, 1);
      literal_--;
    } else if (repeat_ > 0) {
      put(b, repeat_);
      repeat_ = 0;
    } else if (b < 0x80) {
      literal_ = b + 1;
    } else if (b > 0x80) {
      repeat_ = 257 - b;
    }  // 0x80: no-op
  }
}

// Write `count` copies of `byte`, dropping (but counting) any past the end.
void CastServer::put(const uint8_t byte, const uint32_t count) {
  const uint32_t size = rxHeader_.frameBytes();
  if (decoded_ < size) memset(rx_.data() + decoded_, byte, std::min(count, size - decoded_));
  decoded_ += count;
}

void CastServer::commitFrame() {
  if (!receiving_) return notifyError(Error::Incomplete, 0);
  receiving_ = false;
  if (received_ != rxHeader_.byteLength) return notifyError(Error::Incomplete, received_);
  if (crc_ != rxHeader_.crc32) return notifyError(Error::BadCrc);
  if (decoded_ != rxHeader_.frameBytes()) return notifyError(Error::Decode, decoded_);
  portENTER_CRITICAL(&handoffLock);
  ready_ = true;
  portEXIT_CRITICAL(&handoffLock);
  LOG_INF("BLE", "%s %ux%u received (%u bytes%s)", rxHeader_.region() ? "Region" : "Frame", rxHeader_.width,
          rxHeader_.height, rxHeader_.byteLength, rxHeader_.encoding ? ", packbits" : "");
}

}  // namespace cast
