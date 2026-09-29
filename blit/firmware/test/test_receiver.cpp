// Unit tests for the Receiver, frame helpers and BMP codec. No host needed:
//   make -C blit/firmware/test

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "blit.h"

namespace {

int failures = 0;

#define EXPECT(cond)                                                     \
  do {                                                                   \
    if (!(cond)) {                                                       \
      fprintf(stderr, "%s:%d: FAILED: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                        \
    }                                                                    \
  } while (0)

using Bytes = std::vector<uint8_t>;

struct Message {
  uint8_t channel;
  Bytes bytes;
};

class FakeTransport : public blit::Transport {
 public:
  size_t max = 244;
  std::vector<Message> sent;
  bool dropped = false;

  void send(const uint8_t channel, const uint8_t* data, const size_t length) override {
    sent.push_back({channel, Bytes(data, data + length)});
  }
  size_t maxMessage() const override { return max; }
  void disconnect() override { dropped = true; }

  // The oldest message not yet looked at, or an empty one.
  Message next() {
    if (sent.empty()) return {0, {}};
    Message m = sent.front();
    sent.erase(sent.begin());
    return m;
  }
};

struct Status {
  uint8_t event = 0, code = 0;
  uint32_t value = 0;
};

Status status(const Message& m) {
  Status s;
  if (m.channel == blit::kChannelStatus && m.bytes.size() == 6) {
    s.event = m.bytes[0];
    s.code = m.bytes[1];
    s.value = blit::get32(m.bytes.data() + 2);
  }
  return s;
}

bool isError(const Message& m, const blit::Error code) {
  const Status s = status(m);
  return s.event == blit::kStatusError && s.code == static_cast<uint8_t>(code);
}

constexpr blit::Key kKeys[] = {blit::Key::Up, blit::Key::Down, blit::Key::Select};

// A connected receiver: 64 × 32 area, mono1 then gray2, regions on 8.
struct Rig {
  blit::Receiver receiver;
  FakeTransport transport;
  blit::Buffer frame, region;

  explicit Rig(const uint32_t features = blit::kFeaturePersist) {
    blit::Config config;
    config.name = "Test";
    config.panelWidth = 64;
    config.panelHeight = 32;
    config.maxBytes = 4096;
    config.window = 4;
    config.features = features;
    config.regionAlign = 8;
    config.keys = kKeys;
    config.keyCount = 3;
    config.refreshMs = 1500;
    config.now = [] { return 1234u; };
    receiver.begin(config);
    const uint8_t formats[] = {blit::kFormatMono1, blit::kFormatGray2};
    receiver.setFormats(formats, 2);
    receiver.onConnect(transport);
  }

  void control(const Bytes& bytes) { receiver.onControl(bytes.data(), bytes.size()); }

  void data(const uint32_t offset, const uint8_t* bytes, const size_t length) {
    Bytes write(4 + length);
    blit::put32(write.data(), offset);
    memcpy(write.data() + 4, bytes, length);
    receiver.onData(write.data(), write.size());
  }
};

Bytes beginMessage(blit::FrameHeader h) {
  if (h.version == 0) h.version = blit::kVersion;
  Bytes out(blit::kHeaderBytes + blit::kMaxName);
  out.resize(blit::encodeHeader(h, out.data()));
  return out;
}

blit::FrameHeader headerFor(const uint8_t format, const uint16_t w, const uint16_t h, const Bytes& payload,
                            const uint8_t encoding = blit::kEncodingNone) {
  blit::FrameHeader header;
  header.format = format;
  header.encoding = encoding;
  header.width = w;
  header.height = h;
  header.byteLength = payload.size();
  header.crc32 = blit::crc32(payload.data(), payload.size());
  return header;
}

// Plain PackBits, written from the spec (not shared with the library).
Bytes packbits(const Bytes& in) {
  Bytes out;
  size_t i = 0;
  while (i < in.size()) {
    size_t run = 1;
    while (i + run < in.size() && run < 128 && in[i + run] == in[i]) run++;
    if (run >= 3) {
      out.push_back(static_cast<uint8_t>(257 - run));
      out.push_back(in[i]);
      i += run;
      continue;
    }
    const size_t start = i;
    while (i < in.size() && i - start < 128 &&
           !(i + 2 < in.size() && in[i] == in[i + 1] && in[i] == in[i + 2])) {
      i++;
    }
    out.push_back(static_cast<uint8_t>(i - start - 1));
    out.insert(out.end(), in.begin() + start, in.begin() + i);
  }
  return out;
}

// Begin, send `payload` in `chunk`-sized writes and commit. Returns the
// statuses in between (ready, acks, then whatever commit said).
std::vector<Status> send(Rig& rig, const blit::FrameHeader& header, const Bytes& payload, const size_t chunk = 16) {
  std::vector<Status> out;
  rig.control(beginMessage(header));
  for (size_t at = 0; at < payload.size(); at += chunk) {
    rig.data(at, payload.data() + at, std::min(chunk, payload.size() - at));
  }
  rig.control({blit::kOpCommit});
  while (!rig.transport.sent.empty()) out.push_back(status(rig.transport.next()));
  return out;
}

Bytes pattern(const size_t length, const uint8_t seed = 1) {
  Bytes out(length);
  for (size_t i = 0; i < length; i++) out[i] = static_cast<uint8_t>((i * 37 + seed) & 0xff);
  return out;
}

// --- tests -------------------------------------------------------------------

void testCaps() {
  Rig rig;
  rig.receiver.setPower(87, true, true);
  uint8_t caps[blit::Receiver::kMaxCaps];
  const size_t length = rig.receiver.fillCaps(caps, sizeof(caps));
  EXPECT(length > 1 && caps[0] == blit::kVersion);
  bool sawName = false, sawArea = false, sawFormats = false, sawLimits = false, sawKeys = false, sawPower = false;
  for (size_t i = 1; i + 2 <= length;) {
    const uint8_t tag = caps[i], n = caps[i + 1];
    const uint8_t* v = caps + i + 2;
    i += 2 + n;
    EXPECT(i <= length);
    switch (tag) {
      case blit::kTagName:
        sawName = n == 4 && memcmp(v, "Test", 4) == 0;
        break;
      case blit::kTagArea:
        sawArea = blit::get16(v) == 64 && blit::get16(v + 2) == 32;
        break;
      case blit::kTagFormats:
        sawFormats = n == 2 && v[0] == blit::kFormatMono1 && v[1] == blit::kFormatGray2;
        break;
      case blit::kTagLimits:
        // chunk = min(max message, 512) − the 4-byte offset
        sawLimits = blit::get32(v) == 4096 && blit::get16(v + 4) == 240 && blit::get16(v + 6) == 4;
        break;
      case blit::kTagKeys:
        sawKeys = n == 3 && v[0] == 1 && v[1] == 2 && v[2] == 5;
        break;
      case blit::kTagPower:
        sawPower = v[0] == 87 && v[1] == 0x03;
        break;
      case blit::kTagFeatures:
        EXPECT(blit::get32(v) == blit::kFeaturePersist);
        break;
      default:
        break;
    }
  }
  EXPECT(sawName && sawArea && sawFormats && sawLimits && sawKeys && sawPower);

  // Changes reach a connected host as caps events.
  rig.transport.sent.clear();
  rig.receiver.setArea(48, 32);
  Message m = rig.transport.next();
  EXPECT(m.channel == blit::kChannelEvent && m.bytes.size() > 1 && m.bytes[0] == blit::kEventCaps);
  rig.receiver.setArea(48, 32);  // unchanged: nothing
  EXPECT(rig.transport.sent.empty());
  rig.receiver.setFrameSleep(true);
  EXPECT(rig.transport.next().bytes[0] == blit::kEventCaps);
  // Caps that don't fit a message: an empty caps event (re-read Info).
  rig.transport.max = 20;
  rig.receiver.setArea(64, 32);
  m = rig.transport.next();
  EXPECT(m.bytes.size() == 1 && m.bytes[0] == blit::kEventCaps);
}

void testFullFrame() {
  Rig rig;
  const Bytes pixels = pattern(8 * 32);  // 64 × 32 mono1
  const auto statuses = send(rig, headerFor(blit::kFormatMono1, 64, 32, pixels), pixels);
  // ready, then an ack every 4 writes (16 writes of 16 bytes), no reply to commit yet
  EXPECT(statuses.size() == 1 + 4);
  EXPECT(statuses[0].event == blit::kStatusReady && statuses[0].value == 240);
  EXPECT(statuses[1].event == blit::kStatusAck && statuses[1].value == 64);
  EXPECT(statuses.back().event == blit::kStatusAck && statuses.back().value == pixels.size());

  blit::FrameHeader header;
  EXPECT(rig.receiver.takeFrame(header, rig.frame, rig.region));
  EXPECT(!rig.receiver.takeFrame(header, rig.frame, rig.region));  // once
  EXPECT(header.width == 64 && header.height == 32 && !header.region());
  EXPECT(memcmp(rig.frame.data(), pixels.data(), pixels.size()) == 0);
  EXPECT(rig.receiver.owesReply());
  rig.receiver.notifyDone(0);
  EXPECT(!rig.receiver.owesReply());
  const Status done = status(rig.transport.next());
  EXPECT(done.event == blit::kStatusDone && done.value == 0);
}

void testPackBits() {
  Rig rig;
  Bytes pixels(16 * 32, 0);  // 64 × 32 gray2: mostly white, a few runs and literals
  for (size_t i = 100; i < 140; i++) pixels[i] = 0xff;
  for (size_t i = 300; i < 310; i++) pixels[i] = static_cast<uint8_t>(i);
  const Bytes packed = packbits(pixels);
  EXPECT(packed.size() < pixels.size());
  // One byte per write: every run and literal spans writes.
  send(rig, headerFor(blit::kFormatGray2, 64, 32, packed, blit::kEncodingPackBits), packed, 1);
  blit::FrameHeader header;
  EXPECT(rig.receiver.takeFrame(header, rig.frame, rig.region));
  EXPECT(header.frameBytes() == pixels.size());
  EXPECT(memcmp(rig.frame.data(), pixels.data(), pixels.size()) == 0);

  // A payload that decodes short of the frame fails at commit.
  rig.receiver.notifyDone(0);
  rig.transport.sent.clear();
  Bytes shortPacked = packbits(Bytes(pixels.begin(), pixels.end() - 1));
  const auto statuses = send(rig, headerFor(blit::kFormatGray2, 64, 32, shortPacked, blit::kEncodingPackBits),
                             shortPacked);
  EXPECT(statuses.back().event == blit::kStatusError && statuses.back().code == 10);
}

void testErrors() {
  Rig rig;
  const Bytes pixels(8 * 32, 0);
  const blit::FrameHeader good = headerFor(blit::kFormatMono1, 64, 32, pixels);

  // A v1 header (21 bytes): too short for v2.
  Bytes v1 = {blit::kOpBegin, 1, blit::kFormatMono1, 0, 64, 0, 32, 0};
  v1.resize(21);
  rig.control(v1);
  EXPECT(isError(rig.transport.next(), blit::Error::BadHeader));
  // Version 1 in a v2-sized header, or a later version: unsupported.
  for (const uint8_t version : {1, 3}) {
    blit::FrameHeader h = good;
    h.version = version;
    rig.control(beginMessage(h));
    const Message m = rig.transport.next();
    EXPECT(isError(m, blit::Error::Unsupported) && status(m).value == version);
  }
  rig.control({0x7f});
  EXPECT(isError(rig.transport.next(), blit::Error::BadHeader));

  blit::FrameHeader gray4 = good;
  gray4.format = blit::kFormatGray4;  // not in this display's formats
  gray4.byteLength = 32 * 32;
  rig.control(beginMessage(gray4));
  EXPECT(isError(rig.transport.next(), blit::Error::Unsupported));

  // A small payload that would decode past maxBytes (4096).
  blit::FrameHeader big = headerFor(blit::kFormatGray2, 520, 32, Bytes(10));
  big.encoding = blit::kEncodingPackBits;
  rig.control(beginMessage(big));
  EXPECT(isError(rig.transport.next(), blit::Error::TooLarge));
  blit::FrameHeader wrongSize = good;
  wrongSize.byteLength = 100;  // unencoded but not width × height
  rig.control(beginMessage(wrongSize));
  EXPECT(isError(rig.transport.next(), blit::Error::TooLarge));

  // Out of order.
  rig.transport.sent.clear();
  rig.control(beginMessage(good));
  rig.transport.next();  // ready
  rig.data(16, pixels.data(), 16);
  EXPECT(isError(rig.transport.next(), blit::Error::OutOfOrder));

  // Commit early, then a CRC mismatch.
  rig.control(beginMessage(good));
  rig.transport.next();
  rig.data(0, pixels.data(), 16);
  rig.control({blit::kOpCommit});
  EXPECT(isError(rig.transport.next(), blit::Error::Incomplete));
  blit::FrameHeader badCrc = good;
  badCrc.crc32 ^= 1;
  const auto statuses = send(rig, badCrc, pixels);
  EXPECT(statuses.back().event == blit::kStatusError && statuses.back().code == 7);
  EXPECT(rig.receiver.failed());

  // Nothing reached the main loop.
  blit::FrameHeader header;
  EXPECT(!rig.receiver.takeFrame(header, rig.frame, rig.region));
}

void testBusyAndOwedReply() {
  Rig rig;
  const Bytes pixels(8 * 32, 0);
  const blit::FrameHeader h = headerFor(blit::kFormatMono1, 64, 32, pixels);
  send(rig, h, pixels);
  // Committed but not taken yet: the next begin is busy.
  rig.control(beginMessage(h));
  EXPECT(isError(rig.transport.next(), blit::Error::Busy));

  blit::FrameHeader header;
  EXPECT(rig.receiver.takeFrame(header, rig.frame, rig.region));
  // A host error while the frame is being shown doesn't answer for it.
  rig.control({0x7f});
  EXPECT(rig.receiver.owesReply());
  rig.receiver.notifyError(blit::Error::SaveFailed);
  EXPECT(!rig.receiver.owesReply());
}

void testRegions() {
  Rig rig;
  const Bytes full(8 * 32, 0);
  const Bytes patch(1 * 4, 0xff);  // 8 × 4 mono1
  blit::FrameHeader region = headerFor(blit::kFormatMono1, 8, 4, patch);
  region.flags = blit::kFlagRegion;
  region.x = 16;
  region.y = 2;

  // No full frame yet.
  rig.control(beginMessage(region));
  EXPECT(isError(rig.transport.next(), blit::Error::BadRegion));

  send(rig, headerFor(blit::kFormatMono1, 64, 32, full), full);
  blit::FrameHeader base;
  EXPECT(rig.receiver.takeFrame(base, rig.frame, rig.region));
  rig.receiver.setRegionBase(base);
  rig.receiver.notifyDone(0);
  rig.transport.sent.clear();

  const auto statuses = send(rig, region, patch);
  EXPECT(statuses.back().event == blit::kStatusAck);
  blit::FrameHeader got;
  EXPECT(rig.receiver.takeFrame(got, rig.frame, rig.region));
  EXPECT(got.region() && got.x == 16 && got.y == 2);
  blit::applyRegion(rig.frame.data(), base, got, rig.region.data());
  EXPECT(rig.frame.data()[2 * 8 + 2] == 0xff && rig.frame.data()[2 * 8 + 1] == 0 && rig.frame.data()[6 * 8 + 2] == 0);
  rig.receiver.notifyDone(0);
  rig.transport.sent.clear();

  // Misaligned, outside the area, another format: rejected.
  blit::FrameHeader bad = region;
  bad.x = 4;
  rig.control(beginMessage(bad));
  EXPECT(isError(rig.transport.next(), blit::Error::BadRegion));
  bad = region;
  bad.y = 30;
  rig.control(beginMessage(bad));
  EXPECT(isError(rig.transport.next(), blit::Error::BadRegion));
  bad = headerFor(blit::kFormatGray2, 8, 4, Bytes(2 * 4));
  bad.flags = blit::kFlagRegion;
  rig.control(beginMessage(bad));
  EXPECT(isError(rig.transport.next(), blit::Error::BadRegion));

  // After the area changes, the base no longer counts, even once it's back.
  rig.receiver.setArea(60, 32);
  rig.transport.sent.clear();
  rig.control(beginMessage(region));
  EXPECT(isError(rig.transport.next(), blit::Error::BadRegion));
  rig.receiver.setArea(64, 32);
  rig.transport.sent.clear();
  rig.control(beginMessage(region));
  EXPECT(isError(rig.transport.next(), blit::Error::BadRegion));
  rig.receiver.setArea(60, 32);

  // A width that isn't aligned is fine when it reaches the right edge.
  const Bytes narrow(8 * 32, 0);  // 60 × 32 mono1
  send(rig, headerFor(blit::kFormatMono1, 60, 32, narrow), narrow);
  EXPECT(rig.receiver.takeFrame(base, rig.frame, rig.region));
  rig.receiver.setRegionBase(base);
  rig.receiver.notifyDone(0);
  blit::FrameHeader edge = headerFor(blit::kFormatMono1, 4, 1, Bytes(1));
  edge.flags = blit::kFlagRegion;
  edge.x = 56;
  rig.transport.sent.clear();
  rig.control(beginMessage(edge));
  EXPECT(status(rig.transport.next()).event == blit::kStatusReady);
  rig.control({blit::kOpCancel});
  edge.x = 48;
  rig.control(beginMessage(edge));
  EXPECT(isError(rig.transport.next(), blit::Error::BadRegion));

  // After a reconnect the base doesn't count.
  rig.receiver.onDisconnect();
  rig.receiver.onConnect(rig.transport);
  rig.transport.sent.clear();
  rig.control(beginMessage(region));
  EXPECT(isError(rig.transport.next(), blit::Error::BadRegion));
}

void testHelloAndEvents() {
  Rig rig;
  // No hello yet: keys stay with the device.
  rig.receiver.sendKey(blit::Key::Down);
  EXPECT(rig.transport.sent.empty());

  Bytes hello = {blit::kOpHello, 2, blit::kHelloKeys | blit::kHelloPointer, 4, 'h', 'o', 's', 't'};
  rig.control(hello);
  Message m = rig.transport.next();
  EXPECT(m.channel == blit::kChannelEvent && m.bytes[0] == blit::kEventCaps && m.bytes.size() > 1);
  char name[40];
  rig.receiver.hostName(name, sizeof(name));
  EXPECT(strcmp(name, "host") == 0);

  rig.receiver.sendKey(blit::Key::Down, blit::KeyAction::Long);
  m = rig.transport.next();
  EXPECT(m.bytes.size() == 8 && m.bytes[0] == blit::kEventKey && m.bytes[1] == 2 && m.bytes[2] == 2 &&
         blit::get32(m.bytes.data() + 4) == 1234);
  // Pointer events need the feature as well as the hello.
  rig.receiver.sendPointer(blit::PointerAction::Tap, 1, 2);
  EXPECT(rig.transport.sent.empty());

  // A reconnect forgets the hello and the name.
  rig.receiver.onDisconnect();
  rig.receiver.sendKey(blit::Key::Up);
  rig.receiver.onConnect(rig.transport);
  rig.receiver.sendKey(blit::Key::Up);
  EXPECT(rig.transport.sent.empty());
  rig.receiver.hostName(name, sizeof(name));
  EXPECT(name[0] == '\0');

  Rig pointer(blit::kFeaturePointer);
  pointer.control(hello);
  pointer.transport.sent.clear();
  pointer.receiver.sendPointer(blit::PointerAction::Tap, 10, 20);
  m = pointer.transport.next();
  EXPECT(m.bytes.size() == 6 && m.bytes[0] == blit::kEventPointer && m.bytes[1] == 4 &&
         blit::get16(m.bytes.data() + 2) == 10 && blit::get16(m.bytes.data() + 4) == 20);

  rig.receiver.disconnect();
  EXPECT(rig.transport.dropped);
}

void testPower() {
  Rig rig;
  rig.receiver.setPower(80, false, false);
  EXPECT(rig.transport.next().bytes == (Bytes{blit::kEventPower, 80, 0}));
  rig.receiver.setPower(77, false, false);  // under 5 %: nothing
  EXPECT(rig.transport.sent.empty());
  rig.receiver.setPower(75, false, false);
  EXPECT(rig.transport.next().bytes == (Bytes{blit::kEventPower, 75, 0}));
  rig.receiver.setPower(75, true, true);  // plugged in
  EXPECT(rig.transport.next().bytes == (Bytes{blit::kEventPower, 75, 3}));
  // A host that connects reads the level from Info: the next event is 5 % from that.
  rig.receiver.onDisconnect();
  rig.receiver.setPower(60, true, true);
  rig.receiver.onConnect(rig.transport);
  rig.receiver.setPower(57, true, true);
  EXPECT(rig.transport.sent.empty());
}

void testSleepFor() {
  Rig rig;
  blit::FrameHeader h;
  h.nextFrameSeconds = 60;
  EXPECT(rig.receiver.sleepFor(h) == 0);  // frame sleep off
  rig.receiver.setFrameSleep(true);
  EXPECT(rig.receiver.sleepFor(h) == 50);
  h.nextFrameSeconds = 29;
  EXPECT(rig.receiver.sleepFor(h) == 0);
  h.nextFrameSeconds = 60;
  h.flags = blit::kFlagHold;
  EXPECT(rig.receiver.sleepFor(h) == 0);
}

void testLevels() {
  const uint8_t mono[] = {0b10100000};
  EXPECT(blit::level(mono, blit::kFormatMono1, 0) == 1 && blit::level(mono, blit::kFormatMono1, 1) == 0 &&
         blit::level(mono, blit::kFormatMono1, 2) == 1);
  const uint8_t gray2[] = {0b11100100};
  for (uint32_t x = 0; x < 4; x++) EXPECT(blit::level(gray2, blit::kFormatGray2, x) == 3 - x);
  const uint8_t gray4[] = {0xf1};
  EXPECT(blit::level(gray4, blit::kFormatGray4, 0) == 15 && blit::level(gray4, blit::kFormatGray4, 1) == 1);
}

class MemoryReader : public blit::Reader {
 public:
  explicit MemoryReader(Bytes bytes) : bytes_(std::move(bytes)) {}
  size_t read(uint8_t* out, const size_t length) override {
    const size_t n = std::min(length, bytes_.size() - std::min(at_, bytes_.size()));
    memcpy(out, bytes_.data() + at_, n);
    at_ += n;
    return n;
  }
  bool seek(const uint32_t offset) override {
    at_ = offset;
    return offset <= bytes_.size();
  }

 private:
  Bytes bytes_;
  size_t at_ = 0;
};

void testBmp() {
  for (const uint8_t format : {blit::kFormatMono1, blit::kFormatGray2, blit::kFormatGray4, blit::kFormatGray8}) {
    const uint16_t w = 13, h = 5;
    const size_t frameBytes = blit::rowBytes(format, w) * h;
    Bytes pixels = pattern(frameBytes, format);
    // Row padding is zero on the wire, and must come back as zero.
    const uint32_t bpp = blit::bitsPerPixel(format);
    const uint32_t stride = blit::rowBytes(format, w);
    const uint32_t used = w * bpp % 8;
    if (used != 0) {
      for (uint32_t y = 0; y < h; y++) pixels[y * stride + stride - 1] &= static_cast<uint8_t>(0xff << (8 - used));
    }
    blit::Buffer file;
    EXPECT(blit::encodeBmp(pixels.data(), format, w, h, file));
    const size_t size = blit::bmpSize(format, w, h);
    EXPECT(size > frameBytes && blit::get32(file.data() + 2) == size);

    MemoryReader reader(Bytes(file.data(), file.data() + size));
    blit::Buffer out;
    blit::FrameHeader header;
    EXPECT(blit::decodeBmp(reader, out, header));
    EXPECT(header.format == format && header.width == w && header.height == h);
    EXPECT(memcmp(out.data(), pixels.data(), frameBytes) == 0);
  }

  // A top-down 1-bit BMP whose palette is black first: inverted indices.
  const uint16_t w = 8, h = 2;
  Bytes bmp(14 + 40 + 8 + 4 * h, 0);
  bmp[0] = 'B';
  bmp[1] = 'M';
  blit::put32(&bmp[10], 14 + 40 + 8);
  blit::put32(&bmp[14], 40);
  blit::put32(&bmp[18], w);
  blit::put32(&bmp[22], static_cast<uint32_t>(-static_cast<int32_t>(h)));
  blit::put16(&bmp[26], 1);
  blit::put16(&bmp[28], 1);
  memset(&bmp[54 + 4], 255, 3);  // index 0 black, index 1 white
  bmp[62] = 0x0f;                // top row: left half black
  bmp[66] = 0xff;                // bottom row: white
  MemoryReader reader(bmp);
  blit::Buffer out;
  blit::FrameHeader header;
  EXPECT(blit::decodeBmp(reader, out, header));
  EXPECT(header.format == blit::kFormatMono1 && out.data()[0] == 0xf0 && out.data()[1] == 0x00);

  blit::Buffer file;
  EXPECT(!blit::encodeBmp(bmp.data(), blit::kFormatRgb565, 2, 2, file));
  EXPECT(blit::bmpSize(blit::kFormatRgb888, 2, 2) == 0);
}

}  // namespace

int main() {
  testCaps();
  testFullFrame();
  testPackBits();
  testErrors();
  testBusyAndOwedReply();
  testRegions();
  testHelloAndEvents();
  testPower();
  testSleepFor();
  testLevels();
  testBmp();
  if (failures > 0) {
    fprintf(stderr, "%d failed\n", failures);
    return 1;
  }
  printf("test_receiver: ok\n");
  return 0;
}
