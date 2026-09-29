#pragma once

// blat: BLE Attributes Transfer (../PROTOCOL.md). See ../README.md.
#include "blat/Controls.h"
#include "blat/Device.h"
#include "blat/Store.h"
#include "blat/Values.h"
#include "blat/Wire.h"

#if defined(ARDUINO)
#include "blat/PrefsStore.h"
#endif
#if __has_include(<NimBLEDevice.h>)
#include "blat/NimBleLink.h"
#endif
