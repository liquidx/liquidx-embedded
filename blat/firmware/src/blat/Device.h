#pragma once

#include <cstddef>
#include <cstdint>

#include "Controls.h"
#include "Store.h"
#include "Values.h"
#include "Wire.h"

namespace blat {

// Carries messages to one host: GATT notifications, or a stream.
class Transport {
 public:
  virtual ~Transport() = default;
  // One message on a device-to-host channel (kChannelReply, kChannelDataOut,
  // kChannelEvent).
  virtual void send(uint8_t channel, const uint8_t* data, size_t length) = 0;
  // The largest message the link carries right now (over BLE, MTU − 3, at
  // most 512).
  virtual size_t maxMessage() const = 0;
};

// Who the device is, for Info.
struct Identity {
  const char* name = "";      // ≤ 32 bytes
  const char* model = "";     // ≤ 32
  const char* firmware = "";  // ≤ 32
  uint8_t deviceId[8] = {};
};

// An action's param, checked against its declaration.
struct Arg {
  Id id = 0;
  int32_t value = 0;        // bool, int, enum
  const char* text = "";    // text, secret: not NUL-terminated, see length
  size_t length = 0;
};

// What the firmware does for the protocol.
class Delegate {
 public:
  virtual ~Delegate() = default;
  // Show the pairing code (six digits), or hide it when `code` is null.
  virtual void showCode(const char* code) {}
  // Run an action. `params` are the ones the host sent.
  virtual Status invoke(Id action, const Arg* params, size_t count) { return Status::ActionFailed; }
};

// The device side of blat, for one host at a time. Everything runs on one
// task (the main loop): feed it what arrives with receive(), call poll()
// often. It answers through the Transport and the Delegate.
class Device {
 public:
  static constexpr size_t kMaxMessage = 512;
  static constexpr size_t kMaxHosts = 8;
  static constexpr uint16_t kWindow = 8;           // DataOut chunks per ack
  static constexpr uint32_t kCodeSeconds = 120;
  static constexpr uint8_t kCodeAttempts = 3;
  static constexpr uint8_t kCodeDigits = 6;
  static constexpr uint32_t kFirstLockoutSeconds = 60;
  static constexpr uint32_t kTransferTimeoutMs = 10000;

  using Clock = uint32_t (*)();  // milliseconds, wrapping

  Device() = default;
  ~Device();
  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;

  // `store` holds remembered hosts; null means none are remembered.
  bool begin(Values& values, Store* store, const Identity& identity, Delegate& delegate, Clock clock);

  // Whether a new host may ask for a code (PROTOCOL.md#listening-mode).
  void setAcceptingHosts(bool on) { acceptingHosts_ = on; }
  bool acceptingHosts() const { return acceptingHosts_; }

  // A host connected on `transport`, or went away.
  void connected(Transport& transport);
  void disconnected();

  // A message from the host on kChannelRequest or kChannelData.
  void receive(uint8_t channel, const uint8_t* data, size_t length);
  // Sends due events and transfer chunks, expires codes and transfers.
  void poll();

  // Info TLVs for this connection (the Info characteristic, and hello).
  size_t info(uint8_t* out, size_t size) const;
  uint8_t level() const { return level_; }
  bool codeShowing() const { return code_[0] != '\0'; }
  // True (once) when anything arrived since the last call.
  bool takeActivity();
  void forgetHosts();
  uint32_t schemaCrc() const { return schemaCrc_; }

 private:
  struct Host {
    uint8_t used = 0;
    uint8_t id[auth::kHostIdBytes] = {};
    uint8_t key[auth::kHostKeyBytes] = {};
    uint32_t lastUsed = 0;
    char name[24] = "";
  };

  size_t chunkSize() const;
  size_t bodyCapacity() const;
  void send(uint8_t channel, const uint8_t* header, size_t headerLength, const uint8_t* body, size_t bodyLength,
            const uint8_t nonce[13]);
  void reply(uint8_t op, uint8_t seq, Status status, uint16_t detail = 0, const uint8_t* body = nullptr,
             size_t length = 0, uint8_t flags = 0);
  void endSession();
  void startSession(uint8_t level, const uint8_t* keys);
  void hideCode();
  void expireCode();

  void dispatch(uint8_t op, uint8_t seq, const uint8_t* body, size_t length);
  void hello(uint8_t seq, const uint8_t* body, size_t length);
  void authBegin(uint8_t seq, const uint8_t* body, size_t length);
  void authCode(uint8_t seq, const uint8_t* body, size_t length);
  void authConfirm(uint8_t seq, const uint8_t* body, size_t length);
  void authResume(uint8_t seq, const uint8_t* body, size_t length);
  void remember(uint8_t seq, const uint8_t* body, size_t length);
  void get(uint8_t seq, const uint8_t* body, size_t length);
  void set(uint8_t seq, const uint8_t* body, size_t length);
  void invoke(uint8_t seq, const uint8_t* body, size_t length);
  void options(uint8_t seq, const uint8_t* body, size_t length);
  void readOpen(uint8_t seq, const uint8_t* body, size_t length);
  void ack(const uint8_t* body, size_t length);

  // Values on the wire (PROTOCOL.md#values).
  size_t encodeValue(Id id, uint8_t* out, size_t size) const;
  // Parses one value of `id`'s type at `p`; returns bytes used, 0 if malformed.
  size_t decodeValue(Id id, const uint8_t* p, size_t length, Arg& out) const;
  Status checkArg(const Arg& arg) const;

  void sendChanges();
  void sendChunks();
  void loadHosts();
  void saveHosts();

  Values* values_ = nullptr;
  Store* store_ = nullptr;
  Identity identity_;
  Delegate* delegate_ = nullptr;
  Clock clock_ = nullptr;
  Transport* transport_ = nullptr;
  bool acceptingHosts_ = true;
  bool activity_ = false;

  uint8_t* schema_ = nullptr;
  size_t schemaSize_ = 0;
  uint32_t schemaCrc_ = 0;

  // The session with the connected host.
  uint8_t level_ = 0;
  bool wantsEvents_ = false;
  bool sealed_ = false;
  uint8_t hostKey_[16] = {};    // host → device
  uint8_t deviceKey_[16] = {};  // device → host
  uint32_t rxRequests_ = 0;
  uint32_t txReplies_ = 0;
  uint32_t txEvents_ = 0;
  uint8_t nextTransferId_ = 1;

  // Pairing with a code.
  char code_[kCodeDigits + 1] = "";
  uint8_t salt_[auth::kSaltBytes] = {};
  uint32_t codeShownMs_ = 0;
  uint8_t attemptsLeft_ = 0;
  bool spakePending_ = false;
  uint8_t expectConfirmA_[32] = {};
  uint8_t ke_[16] = {};
  uint32_t wrongAttempts_ = 0;  // since the last success, across codes
  uint32_t lockoutSeconds_ = kFirstLockoutSeconds;
  uint32_t lockedAtMs_ = 0;
  uint32_t lockedForMs_ = 0;

  // Resuming a remembered host.
  int resumeHost_ = -1;
  uint8_t challenge_[auth::kNonceBytes] = {};

  // The one transfer in flight (the schema, for now).
  bool transferring_ = false;
  uint8_t transferId_ = 0;
  uint32_t sent_ = 0;
  uint32_t acked_ = 0;
  uint32_t lastAckMs_ = 0;

  Host hosts_[kMaxHosts];

  // Message buffers, here rather than on the (main loop's) stack, which also
  // has to hold mbedTLS's elliptic curve arithmetic while pairing.
  uint8_t rx_[kMaxMessage];
  uint8_t tx_[kMaxMessage];
};

}  // namespace blat
