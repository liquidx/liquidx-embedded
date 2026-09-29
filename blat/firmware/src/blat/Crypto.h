#pragma once

#include <cstddef>
#include <cstdint>

// The cryptography blat authentication needs (PROTOCOL.md#authentication),
// on mbedTLS: SPAKE2 on P-256 for pairing with a code, HMAC-SHA256 for
// resuming, HKDF-SHA256 for keys, AES-128-CCM for sealing.
namespace blat::crypto {

constexpr size_t kPointBytes = 65;
constexpr size_t kScalarBytes = 32;
constexpr size_t kHashBytes = 32;
constexpr size_t kKeyBytes = 16;  // AES-128
constexpr size_t kNonceBytes = 13;
constexpr size_t kTagBytes = 8;

// Randomness. Set once at startup (esp_fill_random on ESP32). Tests set a
// deterministic one.
using RandomFn = void (*)(uint8_t* out, size_t length);
void setRandom(RandomFn fn);
void random(uint8_t* out, size_t length);

void sha256(const uint8_t* data, size_t length, uint8_t out[kHashBytes]);

// HMAC-SHA256 over several pieces.
class Hmac {
 public:
  Hmac(const uint8_t* key, size_t keyLength);
  ~Hmac();
  Hmac(const Hmac&) = delete;
  Hmac& operator=(const Hmac&) = delete;
  void update(const void* data, size_t length);
  void finish(uint8_t out[kHashBytes]);

 private:
  alignas(8) uint8_t ctx_[64];  // mbedtls_md_context_t, kept opaque here
  bool ok_ = false;
};

// HKDF-SHA256 (RFC 5869). No salt = HashLen zero bytes.
void hkdf(const uint8_t* salt, size_t saltLength, const uint8_t* ikm, size_t ikmLength, const uint8_t* info,
          size_t infoLength, uint8_t* out, size_t outLength);

// Constant-time comparison.
bool equal(const uint8_t* a, const uint8_t* b, size_t length);

// AES-128-CCM with a 13-byte nonce and an 8-byte tag. seal() writes
// `length` + 8 bytes; open() takes the ciphertext with its tag and writes
// `length` - 8 bytes. `out` may be `in`.
bool seal(const uint8_t key[kKeyBytes], const uint8_t nonce[kNonceBytes], const uint8_t* aad, size_t aadLength,
          const uint8_t* in, size_t length, uint8_t* out);
bool open(const uint8_t key[kKeyBytes], const uint8_t nonce[kNonceBytes], const uint8_t* aad, size_t aadLength,
          const uint8_t* in, size_t length, uint8_t* out);

// SPAKE2's w for a code: SHA-256("blat code" ‖ salt ‖ code) mod n, as 32
// big-endian bytes.
bool codeToW(const uint8_t* salt, size_t saltLength, const char* code, uint8_t w[kScalarBytes]);

// SPAKE2 (RFC 9382 construction) on P-256 with SHA-256, HKDF and HMAC, as
// party B (the device). Given w and A's share pA, makes B's share pB and
// both key confirmations, and the shared key Ke. False if pA isn't a valid
// point.
struct Spake2Keys {
  uint8_t pB[kPointBytes];
  uint8_t confirmA[kHashBytes];  // what A must send
  uint8_t confirmB[kHashBytes];  // what B sends
  uint8_t ke[16];
};
bool spake2Respond(const uint8_t w[kScalarBytes], const uint8_t pA[kPointBytes], const uint8_t* idA, size_t idALength,
                   const uint8_t* idB, size_t idBLength, const uint8_t* aad, size_t aadLength, Spake2Keys& out);

}  // namespace blat::crypto
