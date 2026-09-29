#include "Device.h"

#include <cstdio>
#include <cstring>

#include "Crypto.h"
#include "Schema.h"

namespace blat {

namespace {

constexpr const char* kHostsKey = "_hosts";
constexpr const char kHostIdentity[] = "blat host";
constexpr const char kDeviceLabel[] = "blat device";
constexpr const char kSessionInfo[] = "blat session";
constexpr size_t kTransferHeaderBytes = 5;  // u8 transferId, u32 offset
constexpr size_t kMaxArgs = 32;

// A 13-byte CCM nonce: channel, then a counter or transfer position, then zeros.
void counterNonce(uint8_t nonce[13], const uint8_t channel, const uint32_t count) {
  memset(nonce, 0, 13);
  nonce[0] = channel;
  put32(nonce + 1, count);
}

void transferNonce(uint8_t nonce[13], const uint8_t channel, const uint8_t transferId, const uint32_t offset) {
  memset(nonce, 0, 13);
  nonce[0] = channel;
  nonce[1] = transferId;
  put32(nonce + 2, offset);
}

bool elapsed(const uint32_t now, const uint32_t since, const uint32_t ms) { return now - since >= ms; }

}  // namespace

Device::~Device() { delete[] schema_; }

bool Device::begin(Values& values, Store* store, const Identity& identity, Delegate& delegate, const Clock clock) {
  values_ = &values;
  store_ = store;
  identity_ = identity;
  delegate_ = &delegate;
  clock_ = clock;
  schemaSize_ = encodeSchema(values.table(), nullptr, 0);
  schema_ = new uint8_t[schemaSize_];
  if (schema_ == nullptr || encodeSchema(values.table(), schema_, schemaSize_) != schemaSize_) return false;
  schemaCrc_ = crc32(schema_, schemaSize_);
  loadHosts();
  return true;
}

// --- Connection and session ---------------------------------------------

void Device::connected(Transport& transport) {
  transport_ = &transport;
  endSession();
  while (values_->takeDeviceChange() != 0) {
  }  // changes before the host arrived aren't news to it
  activity_ = true;
}

void Device::disconnected() {
  transport_ = nullptr;
  endSession();
  hideCode();  // nobody left to type it
  activity_ = true;
}

void Device::endSession() {
  level_ = 0;
  wantsEvents_ = false;
  sealed_ = false;
  memset(hostKey_, 0, sizeof(hostKey_));
  memset(deviceKey_, 0, sizeof(deviceKey_));
  rxRequests_ = txReplies_ = txEvents_ = 0;
  nextTransferId_ = 1;
  spakePending_ = false;
  resumeHost_ = -1;
  transferring_ = false;
}

// Keys take effect from the next message: the reply that completes
// authentication goes out under the session as it was.
void Device::startSession(const uint8_t level, const uint8_t* keys) {
  level_ = level;
  sealed_ = true;
  memcpy(hostKey_, keys, 16);
  memcpy(deviceKey_, keys + 16, 16);
  rxRequests_ = txReplies_ = txEvents_ = 0;
  nextTransferId_ = 1;
  transferring_ = false;
}

// Hide the code once its time is up. poll() does this too, but a request may
// arrive first.
void Device::expireCode() {
  if (codeShowing() && elapsed(clock_(), codeShownMs_, kCodeSeconds * 1000)) hideCode();
}

void Device::hideCode() {
  if (!codeShowing()) return;
  code_[0] = '\0';
  spakePending_ = false;
  delegate_->showCode(nullptr);
}

bool Device::takeActivity() {
  const bool was = activity_;
  activity_ = false;
  return was;
}

// --- Sending --------------------------------------------------------------

size_t Device::bodyCapacity() const {
  const size_t max = transport_ != nullptr ? transport_->maxMessage() : 20;
  const size_t overhead = kReplyHeaderBytes + (sealed_ ? auth::kTagBytes : 0);
  return max > overhead ? max - overhead : 0;
}

size_t Device::chunkSize() const {
  const size_t max = transport_ != nullptr ? transport_->maxMessage() : 244;
  return max - kTransferHeaderBytes - (sealed_ ? auth::kTagBytes : 0);
}

void Device::send(const uint8_t channel, const uint8_t* header, const size_t headerLength, const uint8_t* body,
                  const size_t bodyLength, const uint8_t nonce[13]) {
  if (transport_ == nullptr) return;
  uint8_t* msg = tx_;
  const size_t length = headerLength + bodyLength + (sealed_ ? auth::kTagBytes : 0);
  if (length > sizeof(tx_) || length > transport_->maxMessage()) return;
  memcpy(msg, header, headerLength);
  if (sealed_) {
    if (!crypto::seal(deviceKey_, nonce, header, headerLength, body, bodyLength, msg + headerLength)) return;
  } else if (bodyLength > 0) {
    memcpy(msg + headerLength, body, bodyLength);
  }
  transport_->send(channel, msg, length);
}

void Device::reply(const uint8_t op, const uint8_t seq, const Status status, const uint16_t detail,
                   const uint8_t* body, size_t length, uint8_t flags) {
  // A body that doesn't fit goes empty with `more` set (for hello: read Info).
  if (length > bodyCapacity()) {
    length = 0;
    flags |= kReplyMore;
  }
  uint8_t header[kReplyHeaderBytes] = {op, seq, static_cast<uint8_t>(status), flags};
  put16(header + 4, detail);
  uint8_t nonce[13];
  counterNonce(nonce, kChannelReply, txReplies_);
  if (sealed_) txReplies_++;
  send(kChannelReply, header, sizeof(header), body, length, nonce);
}

// --- Receiving ------------------------------------------------------------

void Device::receive(const uint8_t channel, const uint8_t* data, const size_t length) {
  activity_ = true;
  if (channel != kChannelRequest) return;  // Data: no uploads yet
  if (length < kRequestHeaderBytes) return reply(0, 0, Status::BadRequest);
  const uint8_t op = data[0], seq = data[1];
  uint8_t* body = rx_;
  size_t bodyLength = length - kRequestHeaderBytes;
  if (bodyLength > sizeof(rx_)) return reply(op, seq, Status::TooLarge);
  if (sealed_) {
    uint8_t nonce[13];
    counterNonce(nonce, kChannelRequest, rxRequests_++);
    if (bodyLength < auth::kTagBytes ||
        !crypto::open(hostKey_, nonce, data, kRequestHeaderBytes, data + kRequestHeaderBytes, bodyLength, body)) {
      // Wrong key, replay or tampering: the session is over.
      endSession();
      return reply(op, seq, Status::AuthRequired, kLevelPaired);
    }
    bodyLength -= auth::kTagBytes;
  } else {
    memcpy(body, data + kRequestHeaderBytes, bodyLength);
  }
  dispatch(op, seq, body, bodyLength);
  memset(body, 0, bodyLength);  // may hold a secret
}

void Device::dispatch(const uint8_t op, const uint8_t seq, const uint8_t* body, const size_t length) {
  switch (op) {
    case op::kHello:
      return hello(seq, body, length);
    case op::kAuthBegin:
      return authBegin(seq, body, length);
    case op::kAuthCode:
      return authCode(seq, body, length);
    case op::kAuthConfirm:
      return authConfirm(seq, body, length);
    case op::kAuthResume:
      return authResume(seq, body, length);
    case op::kRemember:
      return remember(seq, body, length);
    case op::kGet:
      return get(seq, body, length);
    case op::kSet:
      return set(seq, body, length);
    case op::kInvoke:
      return invoke(seq, body, length);
    case op::kOptions:
      return options(seq, body, length);
    case op::kReadOpen:
      return readOpen(seq, body, length);
    case op::kAck:
      return ack(body, length);
    case op::kCancel:
      transferring_ = false;
      return reply(op, seq, Status::Ok);
    default:
      // Includes write-open, commit, list and delete: no files yet.
      return reply(op, seq, Status::BadRequest, op);
  }
}

void Device::hello(const uint8_t seq, const uint8_t* body, const size_t length) {
  if (length < 3) return reply(op::kHello, seq, Status::BadRequest, op::kHello);
  if (body[0] < 1) return reply(op::kHello, seq, Status::UnsupportedVersion, kVersion);
  wantsEvents_ = body[1] & 0x01;
  uint8_t out[160];
  const size_t n = info(out, sizeof(out));
  reply(op::kHello, seq, Status::Ok, 0, out, n);
}

size_t Device::info(uint8_t* out, const size_t size) const {
  uint8_t buf[160];
  uint8_t* p = buf;
  *p++ = kVersion;
  const auto text = [&](const uint8_t tag, const char* s) {
    const size_t n = strnlen(s, 32);
    *p++ = tag;
    *p++ = n;
    memcpy(p, s, n);
    p += n;
  };
  text(info::kName, identity_.name);
  text(info::kModel, identity_.model);
  text(info::kFirmware, identity_.firmware);
  *p++ = info::kDeviceId;
  *p++ = 8;
  memcpy(p, identity_.deviceId, 8);
  p += 8;
  *p++ = info::kSchema;
  *p++ = 8;
  p = put32(put32(p, schemaCrc_), schemaSize_);
  *p++ = info::kLimits;
  *p++ = 8;
  p = put32(put16(put16(p, chunkSize()), kWindow), 0);  // no files: maxFile 0
  *p++ = info::kFeatures;
  *p++ = 4;
  p = put32(p, info::kFeatureEvents);
  *p++ = info::kAuth;
  *p++ = 3;
  *p++ = level_;
  *p++ = info::kMethodScreen | (store_ != nullptr ? info::kMethodRemember : 0);
  *p++ = kCodeDigits;
  const size_t n = p - buf;
  if (n > size) return 0;
  memcpy(out, buf, n);
  return n;
}

// --- Authentication -------------------------------------------------------

void Device::authBegin(const uint8_t seq, const uint8_t* body, const size_t length) {
  constexpr uint8_t kOp = op::kAuthBegin;
  if (length < 1) return reply(kOp, seq, Status::BadRequest, kOp);
  const uint32_t now = clock_();
  expireCode();

  if (body[0] == auth::kMethodResume) {
    if (length < 1 + auth::kHostIdBytes) return reply(kOp, seq, Status::BadRequest, kOp);
    resumeHost_ = -1;
    for (size_t i = 0; i < kMaxHosts; i++) {
      if (hosts_[i].used && crypto::equal(hosts_[i].id, body + 1, auth::kHostIdBytes)) resumeHost_ = i;
    }
    if (resumeHost_ < 0) return reply(kOp, seq, Status::WrongCode, 0);
    crypto::random(challenge_, sizeof(challenge_));
    return reply(kOp, seq, Status::Ok, 0, challenge_, sizeof(challenge_));
  }
  if (body[0] != auth::kMethodCode) return reply(kOp, seq, Status::BadRequest, kOp);

  if (lockedForMs_ != 0 && !elapsed(now, lockedAtMs_, lockedForMs_)) {
    const uint32_t left = (lockedForMs_ - (now - lockedAtMs_) + 999) / 1000;
    return reply(kOp, seq, Status::LockedOut, left > 0xFFFF ? 0xFFFF : left);
  }
  lockedForMs_ = 0;
  if (!codeShowing()) {
    if (!acceptingHosts_) return reply(kOp, seq, Status::AuthRequired, kLevelPresent);
    // A fresh code. Asking again while it shows gets the same one, so a host
    // can't skip to a new code (and three new tries) whenever it likes.
    uint32_t r;
    do {
      crypto::random(reinterpret_cast<uint8_t*>(&r), sizeof(r));
    } while (r >= 4294000000u);  // a multiple of 10^6: no bias
    snprintf(code_, sizeof(code_), "%06u", static_cast<unsigned>(r % 1000000u));
    crypto::random(salt_, sizeof(salt_));
    codeShownMs_ = now;
    attemptsLeft_ = kCodeAttempts;
    delegate_->showCode(code_);
  }
  spakePending_ = false;
  const uint32_t left = kCodeSeconds - (now - codeShownMs_) / 1000;
  uint8_t out[4 + auth::kSaltBytes] = {kCodeDigits, attemptsLeft_};
  put16(out + 2, left);
  memcpy(out + 4, salt_, sizeof(salt_));
  reply(kOp, seq, Status::Ok, 0, out, sizeof(out));
}

void Device::authCode(const uint8_t seq, const uint8_t* body, const size_t length) {
  constexpr uint8_t kOp = op::kAuthCode;
  expireCode();
  if (!codeShowing()) return reply(kOp, seq, Status::AuthRequired, kLevelPresent);
  if (length < auth::kPointBytes) return reply(kOp, seq, Status::BadRequest, kOp);
  uint8_t w[32];
  crypto::Spake2Keys keys;
  const bool ok = crypto::codeToW(salt_, sizeof(salt_), code_, w) &&
                  crypto::spake2Respond(w, body, reinterpret_cast<const uint8_t*>(kHostIdentity),
                                        sizeof(kHostIdentity) - 1, identity_.deviceId, sizeof(identity_.deviceId),
                                        salt_, sizeof(salt_), keys);
  memset(w, 0, sizeof(w));
  if (!ok) return reply(kOp, seq, Status::BadRequest, kOp);  // pA isn't a point
  memcpy(expectConfirmA_, keys.confirmA, sizeof(expectConfirmA_));
  memcpy(ke_, keys.ke, sizeof(ke_));
  spakePending_ = true;
  uint8_t out[auth::kPointBytes + auth::kMacBytes];
  memcpy(out, keys.pB, auth::kPointBytes);
  memcpy(out + auth::kPointBytes, keys.confirmB, auth::kMacBytes);
  memset(&keys, 0, sizeof(keys));
  reply(kOp, seq, Status::Ok, 0, out, sizeof(out));
}

void Device::authConfirm(const uint8_t seq, const uint8_t* body, const size_t length) {
  constexpr uint8_t kOp = op::kAuthConfirm;
  expireCode();
  if (!spakePending_ || !codeShowing()) return reply(kOp, seq, Status::AuthRequired, kLevelPresent);
  if (length < auth::kMacBytes) return reply(kOp, seq, Status::BadRequest, kOp);
  spakePending_ = false;

  if (!crypto::equal(body, expectConfirmA_, auth::kMacBytes)) {
    wrongAttempts_++;
    attemptsLeft_--;
    const uint8_t left = attemptsLeft_;
    if (left == 0) hideCode();
    if (wrongAttempts_ % (kCodeAttempts * 3) == 0) {
      // Three codes' worth of wrong guesses in a row: stop showing codes for a
      // while, twice as long each time.
      hideCode();
      lockedAtMs_ = clock_();
      lockedForMs_ = lockoutSeconds_ * 1000;
      lockoutSeconds_ *= 2;
    }
    return reply(kOp, seq, Status::WrongCode, left);
  }

  uint8_t keys[32];
  crypto::hkdf(salt_, sizeof(salt_), ke_, sizeof(ke_), reinterpret_cast<const uint8_t*>(kSessionInfo),
               sizeof(kSessionInfo) - 1, keys, sizeof(keys));
  memset(ke_, 0, sizeof(ke_));
  wrongAttempts_ = 0;
  lockoutSeconds_ = kFirstLockoutSeconds;
  hideCode();
  const uint8_t level = kLevelPresent;
  reply(kOp, seq, Status::Ok, 0, &level, 1);
  startSession(level, keys);
  memset(keys, 0, sizeof(keys));
}

void Device::authResume(const uint8_t seq, const uint8_t* body, const size_t length) {
  constexpr uint8_t kOp = op::kAuthResume;
  if (resumeHost_ < 0) return reply(kOp, seq, Status::AuthRequired, kLevelPaired);
  if (length < auth::kNonceBytes + auth::kMacBytes) return reply(kOp, seq, Status::BadRequest, kOp);
  Host& host = hosts_[resumeHost_];
  resumeHost_ = -1;  // one try per challenge
  const uint8_t* hostNonce = body;

  const auto mac = [&](const char* label, const size_t labelLength, uint8_t out[32]) {
    crypto::Hmac h(host.key, sizeof(host.key));
    h.update(label, labelLength);
    h.update(challenge_, sizeof(challenge_));
    h.update(hostNonce, auth::kNonceBytes);
    h.update(identity_.deviceId, sizeof(identity_.deviceId));
    h.finish(out);
  };
  uint8_t expected[32];
  mac(kHostIdentity, sizeof(kHostIdentity) - 1, expected);
  if (!crypto::equal(expected, body + auth::kNonceBytes, sizeof(expected))) {
    return reply(kOp, seq, Status::WrongCode, 0);
  }

  uint8_t out[auth::kMacBytes + 1];
  mac(kDeviceLabel, sizeof(kDeviceLabel) - 1, out);
  out[auth::kMacBytes] = kLevelPaired;
  uint8_t salt[2 * auth::kNonceBytes];
  memcpy(salt, challenge_, auth::kNonceBytes);
  memcpy(salt + auth::kNonceBytes, hostNonce, auth::kNonceBytes);
  uint8_t keys[32];
  crypto::hkdf(salt, sizeof(salt), host.key, sizeof(host.key), reinterpret_cast<const uint8_t*>(kSessionInfo),
               sizeof(kSessionInfo) - 1, keys, sizeof(keys));

  uint32_t newest = 0;
  for (const auto& h : hosts_) newest = h.lastUsed > newest ? h.lastUsed : newest;
  host.lastUsed = newest + 1;
  saveHosts();

  reply(kOp, seq, Status::Ok, 0, out, sizeof(out));
  startSession(kLevelPaired, keys);
  memset(keys, 0, sizeof(keys));
}

void Device::remember(const uint8_t seq, const uint8_t* body, const size_t length) {
  constexpr uint8_t kOp = op::kRemember;
  if (!sealed_) return reply(kOp, seq, Status::AuthRequired, kLevelPaired);
  if (store_ == nullptr) return reply(kOp, seq, Status::StorageError);
  // An empty slot, or the least recently used host.
  size_t slot = 0;
  uint32_t newest = 0;
  for (size_t i = 0; i < kMaxHosts; i++) {
    newest = hosts_[i].lastUsed > newest ? hosts_[i].lastUsed : newest;
    if (!hosts_[i].used) {
      if (hosts_[slot].used) slot = i;
    } else if (hosts_[slot].used && hosts_[i].lastUsed < hosts_[slot].lastUsed) {
      slot = i;
    }
  }
  Host& host = hosts_[slot];
  host = Host{};
  host.used = 1;
  host.lastUsed = newest + 1;
  crypto::random(host.id, sizeof(host.id));
  crypto::random(host.key, sizeof(host.key));
  const size_t nameLength = length >= 1 && body[0] <= length - 1 ? body[0] : 0;
  const size_t n = nameLength < sizeof(host.name) - 1 ? nameLength : sizeof(host.name) - 1;
  memcpy(host.name, body + 1, n);
  saveHosts();
  uint8_t out[auth::kHostIdBytes + auth::kHostKeyBytes];
  memcpy(out, host.id, sizeof(host.id));
  memcpy(out + sizeof(host.id), host.key, sizeof(host.key));
  reply(kOp, seq, Status::Ok, 0, out, sizeof(out));
  memset(out, 0, sizeof(out));
}

void Device::forgetHosts() {
  for (auto& h : hosts_) h = Host{};
  if (store_ != nullptr) store_->remove(kHostsKey);
}

void Device::loadHosts() {
  for (auto& h : hosts_) h = Host{};
  if (store_ == nullptr) return;
  Host saved[kMaxHosts];
  if (store_->getBytes(kHostsKey, saved, sizeof(saved)) == sizeof(saved)) memcpy(hosts_, saved, sizeof(saved));
}

void Device::saveHosts() {
  if (store_ != nullptr) store_->putBytes(kHostsKey, hosts_, sizeof(hosts_));
}

// --- Values ---------------------------------------------------------------

size_t Device::encodeValue(const Id id, uint8_t* out, const size_t size) const {
  const Control& c = values_->control(id);
  const int32_t v = values_->get(id);
  switch (c.type_) {
    case Type::Bool:
      if (size < 1) return 0;
      out[0] = v ? 1 : 0;
      return 1;
    case Type::Int:
      if (size < 4) return 0;
      put32(out, v);
      return 4;
    case Type::Enum:
      if (size < 2) return 0;
      put16(out, v);
      return 2;
    case Type::Secret:
      if (size < 1) return 0;
      out[0] = values_->isSet(id) ? 1 : 0;
      return 1;
    case Type::Text: {
      const char* s = values_->text(id);
      const size_t n = strlen(s);
      if (size < 1 + n) return 0;
      out[0] = n;
      memcpy(out + 1, s, n);
      return 1 + n;
    }
    default:
      return 0;
  }
}

size_t Device::decodeValue(const Id id, const uint8_t* p, const size_t length, Arg& out) const {
  out = Arg{};
  out.id = id;
  switch (values_->control(id).type_) {
    case Type::Bool:
      if (length < 1) return 0;
      out.value = p[0];
      return 1;
    case Type::Int:
      if (length < 4) return 0;
      out.value = static_cast<int32_t>(get32(p));
      return 4;
    case Type::Enum:
      if (length < 2) return 0;
      out.value = get16(p);
      return 2;
    case Type::Text:
    case Type::Secret:
      if (length < 1 || length < 1u + p[0]) return 0;
      out.text = reinterpret_cast<const char*>(p + 1);
      out.length = p[0];
      return 1 + p[0];
    default:
      return 0;
  }
}

Status Device::checkArg(const Arg& arg) const {
  return values_->control(arg.id).isText() ? values_->checkText(arg.id, arg.text, arg.length)
                                            : values_->check(arg.id, arg.value);
}

void Device::get(const uint8_t seq, const uint8_t* body, const size_t length) {
  constexpr uint8_t kOp = op::kGet;
  if (length % 2 != 0) return reply(kOp, seq, Status::BadRequest, kOp);
  const Table& t = values_->table();
  uint8_t out[kMaxMessage];
  const size_t cap = bodyCapacity() < sizeof(out) ? bodyCapacity() : sizeof(out);
  size_t n = 0;
  uint8_t flags = 0;
  const auto add = [&](const Id id) {
    uint8_t value[1 + 255];
    const size_t v = encodeValue(id, value, sizeof(value));
    if (n + 2 + v > cap) {
      flags = kReplyMore;
      return false;
    }
    put16(out + n, id);
    memcpy(out + n + 2, value, v);
    n += 2 + v;
    return true;
  };

  if (length == 0) {
    // Every value this connection may read.
    for (Id id = 1; id <= t.count; id++) {
      if (t[id].hasValue() && t[id].read_ <= level_ && !add(id)) break;
    }
    return reply(kOp, seq, Status::Ok, 0, out, n, flags);
  }
  for (size_t i = 0; i < length; i += 2) {
    const Id id = get16(body + i);
    if (!t.valid(id) || !t[id].hasValue()) return reply(kOp, seq, Status::UnknownId, id);
    if (t[id].read_ > level_) return reply(kOp, seq, Status::AuthRequired, t[id].read_);
  }
  for (size_t i = 0; i < length && add(get16(body + i)); i += 2) {
  }
  reply(kOp, seq, Status::Ok, 0, out, n, flags);
}

void Device::set(const uint8_t seq, const uint8_t* body, const size_t length) {
  constexpr uint8_t kOp = op::kSet;
  const Table& t = values_->table();
  Arg args[kMaxArgs];
  size_t count = 0;
  // Check everything first: a set applies all of its values or none.
  for (size_t at = 0; at < length;) {
    if (length - at < 2 || count == kMaxArgs) return reply(kOp, seq, Status::BadRequest, kOp);
    const Id id = get16(body + at);
    at += 2;
    if (!t.valid(id) || !t[id].hasValue()) return reply(kOp, seq, Status::UnknownId, id);
    if (t[id].is(flag::kReadOnly)) return reply(kOp, seq, Status::ReadOnly, id);
    if (t[id].write_ > level_ || level_ < kLevelPaired) {
      return reply(kOp, seq, Status::AuthRequired, t[id].write_ < kLevelPaired ? kLevelPaired : t[id].write_);
    }
    const size_t used = decodeValue(id, body + at, length - at, args[count]);
    if (used == 0) return reply(kOp, seq, Status::BadRequest, id);
    at += used;
    const Status s = checkArg(args[count]);
    if (s != Status::Ok) return reply(kOp, seq, s, id);
    count++;
  }
  for (size_t i = 0; i < count; i++) {
    const Arg& a = args[i];
    if (t[a.id].isText()) {
      values_->setTextFromHost(a.id, a.text, a.length);
    } else {
      values_->setFromHost(a.id, a.value);
    }
  }
  reply(kOp, seq, Status::Ok);
}

void Device::invoke(const uint8_t seq, const uint8_t* body, const size_t length) {
  constexpr uint8_t kOp = op::kInvoke;
  const Table& t = values_->table();
  if (length < 2) return reply(kOp, seq, Status::BadRequest, kOp);
  const Id action = get16(body);
  if (!t.valid(action) || t[action].type_ != Type::Action) return reply(kOp, seq, Status::UnknownId, action);
  if (t[action].write_ > level_ || level_ < kLevelPaired) {
    return reply(kOp, seq, Status::AuthRequired, t[action].write_ < kLevelPaired ? kLevelPaired : t[action].write_);
  }
  Arg args[kMaxArgs];
  size_t count = 0;
  for (size_t at = 2; at < length;) {
    if (length - at < 2 || count == kMaxArgs) return reply(kOp, seq, Status::BadRequest, kOp);
    const Id id = get16(body + at);
    at += 2;
    if (!t.valid(id) || parentOf(t, id) != action || !t[id].hasValue()) {
      return reply(kOp, seq, Status::UnknownId, id);
    }
    const size_t used = decodeValue(id, body + at, length - at, args[count]);
    if (used == 0) return reply(kOp, seq, Status::BadRequest, id);
    at += used;
    const Status s = checkArg(args[count]);
    if (s != Status::Ok) return reply(kOp, seq, s, id);
    count++;
  }
  for (Id id = action + 1; id <= t.count; id++) {
    if (parentOf(t, id) != action || !t[id].is(flag::kRequired)) continue;
    bool given = false;
    for (size_t i = 0; i < count; i++) given = given || args[i].id == id;
    if (!given) return reply(kOp, seq, Status::InvalidValue, id);
  }
  const Status s = delegate_->invoke(action, args, count);
  reply(kOp, seq, s, s == Status::Ok || s == Status::Running ? 0 : action);
}

void Device::options(const uint8_t seq, const uint8_t* body, const size_t length) {
  constexpr uint8_t kOp = op::kOptions;
  const Table& t = values_->table();
  if (length < 2) return reply(kOp, seq, Status::BadRequest, kOp);
  const Id id = get16(body);
  if (!t.valid(id) || t[id].type_ != Type::Enum) return reply(kOp, seq, Status::UnknownId, id);
  if (t[id].read_ > level_) return reply(kOp, seq, Status::AuthRequired, t[id].read_);
  const Control& c = t[id];
  uint8_t out[kMaxMessage];
  const size_t cap = bodyCapacity() < sizeof(out) ? bodyCapacity() : sizeof(out);
  size_t n = 1;
  uint8_t count = 0, flags = 0;
  for (uint8_t i = 0; i < c.optionCount_; i++) {
    const size_t labelLength = strnlen(c.options_[i].label, 255);
    if (n + 3 + labelLength > cap) {
      flags = kReplyMore;
      break;
    }
    put16(out + n, c.options_[i].value);
    out[n + 2] = labelLength;
    memcpy(out + n + 3, c.options_[i].label, labelLength);
    n += 3 + labelLength;
    count++;
  }
  out[0] = count;
  reply(kOp, seq, Status::Ok, 0, out, n, flags);
}

// --- Transfers --------------------------------------------------------------

void Device::readOpen(const uint8_t seq, const uint8_t* body, const size_t length) {
  constexpr uint8_t kOp = op::kReadOpen;
  if (length < 3) return reply(kOp, seq, Status::BadRequest, kOp);
  if (body[0] != 0) return reply(kOp, seq, Status::NotFound);  // only the schema, so far
  if (transferring_) return reply(kOp, seq, Status::Busy);
  // Nonces for DataOut use the transfer id; never reuse one under a key.
  if (sealed_ && nextTransferId_ == 0) return reply(kOp, seq, Status::AuthRequired, level_);
  transferId_ = nextTransferId_++;
  transferring_ = true;
  sent_ = acked_ = 0;
  lastAckMs_ = clock_();
  uint8_t out[11] = {transferId_};
  put16(put32(put32(out + 1, schemaSize_), schemaCrc_), chunkSize());
  reply(kOp, seq, Status::Ok, 0, out, sizeof(out));
  sendChunks();
}

void Device::ack(const uint8_t* body, const size_t length) {
  if (length < 5 || !transferring_ || body[0] != transferId_) return;
  const uint32_t received = get32(body + 1);
  if (received > sent_) return;
  acked_ = received;
  lastAckMs_ = clock_();
  if (acked_ >= schemaSize_) {
    transferring_ = false;
    return;
  }
  sendChunks();
}

// Up to kWindow chunks past the last ack.
void Device::sendChunks() {
  if (!transferring_ || transport_ == nullptr) return;
  const size_t chunk = chunkSize();
  while (sent_ < schemaSize_ && sent_ - acked_ < kWindow * chunk) {
    const size_t n = schemaSize_ - sent_ < chunk ? schemaSize_ - sent_ : chunk;
    uint8_t header[kTransferHeaderBytes] = {transferId_};
    put32(header + 1, sent_);
    uint8_t nonce[13];
    transferNonce(nonce, kChannelDataOut, transferId_, sent_);
    send(kChannelDataOut, header, sizeof(header), schema_ + sent_, n, nonce);
    sent_ += n;
  }
}

// --- Events -----------------------------------------------------------------

void Device::sendChanges() {
  if (transport_ == nullptr) return;
  const Table& t = values_->table();
  uint8_t body[kMaxMessage];
  const size_t max = transport_->maxMessage();
  const size_t cap = max - 1 - (sealed_ ? auth::kTagBytes : 0);
  size_t n = 0;
  const auto flush = [&]() {
    if (n == 0) return;
    const uint8_t type = event::kChanged;
    uint8_t nonce[13];
    counterNonce(nonce, kChannelEvent, txEvents_);
    if (sealed_) txEvents_++;
    send(kChannelEvent, &type, 1, body, n, nonce);
    n = 0;
  };
  for (Id id = values_->takeDeviceChange(); id != 0; id = values_->takeDeviceChange()) {
    const Control& c = t[id];
    if (!wantsEvents_ || !c.is(flag::kLive) || c.read_ > level_) continue;
    uint8_t value[1 + 255];
    const size_t v = encodeValue(id, value, sizeof(value));
    if (n + 2 + v > cap) flush();
    put16(body + n, id);
    memcpy(body + n + 2, value, v);
    n += 2 + v;
  }
  flush();
}

void Device::poll() {
  const uint32_t now = clock_();
  expireCode();
  if (transferring_ && elapsed(now, lastAckMs_, kTransferTimeoutMs)) transferring_ = false;
  if (transport_ == nullptr) {
    while (values_->takeDeviceChange() != 0) {
    }
    return;
  }
  sendChanges();
  sendChunks();
}

}  // namespace blat
