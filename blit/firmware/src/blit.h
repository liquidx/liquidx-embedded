#pragma once

// blit: pixels to low-power displays (../PROTOCOL.md). See ../README.md.
#include "blit/Bmp.h"
#include "blit/Frame.h"
#include "blit/Receiver.h"
#include "blit/Wire.h"

#if __has_include(<NimBLEDevice.h>)
#include "blit/NimBleLink.h"
#endif
