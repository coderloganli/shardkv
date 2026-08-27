#pragma once

#include <optional>
#include <string>

#include "store/clock.h"

namespace shardkv {

// A stored value: bytes, and optionally a deadline after which the key is gone.
//
// Bytes and nothing else -- there is no cached integer form, even though the
// counter commands would benefit. See
// docs/adr/0004-values-are-byte-strings.md: an integer representation is only
// safe when the text is the canonical decimal form, which puts an invariant on
// every command that writes a value, and gets its revenge at GET rather than at
// INCR. `SET k 001` must still read back as `001`.
//
// The deadline is here from the start even though only the lazy half of expiry
// is implemented, so that adding the sampled half later does not change the
// data structure.
struct Value {
  std::string data;
  std::optional<TimePoint> expires_at;
};

}  // namespace shardkv
