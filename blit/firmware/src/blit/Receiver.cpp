#include "Receiver.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace blit {

namespace {

using Guard = std::lock_guard<Lock>;

constexpr size_t kOffsetBytes = 4;     // Data write prefix
constexpr size_t kMaxAttrValue = 512;  // BLE caps any attribute value at 512 bytes
constexpr uint8_t kPowerStep = 5;      // percent, before a power event

}  // namespace

void Receiver::begin(const Config& config) {
  config_ = config;
  areaW_ = config.panelWidth;
  areaH_ = config.panelHeight;
}

// --- Runtime caps (main loop) --------------------------------------------------

// A new area also retires the region base: the host renders the next frame
// at the new size, and must not patch the old one.
void Receiver::setArea(const uint16_t width, const uint16_t height) {
  const bool changed = areaW_.exchange(width) != width;
  if (areaH_.exchange(height) == height && !changed) return;
  {
    Guard guard(lock_);
    base_.link = 0;
  }
  sendCaps();
}

void Receiver::setFormats(const uint8_t* formats, size_t count) {
  count = std::min(count, kMaxFormats);
  bool changed;
  {
    Guard guard(lock_);
    changed = count != formatCount_ || memcmp(formats, formats_, count) != 0;
    memcpy(formats_, formats, count);
    formatCount_ = count;
  }
  if (changed) sendCaps();
}

void Receiver::setFrameSleep(const bool on) {
  if (frameSleep_.exchange(on) != on) sendCaps();
}

void Receiver::setPower(const uint8_t percent, const bool charging, const bool external) {
  battery_ = percent;
  powerFlags_ = (charging ? 0x01 : 0) | (external ? 0x02 : 0);
  const uint8_t sent = sentBattery_;
  const bool moved = (sent == 255 || percent == 255) ? sent != percent : std::abs(percent - sent) >= kPowerStep;
  if (connected_ && (moved || powerFlags_ != sentPowerFlags_)) sendPower();
}

void Receiver::hostName(char* out, const size_t size) const {
  Guard guard(lock_);
  snprintf(out, size, "%s", hostName_);
}

// --- Frames (main loop) --------------------------------------------------------

bool Receiver::takeFrame(FrameHeader& header, Buffer& full, Buffer& region) {
  Guard guard(lock_);
  if (!ready_) return false;
  header = rxHeader_;
  rx_.swap(header.region() ? region : full);
  ready_ = false;
  owed_ = true;
  return true;
}

void Receiver::setRegionBase(const FrameHeader& header) {
  Guard guard(lock_);
  base_.link = header.link;
  base_.format = header.format;
  base_.width = header.width;
  base_.height = header.height;
}

uint32_t Receiver::sleepFor(const FrameHeader& header) const {
  if (!frameSleep_ || header.hold() || header.nextFrameSeconds < config_.minSleepGapSeconds) return 0;
  return header.nextFrameSeconds > config_.wakeMarginSeconds ? header.nextFrameSeconds - config_.wakeMarginSeconds
                                                             : 0;
}

void Receiver::notifyDone(const uint32_t sleepSeconds) {
  owed_ = false;
  notify(kStatusDone, 0, sleepSeconds);
}

void Receiver::notifyError(const Error code, const uint32_t detail) {
  owed_ = false;
  fail(code, detail);
}

// --- Events --------------------------------------------------------------------

void Receiver::sendKey(const Key key, const KeyAction action) {
  if (!wantsKeys_) return;
  uint8_t msg[8] = {kEventKey, static_cast<uint8_t>(key), static_cast<uint8_t>(action), 0};
  put32(msg + 4, config_.now ? config_.now() : 0);
  event(msg, sizeof(msg));
}

void Receiver::sendPointer(const PointerAction action, const uint16_t x, const uint16_t y) {
  if (!wantsPointer_) return;
  uint8_t msg[6] = {kEventPointer, static_cast<uint8_t>(action)};
  put16(put16(msg + 2, x), y);
  event(msg, sizeof(msg));
}

void Receiver::sendPower() {
  sentBattery_ = battery_.load();
  sentPowerFlags_ = powerFlags_.load();
  const uint8_t msg[3] = {kEventPower, sentBattery_, sentPowerFlags_};
  event(msg, sizeof(msg));
}

void Receiver::disconnect() {
  if (connected_ && transport_ != nullptr) transport_->disconnect();
}

void Receiver::notify(const uint8_t event, const uint8_t code, const uint32_t value) {
  if (!connected_ || transport_ == nullptr) return;
  uint8_t msg[6] = {event, code};
  put32(msg + 2, value);
  transport_->send(kChannelStatus, msg, sizeof(msg));
}

// Events go only to a connected host, and are never queued.
void Receiver::event(const uint8_t* data, const size_t length) {
  if (!connected_ || transport_ == nullptr) return;
  transport_->send(kChannelEvent, data, length);
}

// Caps changed (or a hello): the whole caps if they fit in one message,
// otherwise an empty caps event, which tells the host to read Info.
void Receiver::sendCaps() {
  if (!connected_ || transport_ == nullptr) return;
  uint8_t msg[1 + kMaxCaps] = {kEventCaps};
  size_t length = 1 + fillCaps(msg + 1, sizeof(msg) - 1);
  if (length > transport_->maxMessage()) length = 1;
  event(msg, length);
}

// Payload bytes per Data write: the write must fit the link and the attribute
// limit, less its offset prefix.
uint16_t Receiver::chunk() const {
  const size_t message = transport_ != nullptr ? transport_->maxMessage() : 20;
  return std::min(message, kMaxAttrValue) - kOffsetBytes;
}

size_t Receiver::fillCaps(uint8_t* out, const size_t size) const {
  uint8_t caps[kMaxCaps];
  uint8_t* p = caps;
  *p++ = kVersion;
  const auto tag = [&](const uint8_t t, const uint8_t length) -> uint8_t* {
    *p++ = t;
    *p++ = length;
    uint8_t* value = p;
    p += length;
    return value;
  };

  const size_t nameLength = strnlen(config_.name, kMaxHostName);
  if (nameLength > 0) memcpy(tag(kTagName, nameLength), config_.name, nameLength);
  if (config_.panelWidth > 0) put16(put16(tag(kTagPanel, 4), config_.panelWidth), config_.panelHeight);
  put16(put16(tag(kTagArea, 4), areaW_.load()), areaH_.load());
  {
    Guard guard(lock_);
    memcpy(tag(kTagFormats, formatCount_), formats_, formatCount_);
  }
  *tag(kTagEncodings, 1) = kEncodingPackBits;
  put16(put16(put32(tag(kTagLimits, 8), config_.maxBytes), chunk()), config_.window);
  put32(tag(kTagFeatures, 4), config_.features | (frameSleep_ ? kFeatureFrameSleep : 0));
  if (config_.regionAlign > 0) *tag(kTagRegions, 1) = config_.regionAlign;
  const size_t keyCount = std::min<size_t>(config_.keyCount, 16);
  if (keyCount > 0) {
    uint8_t* keys = tag(kTagKeys, keyCount);
    for (size_t i = 0; i < keyCount; i++) keys[i] = static_cast<uint8_t>(config_.keys[i]);
  }
  if (config_.minIntervalMs > 0 || config_.refreshMs > 0) {
    put32(put32(tag(kTagPacing, 8), config_.minIntervalMs), config_.refreshMs);
  }
  uint8_t* power = tag(kTagPower, 2);
  power[0] = battery_;
  power[1] = powerFlags_;

  const size_t length = std::min<size_t>(p - caps, size);
  memcpy(out, caps, length);
  return length;
}

// --- Transport (its task) ------------------------------------------------------

void Receiver::onConnect(Transport& transport) {
  transport_ = &transport;
  link_++;
  wantsKeys_ = false;
  wantsPointer_ = false;
  receiving_ = false;
  // The host reads the battery from Info.
  sentBattery_ = battery_.load();
  sentPowerFlags_ = powerFlags_.load();
  {
    Guard guard(lock_);
    hostName_[0] = '\0';
  }
  connected_ = true;
  activity_ = true;
}

void Receiver::onDisconnect() {
  connected_ = false;
  receiving_ = false;
  activity_ = true;
}

void Receiver::reset() {
  connected_ = false;
  receiving_ = false;
  owed_ = false;  // no one to tell
  Guard guard(lock_);
  ready_ = false;
  base_.link = 0;
}

void Receiver::onControl(const uint8_t* data, const size_t length) {
  activity_ = true;
  if (length == 0) return fail(Error::BadHeader);
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
      return fail(Error::BadHeader, data[0]);
  }
}

void Receiver::hello(const uint8_t* d, const size_t length) {
  if (length < 4) return fail(Error::BadHeader, kOpHello);
  wantsKeys_ = d[2] & kHelloKeys;
  wantsPointer_ = (d[2] & kHelloPointer) && (config_.features & kFeaturePointer);
  const size_t nameLength = std::min<size_t>({d[3], length - 4, kMaxHostName});
  {
    Guard guard(lock_);
    memcpy(hostName_, d + 4, nameLength);
    hostName_[nameLength] = '\0';
  }
  sendCaps();
}

bool Receiver::acceptsFormat(const uint8_t format) const {
  Guard guard(lock_);
  return std::find(formats_, formats_ + formatCount_, format) != formats_ + formatCount_;
}

// A region patches the full frame on screen, so it has to match it: received
// on this connection, the same format, and exactly the current area (after the
// area changes the host re-renders at the new size anyway).
bool Receiver::regionFits(const FrameHeader& h) const {
  decltype(base_) base;
  {
    Guard guard(lock_);
    base = base_;
  }
  const uint32_t areaW = areaW_, areaH = areaH_;
  const uint32_t align = config_.regionAlign;
  const uint32_t right = h.x + h.width;
  return align > 0 && base.link == link_ && base.format == h.format && base.width == areaW &&
         base.height == areaH && right <= areaW && h.y + h.height <= areaH && h.x % align == 0 &&
         (h.x * bitsPerPixel(h.format)) % 8 == 0 && (h.width % align == 0 || right == areaW);
}

void Receiver::beginFrame(const uint8_t* d, const size_t length) {
  receiving_ = false;
  FrameHeader h;
  if (!parseHeader(d, length, h)) return fail(Error::BadHeader);
  h.link = link_;

  if (h.version != kVersion) return fail(Error::Unsupported, h.version);
  if (bitsPerPixel(h.format) == 0 || !acceptsFormat(h.format)) return fail(Error::Unsupported, h.format);
  if (h.encoding != kEncodingNone && h.encoding != kEncodingPackBits) {
    return fail(Error::Unsupported, h.encoding);
  }
  if (h.width == 0 || h.height == 0 || h.width > config_.maxSide || h.height > config_.maxSide) {
    return fail(Error::BadHeader);
  }
  // Both the payload as sent and the frame it decodes to must fit.
  const uint32_t frameBytes = h.frameBytes();
  if (h.byteLength > config_.maxBytes || frameBytes > config_.maxBytes ||
      (h.encoding == kEncodingNone && h.byteLength != frameBytes)) {
    return fail(Error::TooLarge, frameBytes);
  }
  if (h.region() && !regionFits(h)) return fail(Error::BadRegion);

  bool busy;
  {
    Guard guard(lock_);
    busy = ready_;
  }
  if (busy) return fail(Error::Busy);
  if (!rx_.reserve(frameBytes)) return fail(Error::TooLarge, frameBytes);

  rxHeader_ = h;
  failed_ = false;
  received_ = decoded_ = crc_ = unacked_ = 0;
  literal_ = 0;
  repeat_ = 0;
  receiving_ = true;
  notify(kStatusReady, 0, chunk());
}

// The transfer is over (any error ends it). Unlike notifyError(), leaves a
// taken frame's reply owed: the host broke the rules, not the frame on screen.
void Receiver::fail(const Error code, const uint32_t detail) {
  receiving_ = false;
  failed_ = true;
  notify(kStatusError, static_cast<uint8_t>(code), detail);
}

void Receiver::onData(const uint8_t* data, const size_t length) {
  if (!receiving_ || length < kOffsetBytes) return;
  const uint32_t offset = get32(data);
  const size_t n = length - kOffsetBytes;
  if (offset != received_) return fail(Error::OutOfOrder, received_);
  if (received_ + n > rxHeader_.byteLength) return fail(Error::TooLarge, rxHeader_.byteLength);
  crc_ = crc32(data + kOffsetBytes, n, crc_);
  decode(data + kOffsetBytes, n);
  received_ += n;
  if (++unacked_ >= config_.window || received_ == rxHeader_.byteLength) {
    unacked_ = 0;
    notify(kStatusAck, 0, received_);
  }
}

// Straight into the frame buffer as bytes arrive. PackBits runs may span
// Data writes, so the decoder's state lives between calls.
void Receiver::decode(const uint8_t* data, const size_t length) {
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
void Receiver::put(const uint8_t byte, const uint32_t count) {
  const uint32_t size = rxHeader_.frameBytes();
  if (decoded_ < size) memset(rx_.data() + decoded_, byte, std::min(count, size - decoded_));
  decoded_ += count;
}

void Receiver::commitFrame() {
  if (!receiving_) return fail(Error::Incomplete, 0);
  receiving_ = false;
  if (received_ != rxHeader_.byteLength) return fail(Error::Incomplete, received_);
  if (crc_ != rxHeader_.crc32) return fail(Error::BadCrc);
  if (decoded_ != rxHeader_.frameBytes()) return fail(Error::Decode, decoded_);
  Guard guard(lock_);
  ready_ = true;
}

}  // namespace blit
