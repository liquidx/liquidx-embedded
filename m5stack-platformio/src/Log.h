#pragma once

#include <cstdio>

// Serial logging: LOG_INF("BLE", "MTU %u", mtu) prints "[BLE] MTU 517".
#if defined(ARDUINO)
#include <Arduino.h>
#define LOG_PRINT(...) Serial.printf(__VA_ARGS__)
#else
#define LOG_PRINT(...) printf(__VA_ARGS__)
#endif

#define LOG_INF(tag, fmt, ...) LOG_PRINT("[%s] " fmt "\n", tag, ##__VA_ARGS__)
#define LOG_ERR(tag, fmt, ...) LOG_PRINT("[%s] ERROR " fmt "\n", tag, ##__VA_ARGS__)
