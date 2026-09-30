#pragma once

// A short critical section between the BLE task and the main loop: a spinlock
// on ESP32 (safe across cores, never sleeps), a mutex elsewhere (tests).
#if defined(ESP_PLATFORM)
#include <freertos/FreeRTOS.h>
#else
#include <mutex>
#endif

namespace blit {

class Lock {
 public:
#if defined(ESP_PLATFORM)
  void lock() { portENTER_CRITICAL(&mux_); }
  void unlock() { portEXIT_CRITICAL(&mux_); }

 private:
  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
#else
  void lock() { mutex_.lock(); }
  void unlock() { mutex_.unlock(); }

 private:
  std::mutex mutex_;
#endif
};

}  // namespace blit
