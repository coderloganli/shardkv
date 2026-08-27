#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace shardkv {

// RESP2 encoders. Each appends to `out` rather than returning a string, so a
// connection builds a whole reply -- or a pipelined run of them -- in its own
// write buffer without an intermediate allocation.
namespace resp {

void encodeSimpleString(std::string_view s, std::string& out);
void encodeError(std::string_view message, std::string& out);
void encodeInteger(std::int64_t value, std::string& out);
void encodeBulkString(std::string_view s, std::string& out);

// $-1\r\n -- a missing value. Distinct from an empty bulk string ($0\r\n\r\n),
// and clients rely on the difference.
void encodeNullBulkString(std::string& out);

// A nullopt element becomes a null bulk string, which is how MGET reports the
// keys it did not find.
void encodeArray(const std::vector<std::optional<std::string>>& elements,
                 std::string& out);

}  // namespace resp
}  // namespace shardkv
