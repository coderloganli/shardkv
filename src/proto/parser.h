#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

#include "proto/resp.h"

namespace shardkv {

// Parses one command from the front of `in`.
//
// A pure function: it touches no socket and no event loop, so it can be tested
// by feeding it byte sequences.
//
// argv_out is supplied by the caller and is cleared on entry. Owning it here
// rather than returning it is what keeps the steady state free of allocation:
// a Connection reuses one vector for the life of the connection, so its
// capacity is reached once and never grown again. (The payload bytes are never
// copied at all -- see the lifetime rule below.)
//
// LIFETIME -- READ THIS BEFORE USING argv_out
//
// The string_views in argv_out point INTO `in`. They are valid only until the
// read buffer is next mutated, and both of the things a connection routinely
// does count as mutation:
//
//   * appending freshly read bytes, which may reallocate
//   * consuming the prefix of a finished command
//
// So the order inside a connection is fixed, and is not a matter of taste:
//
//   parse -> execute (copying anything that must outlive the call) -> encode
//   the reply -> consume the bytes
//
// No command handler may retain an argv view after it returns. The same rule
// reappears once there are several threads: a cross-shard message has to own a
// copy of its key rather than borrow one.
//
// Returns kOk, kNeedMore or kProtocolError; consumed_out is set only for kOk.
ParseStatus parse(std::string_view in,
                  std::vector<std::string_view>& argv_out,
                  std::size_t& consumed_out);

}  // namespace shardkv
