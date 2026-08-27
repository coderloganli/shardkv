#pragma once

#include <cstddef>

namespace shardkv {

// The three answers a parse can give.
enum class ParseStatus {
  // A whole command was read. argv holds its parts, consumed says how many
  // bytes of the input it occupied.
  kOk,
  // The input is a prefix of a valid command. Read more and call again with
  // the longer input. consumed is 0.
  kNeedMore,
  // The input cannot be the start of any valid command. The stream can no
  // longer be resynchronised, so the connection is answered with an error and
  // closed.
  kProtocolError,
};

// Protocol limits, matching Redis. Anything larger is kProtocolError rather
// than an allocation.
inline constexpr std::size_t kMaxMultibulkElements = 1024 * 1024;
inline constexpr std::size_t kMaxBulkLength = 512 * 1024 * 1024;

}  // namespace shardkv
