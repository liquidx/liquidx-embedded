#include "SdSpace.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <SDCardManager.h>

#include <cstring>
#include <new>

namespace {

constexpr uint32_t kSectorSize = 512;
constexpr uint32_t kChunkSectors = 64;  // 32 KB per read
constexpr unsigned long kStepBudgetMs = 40;  // per step(): keys wait at most this long

uint16_t le16(const uint8_t* p) { return p[0] | (p[1] << 8); }
uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

bool isFat32Boot(const uint8_t* s) {
  return le16(s + 510) == 0xAA55 && le16(s + 11) == kSectorSize && s[13] != 0 && memcmp(s + 82, "FAT32   ", 8) == 0;
}

}  // namespace

bool SdSpaceScan::step() {
  switch (state_) {
    case State::Start:
      if (!start()) {
        // Not FAT32 (or unreadable): let SdFat count it.
        if (Storage.ready()) {
          finish(true, SDCardManager::getInstance().sdUsedBytes());
        } else {
          finish(false, 0);
        }
      }
      return done();

    case State::Scanning: {
      uint8_t* s = buf_.get();
      const unsigned long stepStart = millis();
      while (state_ == State::Scanning && millis() - stepStart < kStepBudgetMs) {
        if (!dev_->readSectors(sector_, s, kChunkSectors)) {
          finish(false, 0);
          break;
        }
        sector_ += kChunkSectors;
        const uint32_t count = kSectorSize * kChunkSectors / 4;
        for (uint32_t i = 0; i < count && entry_ < entries_; i++, entry_++) {
          // Entries 0 and 1 are reserved; clusters are numbered from 2.
          if (entry_ >= 2 && (le32(s + i * 4) & 0x0FFFFFFF) == 0) free_++;
        }
        if (entry_ >= entries_) {
          finish(true, static_cast<uint64_t>(clusters_ - free_) * perCluster_ * kSectorSize);
        }
      }
      return done();
    }

    case State::Done:
      return true;
  }
  return true;
}

// Find the FAT32 volume and set up the scan. False if there isn't one.
bool SdSpaceScan::start() {
  startMs_ = millis();
  if (!Storage.ready()) return false;
  dev_ = SDCardManager::getInstance().rawBlockDevice();
  if (dev_ == nullptr) return false;
  buf_.reset(new (std::nothrow) uint8_t[kSectorSize * kChunkSectors]);
  if (!buf_) return false;
  uint8_t* s = buf_.get();

  // The volume boot sector: sector 0 on a superfloppy, else the first FAT32
  // partition in the MBR.
  uint32_t start = 0;
  if (!dev_->readSector(0, s)) return false;
  if (!isFat32Boot(s)) {
    if (le16(s + 510) != 0xAA55) return false;
    for (int i = 0; i < 4 && start == 0; i++) {
      const uint8_t* entry = s + 446 + i * 16;
      if (entry[4] == 0x0B || entry[4] == 0x0C) start = le32(entry + 8);
    }
    if (start == 0 || !dev_->readSector(start, s) || !isFat32Boot(s)) return false;
  }

  perCluster_ = s[13];
  const uint32_t reserved = le16(s + 14);
  const uint32_t fats = s[16];
  const uint32_t totalSectors = le16(s + 19) ? le16(s + 19) : le32(s + 32);
  const uint32_t fatSectors = le32(s + 36);
  clusters_ = (totalSectors - reserved - fats * fatSectors) / perCluster_;
  entries_ = clusters_ + 2;
  sector_ = start + reserved;
  entry_ = free_ = 0;
  state_ = State::Scanning;
  LOG_INF("SD", "FAT32: %u clusters of %u KB, FAT is %u sectors", clusters_, perCluster_ / 2, fatSectors);
  return true;
}

void SdSpaceScan::finish(const bool ok, const uint64_t used) {
  ok_ = ok;
  used_ = used;
  state_ = State::Done;
  buf_.reset();
  LOG_INF("SD", "Used-space scan %s in %lu ms", ok ? "done" : "failed", millis() - startMs_);
}
