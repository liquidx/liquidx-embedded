#pragma once

#include <blat.h>
#include <blit.h>

// The two protocols' GATT services, for Radio: it adds them to its server and
// forwards connects and disconnects. Firmware only (NimBLE).
namespace cast {
blit::NimBleLink& link();
}
namespace remote {
blat::NimBleLink& link();
}
