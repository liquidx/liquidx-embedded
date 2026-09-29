#pragma once

#include <cstddef>
#include <cstdint>

#include "Controls.h"

namespace blat {

// Encode a table as the schema blob (PROTOCOL.md#schema). Returns the length;
// with `out` null, just measures. Returns 0 if `size` is too small.
size_t encodeSchema(const Table& table, uint8_t* out, size_t size);

// The id of a control's parent, or 0 when it's at the top level.
Id parentOf(const Table& table, Id id);

}  // namespace blat
