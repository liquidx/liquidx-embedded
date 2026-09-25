#pragma once

#include <FsLib/FsLib.h>

#include <cstdint>
#include <memory>

// Measures used space on the SD card without blocking the UI.
//
// Used space means counting free clusters across the whole FAT, which on a
// large FAT32 card is megabytes of reads (seconds at the X4C's 1-bit SDMMC
// speed). This reads the FAT straight from the block device in multi-sector
// chunks, a time-boxed batch per step(), so the main loop can keep handling
// keys between them. Other filesystems (exFAT's allocation bitmap is small) are
// counted by SdFat in one go.
class SdSpaceScan {
 public:
  // Scan for up to ~40 ms. Returns true once finished; then ok() and usedBytes().
  bool step();
  bool done() const { return state_ == State::Done; }
  bool ok() const { return ok_; }
  uint64_t usedBytes() const { return used_; }

 private:
  enum class State : uint8_t { Start, Scanning, Done };

  bool start();
  void finish(bool ok, uint64_t used);

  State state_ = State::Start;
  bool ok_ = false;
  uint64_t used_ = 0;

  FsBlockDeviceInterface* dev_ = nullptr;
  std::unique_ptr<uint8_t[]> buf_;
  uint32_t sector_ = 0;      // next FAT sector to read
  uint32_t entry_ = 0;       // next FAT entry to count
  uint32_t entries_ = 0;     // clusters + 2 reserved entries
  uint32_t clusters_ = 0;
  uint32_t perCluster_ = 0;  // sectors per cluster
  uint32_t free_ = 0;
  unsigned long startMs_ = 0;
};
