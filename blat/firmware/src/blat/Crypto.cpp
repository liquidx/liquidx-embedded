#include "Crypto.h"

#include <mbedtls/ccm.h>
#include <mbedtls/ecp.h>
#include <mbedtls/md.h>

#include <cstring>
#include <new>

namespace blat::crypto {

namespace {

RandomFn randomFn = nullptr;

// SPAKE2's M and N for P-256 (RFC 9382 section 6), uncompressed.
constexpr uint8_t kM[kPointBytes] = {
    0x04, 0x88, 0x6e, 0x2f, 0x97, 0xac, 0xe4, 0x6e, 0x55, 0xba, 0x9d, 0xd7, 0x24, 0x25, 0x79, 0xf2, 0x99,
    0x3b, 0x64, 0xe1, 0x6e, 0xf3, 0xdc, 0xab, 0x95, 0xaf, 0xd4, 0x97, 0x33, 0x3d, 0x8f, 0xa1, 0x2f, 0x5f,
    0xf3, 0x55, 0x16, 0x3e, 0x43, 0xce, 0x22, 0x4e, 0x0b, 0x0e, 0x65, 0xff, 0x02, 0xac, 0x8e, 0x5c, 0x7b,
    0xe0, 0x94, 0x19, 0xc7, 0x85, 0xe0, 0xca, 0x54, 0x7d, 0x55, 0xa1, 0x2e, 0x2d, 0x20};
constexpr uint8_t kN[kPointBytes] = {
    0x04, 0xd8, 0xbb, 0xd6, 0xc6, 0x39, 0xc6, 0x29, 0x37, 0xb0, 0x4d, 0x99, 0x7f, 0x38, 0xc3, 0x77, 0x07,
    0x19, 0xc6, 0x29, 0xd7, 0x01, 0x4d, 0x49, 0xa2, 0x4b, 0x4f, 0x98, 0xba, 0xa1, 0x29, 0x2b, 0x49, 0x07,
    0xd6, 0x0a, 0xa6, 0xbf, 0xad, 0xe4, 0x50, 0x08, 0xa6, 0x36, 0x33, 0x7f, 0x51, 0x68, 0xc6, 0x4d, 0x9b,
    0xd3, 0x60, 0x34, 0x80, 0x8c, 0xd5, 0x64, 0x49, 0x0b, 0x1e, 0x65, 0x6e, 0xdb, 0xe7};

const mbedtls_md_info_t* sha256Info() { return mbedtls_md_info_from_type(MBEDTLS_MD_SHA256); }

int mbedRandom(void*, unsigned char* out, const size_t length) {
  random(out, length);
  return 0;
}

// The transcript's length-prefixed pieces: u64 little-endian length, then the bytes.
uint8_t* lengthPrefixed(uint8_t* p, const uint8_t* data, const size_t length) {
  for (int i = 0; i < 8; i++) *p++ = i < 4 ? static_cast<uint8_t>(length >> (8 * i)) : 0;
  memcpy(p, data, length);
  return p + length;
}

// Scoped mbedTLS objects.
struct Group {
  mbedtls_ecp_group g;
  Group() {
    mbedtls_ecp_group_init(&g);
    ok = mbedtls_ecp_group_load(&g, MBEDTLS_ECP_DP_SECP256R1) == 0;
  }
  ~Group() { mbedtls_ecp_group_free(&g); }
  bool ok;
};
struct Point {
  mbedtls_ecp_point p;
  Point() { mbedtls_ecp_point_init(&p); }
  ~Point() { mbedtls_ecp_point_free(&p); }
};
struct Mpi {
  mbedtls_mpi m;
  Mpi() { mbedtls_mpi_init(&m); }
  ~Mpi() { mbedtls_mpi_free(&m); }
};

bool writePoint(const Group& grp, const Point& pt, uint8_t out[kPointBytes]) {
  size_t length = 0;
  return mbedtls_ecp_point_write_binary(&grp.g, &pt.p, MBEDTLS_ECP_PF_UNCOMPRESSED, &length, out, kPointBytes) ==
             0 &&
         length == kPointBytes;
}

}  // namespace

void setRandom(const RandomFn fn) { randomFn = fn; }

void random(uint8_t* out, const size_t length) {
  if (randomFn != nullptr) {
    randomFn(out, length);
  } else {
    memset(out, 0, length);  // no source set: a bug, and obviously so
  }
}

void sha256(const uint8_t* data, const size_t length, uint8_t out[kHashBytes]) {
  mbedtls_md(sha256Info(), data, length, out);
}

static_assert(sizeof(mbedtls_md_context_t) <= 64, "Hmac::ctx_ is too small for this mbedTLS");

Hmac::Hmac(const uint8_t* key, const size_t keyLength) {
  auto* ctx = new (ctx_) mbedtls_md_context_t;
  mbedtls_md_init(ctx);
  ok_ = mbedtls_md_setup(ctx, sha256Info(), 1) == 0 && mbedtls_md_hmac_starts(ctx, key, keyLength) == 0;
}

Hmac::~Hmac() { mbedtls_md_free(reinterpret_cast<mbedtls_md_context_t*>(ctx_)); }

void Hmac::update(const void* data, const size_t length) {
  if (ok_) mbedtls_md_hmac_update(reinterpret_cast<mbedtls_md_context_t*>(ctx_), static_cast<const uint8_t*>(data), length);
}

void Hmac::finish(uint8_t out[kHashBytes]) {
  if (!ok_ || mbedtls_md_hmac_finish(reinterpret_cast<mbedtls_md_context_t*>(ctx_), out) != 0) {
    memset(out, 0, kHashBytes);
  }
}

void hkdf(const uint8_t* salt, const size_t saltLength, const uint8_t* ikm, const size_t ikmLength,
          const uint8_t* info, const size_t infoLength, uint8_t* out, const size_t outLength) {
  static const uint8_t kZeros[kHashBytes] = {};
  uint8_t prk[kHashBytes];
  {
    Hmac extract(saltLength > 0 ? salt : kZeros, saltLength > 0 ? saltLength : kHashBytes);
    extract.update(ikm, ikmLength);
    extract.finish(prk);
  }
  uint8_t t[kHashBytes];
  size_t done = 0;
  for (uint8_t counter = 1; done < outLength; counter++) {
    Hmac expand(prk, sizeof(prk));
    if (counter > 1) expand.update(t, sizeof(t));
    expand.update(info, infoLength);
    expand.update(&counter, 1);
    expand.finish(t);
    const size_t n = outLength - done < kHashBytes ? outLength - done : kHashBytes;
    memcpy(out + done, t, n);
    done += n;
  }
  memset(prk, 0, sizeof(prk));
  memset(t, 0, sizeof(t));
}

bool equal(const uint8_t* a, const uint8_t* b, const size_t length) {
  uint8_t diff = 0;
  for (size_t i = 0; i < length; i++) diff |= a[i] ^ b[i];
  return diff == 0;
}

bool seal(const uint8_t key[kKeyBytes], const uint8_t nonce[kNonceBytes], const uint8_t* aad, const size_t aadLength,
          const uint8_t* in, const size_t length, uint8_t* out) {
  mbedtls_ccm_context ctx;
  mbedtls_ccm_init(&ctx);
  const bool ok = mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128) == 0 &&
                  mbedtls_ccm_encrypt_and_tag(&ctx, length, nonce, kNonceBytes, aad, aadLength, in, out, out + length,
                                              kTagBytes) == 0;
  mbedtls_ccm_free(&ctx);
  return ok;
}

bool open(const uint8_t key[kKeyBytes], const uint8_t nonce[kNonceBytes], const uint8_t* aad, const size_t aadLength,
          const uint8_t* in, const size_t length, uint8_t* out) {
  if (length < kTagBytes) return false;
  const size_t n = length - kTagBytes;
  uint8_t tag[kTagBytes];
  memcpy(tag, in + n, kTagBytes);  // `out` may overwrite `in`
  mbedtls_ccm_context ctx;
  mbedtls_ccm_init(&ctx);
  const bool ok = mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128) == 0 &&
                  mbedtls_ccm_auth_decrypt(&ctx, n, nonce, kNonceBytes, aad, aadLength, in, out, tag, kTagBytes) == 0;
  mbedtls_ccm_free(&ctx);
  return ok;
}

bool codeToW(const uint8_t* salt, const size_t saltLength, const char* code, uint8_t w[kScalarBytes]) {
  static const char kLabel[] = "blat code";
  uint8_t input[sizeof(kLabel) - 1 + 32 + 16];
  const size_t codeLength = strlen(code);
  if (saltLength > 32 || codeLength > 16) return false;
  uint8_t* p = input;
  memcpy(p, kLabel, sizeof(kLabel) - 1);
  p += sizeof(kLabel) - 1;
  memcpy(p, salt, saltLength);
  p += saltLength;
  memcpy(p, code, codeLength);
  p += codeLength;
  uint8_t digest[kHashBytes];
  sha256(input, p - input, digest);

  Group grp;
  Mpi h, reduced;
  const bool ok = grp.ok && mbedtls_mpi_read_binary(&h.m, digest, sizeof(digest)) == 0 &&
                  mbedtls_mpi_mod_mpi(&reduced.m, &h.m, &grp.g.N) == 0 &&
                  mbedtls_mpi_write_binary(&reduced.m, w, kScalarBytes) == 0;
  memset(digest, 0, sizeof(digest));
  return ok;
}

bool spake2Respond(const uint8_t w[kScalarBytes], const uint8_t pA[kPointBytes], const uint8_t* idA,
                   const size_t idALength, const uint8_t* idB, const size_t idBLength, const uint8_t* aad,
                   const size_t aadLength, Spake2Keys& out) {
  if (idALength > 32 || idBLength > 32 || aadLength > 32) return false;
  Group grp;
  Point M, N, A, T, B, K;
  Mpi wm, negW, y, one;
  bool ok = grp.ok && mbedtls_ecp_point_read_binary(&grp.g, &M.p, kM, kPointBytes) == 0 &&
            mbedtls_ecp_point_read_binary(&grp.g, &N.p, kN, kPointBytes) == 0 &&
            mbedtls_ecp_point_read_binary(&grp.g, &A.p, pA, kPointBytes) == 0 &&
            mbedtls_ecp_check_pubkey(&grp.g, &A.p) == 0 && mbedtls_mpi_read_binary(&wm.m, w, kScalarBytes) == 0 &&
            mbedtls_mpi_lset(&one.m, 1) == 0;
  // pB = y·G + w·N
  ok = ok && mbedtls_ecp_gen_privkey(&grp.g, &y.m, mbedRandom, nullptr) == 0 &&
       mbedtls_ecp_muladd(&grp.g, &B.p, &y.m, &grp.g.G, &wm.m, &N.p) == 0;
  // K = y·(pA − w·M) (cofactor 1). −w is n − w.
  ok = ok && mbedtls_mpi_sub_mpi(&negW.m, &grp.g.N, &wm.m) == 0 &&
       mbedtls_ecp_muladd(&grp.g, &T.p, &one.m, &A.p, &negW.m, &M.p) == 0 && mbedtls_ecp_is_zero(&T.p) == 0 &&
       mbedtls_ecp_mul(&grp.g, &K.p, &y.m, &T.p, mbedRandom, nullptr) == 0 && mbedtls_ecp_is_zero(&K.p) == 0;
  uint8_t kBytes[kPointBytes];
  ok = ok && writePoint(grp, B, out.pB) && writePoint(grp, K, kBytes);
  if (!ok) return false;

  // TT = len(A)‖A ‖ len(B)‖B ‖ len(pA)‖pA ‖ len(pB)‖pB ‖ len(K)‖K ‖ len(w)‖w
  uint8_t tt[6 * 8 + 32 + 32 + 3 * kPointBytes + kScalarBytes];
  uint8_t* p = tt;
  p = lengthPrefixed(p, idA, idALength);
  p = lengthPrefixed(p, idB, idBLength);
  p = lengthPrefixed(p, pA, kPointBytes);
  p = lengthPrefixed(p, out.pB, kPointBytes);
  p = lengthPrefixed(p, kBytes, kPointBytes);
  p = lengthPrefixed(p, w, kScalarBytes);
  const size_t ttLength = p - tt;

  // Ke ‖ Ka = Hash(TT); KcA ‖ KcB = KDF(nil, Ka, "ConfirmationKeys" ‖ AAD)
  uint8_t hash[kHashBytes];
  sha256(tt, ttLength, hash);
  memcpy(out.ke, hash, 16);
  static const char kInfo[] = "ConfirmationKeys";
  uint8_t info[sizeof(kInfo) - 1 + 32];
  memcpy(info, kInfo, sizeof(kInfo) - 1);
  memcpy(info + sizeof(kInfo) - 1, aad, aadLength);
  uint8_t kc[32];
  hkdf(nullptr, 0, hash + 16, 16, info, sizeof(kInfo) - 1 + aadLength, kc, sizeof(kc));

  // cA = MAC(KcA, TT), cB = MAC(KcB, TT)
  {
    Hmac mac(kc, 16);
    mac.update(tt, ttLength);
    mac.finish(out.confirmA);
  }
  {
    Hmac mac(kc + 16, 16);
    mac.update(tt, ttLength);
    mac.finish(out.confirmB);
  }
  memset(kBytes, 0, sizeof(kBytes));
  memset(hash, 0, sizeof(hash));
  memset(kc, 0, sizeof(kc));
  memset(tt, 0, sizeof(tt));
  return true;
}

}  // namespace blat::crypto
