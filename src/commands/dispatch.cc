#include "commands/dispatch.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include "proto/encoder.h"

namespace shardkv {
namespace {

using std::chrono::duration_cast;
using std::chrono::milliseconds;
using std::chrono::seconds;

constexpr std::string_view kNotAnInteger =
    "ERR value is not an integer or out of range";
constexpr std::string_view kInvalidExpire =
    "ERR invalid expire time in 'set' command";
constexpr std::string_view kInvalidExpireExpire =
    "ERR invalid expire time in 'expire' command";
constexpr std::string_view kSyntaxError = "ERR syntax error";

// The largest TTL accepted, in seconds: a hundred years, comfortably longer
// than any process will run and comfortably short of anything that could
// overflow the clock.
//
// Without a bound this is an attacker-controlled overflow, not a nicety:
// `SET k v EX 9223372036854775807` adds that many seconds to a
// steady_clock::time_point, whose duration is nanoseconds on this platform, so
// the multiplication alone is signed overflow -- undefined behaviour, and in
// practice a deadline in the past, which quietly deletes the key it was asked
// to preserve.
constexpr std::int64_t kMaxExpireSeconds = 100LL * 365 * 24 * 60 * 60;

std::string lowered(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

// A command name safe to quote back inside an error reply.
//
// The name is whatever bytes the client sent, and a bulk string may contain
// any of them -- CR and LF included. Splicing it raw into a simple error is
// response-line injection: `*1\r\n$12\r\nBAD\r\nINJECT\r\n` is a perfectly
// legal request whose name would end the error line early and let the sender
// dictate the bytes a client reads as the next reply.
//
// So every byte outside printable ASCII becomes \xHH, and the whole thing is
// capped: an error message is not the place to echo back a megabyte.
std::string quotable(std::string_view name) {
  constexpr std::size_t kMaxEchoed = 128;
  std::string out;
  out.reserve(std::min(name.size(), kMaxEchoed));

  for (std::size_t i = 0; i < name.size() && i < kMaxEchoed; ++i) {
    const unsigned char c = static_cast<unsigned char>(name[i]);
    if (c >= 0x20 && c < 0x7f && c != '\'' && c != '\\') {
      out += static_cast<char>(c);
    } else {
      static constexpr char kHex[] = "0123456789abcdef";
      out += "\\x";
      out += kHex[c >> 4];
      out += kHex[c & 0x0f];
    }
  }
  if (name.size() > kMaxEchoed) out += "...";
  return out;
}

void wrongArity(std::string_view command, std::string& out) {
  std::string message = "ERR wrong number of arguments for '";
  message += quotable(lowered(command));
  message += "' command";
  resp::encodeError(message, out);
}

// A value usable as a counter: a canonical signed decimal that round-trips
// exactly. "001" and "+1" are rejected, matching Redis -- they are legitimate
// stored values, just not numbers this may operate on.
std::optional<std::int64_t> asCounter(std::string_view s) {
  if (s.empty()) return std::nullopt;
  std::int64_t value = 0;
  const auto* begin = s.data();
  const auto* end = s.data() + s.size();
  const auto result = std::from_chars(begin, end, value);
  if (result.ec != std::errc{} || result.ptr != end) return std::nullopt;

  // Reject any spelling that is not the canonical one, so that reading a value
  // back always yields the bytes that were stored.
  std::array<char, 24> buf{};
  const auto printed = std::to_chars(buf.data(), buf.data() + buf.size(), value);
  if (std::string_view(buf.data(), printed.ptr - buf.data()) != s) {
    return std::nullopt;
  }
  return value;
}

std::string formatted(std::int64_t value) {
  std::array<char, 24> buf{};
  const auto result = std::to_chars(buf.data(), buf.data() + buf.size(), value);
  return std::string(buf.data(), result.ptr);
}

// Checked add. Overflow is an error reply, never a wrap: a counter that silently
// wraps is worse than one that refuses.
bool addChecked(std::int64_t a, std::int64_t b, std::int64_t& out) {
  if (b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) return false;
  if (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b) return false;
  out = a + b;
  return true;
}

void applyDelta(Shard& shard, std::string_view key, std::int64_t delta,
                std::string& out) {
  std::int64_t current = 0;
  if (Value* value = shard.lookup(key); value != nullptr) {
    const auto parsed = asCounter(value->data);
    if (!parsed.has_value()) {
      resp::encodeError(kNotAnInteger, out);
      return;
    }
    current = *parsed;
  }

  std::int64_t result = 0;
  if (!addChecked(current, delta, result)) {
    resp::encodeError(kNotAnInteger, out);
    return;
  }

  // Preserve any TTL: INCR changes the value, not the deadline.
  const std::string text = formatted(result);
  if (Value* value = shard.lookup(key); value != nullptr) {
    value->data = text;
  } else {
    shard.set(key, text);
  }
  resp::encodeInteger(result, out);
}

// The delta commands share everything but their sign and their argument count.
void counterCommand(Shard& shard, const std::vector<std::string_view>& argv,
                    bool has_delta, int sign, std::string& out) {
  const std::size_t expected = has_delta ? 3 : 2;
  if (argv.size() != expected) {
    wrongArity(argv[0], out);
    return;
  }

  std::int64_t delta = 1;
  if (has_delta) {
    const auto parsed = asCounter(argv[2]);
    if (!parsed.has_value()) {
      resp::encodeError(kNotAnInteger, out);
      return;
    }
    delta = *parsed;
  }

  if (sign < 0) {
    if (delta == std::numeric_limits<std::int64_t>::min()) {
      resp::encodeError(kNotAnInteger, out);
      return;
    }
    delta = -delta;
  }
  applyDelta(shard, argv[1], delta, out);
}

void ttlSeconds(Shard& shard, std::string_view key, std::string& out) {
  Value* value = shard.lookup(key);
  if (value == nullptr) {
    resp::encodeInteger(-2, out);  // no such key
    return;
  }
  if (!value->expires_at.has_value()) {
    resp::encodeInteger(-1, out);  // exists, but never expires
    return;
  }
  // Rounded to nearest, not truncated. The specification does not spell the
  // rule out, but the documented example is unambiguous: EXPIRE k 10 followed
  // immediately by TTL k answers 10. Truncating answers 9 as soon as a
  // microsecond has passed, which is what this did until a real redis-cli
  // session showed it.
  const auto remaining_ms =
      duration_cast<milliseconds>(*value->expires_at - shard.clock().now()).count();
  resp::encodeInteger((remaining_ms + 500) / 1000, out);
}

void setCommand(Shard& shard, const std::vector<std::string_view>& argv,
                std::string& out) {
  if (argv.size() != 3 && argv.size() != 5) {
    wrongArity(argv[0], out);
    return;
  }

  std::optional<std::int64_t> expire;
  if (argv.size() == 5) {
    if (lowered(argv[3]) != "ex") {
      resp::encodeError(kSyntaxError, out);
      return;
    }
    const auto parsed = asCounter(argv[4]);
    if (!parsed.has_value()) {
      resp::encodeError(kNotAnInteger, out);
      return;
    }
    // Validated before anything is written: a rejected argument must not leave
    // the key set. The upper bound is what stops an attacker-supplied TTL from
    // overflowing the clock.
    if (*parsed <= 0 || *parsed > kMaxExpireSeconds) {
      resp::encodeError(kInvalidExpire, out);
      return;
    }
    expire = *parsed;
  }

  shard.set(argv[1], argv[2]);
  if (expire.has_value()) {
    shard.lookup(argv[1])->expires_at = shard.clock().now() + seconds(*expire);
  }
  resp::encodeSimpleString("OK", out);
}

void infoCommand(Shard& shard, const LoopStats& stats, std::string& out) {
  std::string body;
  body += "# Server\r\n";
  body += "shardkv_version:0.1.0\r\n";
  body += "shards:1\r\n";
  body += "\r\n# Keyspace\r\n";
  body += "shard0_keys:" + formatted(static_cast<std::int64_t>(shard.size())) + "\r\n";
  body += "\r\n# Clients\r\n";
  // The live count, supplied by the loop -- a shard holds keys and has never
  // heard of a socket. With one loop this is the only count there is; the name
  // is already indexed so that N loops need no new format.
  body += "loop0_connections:" +
          formatted(static_cast<std::int64_t>(stats.connections)) + "\r\n";
  body += "loop0_short_writes:" +
          formatted(static_cast<std::int64_t>(stats.short_writes)) + "\r\n";
  body += "loop0_peer_gone_writes:" +
          formatted(static_cast<std::int64_t>(stats.peer_gone_writes)) + "\r\n";
  resp::encodeBulkString(body, out);
}

}  // namespace

AfterCommand dispatch(Shard& shard, const std::vector<std::string_view>& argv,
                      std::string& out, const LoopStats& stats) {
  // An empty inline line. Not a command, so nothing is answered.
  if (argv.empty()) return AfterCommand::kKeepOpen;

  const std::string name = lowered(argv[0]);

  if (name == "ping") {
    if (argv.size() == 1) {
      resp::encodeSimpleString("PONG", out);
    } else if (argv.size() == 2) {
      resp::encodeBulkString(argv[1], out);
    } else {
      wrongArity(argv[0], out);
    }
  } else if (name == "echo") {
    if (argv.size() != 2) {
      wrongArity(argv[0], out);
    } else {
      resp::encodeBulkString(argv[1], out);
    }
  } else if (name == "quit") {
    resp::encodeSimpleString("OK", out);
    return AfterCommand::kClose;
  } else if (name == "command") {
    // Answered rather than refused, cheaply, for client compatibility. Whether
    // a real client hangs without it is a separate question, checked by hand.
    resp::encodeArray({}, out);
  } else if (name == "set") {
    setCommand(shard, argv, out);
  } else if (name == "get") {
    if (argv.size() != 2) {
      wrongArity(argv[0], out);
    } else if (Value* value = shard.lookup(argv[1]); value != nullptr) {
      resp::encodeBulkString(value->data, out);
    } else {
      resp::encodeNullBulkString(out);
    }
  } else if (name == "getset") {
    if (argv.size() != 3) {
      wrongArity(argv[0], out);
    } else {
      std::optional<std::string> previous;
      if (Value* value = shard.lookup(argv[1]); value != nullptr) {
        previous = value->data;
      }
      shard.set(argv[1], argv[2]);
      if (previous.has_value()) {
        resp::encodeBulkString(*previous, out);
      } else {
        resp::encodeNullBulkString(out);
      }
    }
  } else if (name == "append") {
    if (argv.size() != 3) {
      wrongArity(argv[0], out);
    } else if (Value* value = shard.lookup(argv[1]); value != nullptr) {
      value->data.append(argv[2]);
      resp::encodeInteger(static_cast<std::int64_t>(value->data.size()), out);
    } else {
      shard.set(argv[1], argv[2]);
      resp::encodeInteger(static_cast<std::int64_t>(argv[2].size()), out);
    }
  } else if (name == "strlen") {
    if (argv.size() != 2) {
      wrongArity(argv[0], out);
    } else if (Value* value = shard.lookup(argv[1]); value != nullptr) {
      resp::encodeInteger(static_cast<std::int64_t>(value->data.size()), out);
    } else {
      resp::encodeInteger(0, out);
    }
  } else if (name == "incr") {
    counterCommand(shard, argv, /*has_delta=*/false, 1, out);
  } else if (name == "decr") {
    counterCommand(shard, argv, /*has_delta=*/false, -1, out);
  } else if (name == "incrby") {
    counterCommand(shard, argv, /*has_delta=*/true, 1, out);
  } else if (name == "decrby") {
    counterCommand(shard, argv, /*has_delta=*/true, -1, out);
  } else if (name == "del") {
    if (argv.size() < 2) {
      wrongArity(argv[0], out);
    } else {
      std::int64_t deleted = 0;
      for (std::size_t i = 1; i < argv.size(); ++i) {
        // Through lookup() first, so an already-expired key is not counted as
        // a deletion the caller caused.
        if (shard.lookup(argv[i]) != nullptr && shard.erase(argv[i])) ++deleted;
      }
      resp::encodeInteger(deleted, out);
    }
  } else if (name == "exists") {
    if (argv.size() < 2) {
      wrongArity(argv[0], out);
    } else {
      std::int64_t found = 0;
      // Occurrences, not distinct keys: EXISTS k k is 2 in Redis.
      for (std::size_t i = 1; i < argv.size(); ++i) {
        if (shard.lookup(argv[i]) != nullptr) ++found;
      }
      resp::encodeInteger(found, out);
    }
  } else if (name == "type") {
    if (argv.size() != 2) {
      wrongArity(argv[0], out);
    } else if (shard.lookup(argv[1]) != nullptr) {
      resp::encodeSimpleString("string", out);
    } else {
      resp::encodeSimpleString("none", out);
    }
  } else if (name == "dbsize") {
    if (argv.size() != 1) {
      wrongArity(argv[0], out);
    } else {
      resp::encodeInteger(static_cast<std::int64_t>(shard.size()), out);
    }
  } else if (name == "flushdb") {
    if (argv.size() != 1) {
      wrongArity(argv[0], out);
    } else {
      shard.clear();
      resp::encodeSimpleString("OK", out);
    }
  } else if (name == "mget") {
    if (argv.size() < 2) {
      wrongArity(argv[0], out);
    } else {
      std::vector<std::optional<std::string>> values;
      values.reserve(argv.size() - 1);
      for (std::size_t i = 1; i < argv.size(); ++i) {
        if (Value* value = shard.lookup(argv[i]); value != nullptr) {
          values.emplace_back(value->data);
        } else {
          values.emplace_back(std::nullopt);
        }
      }
      resp::encodeArray(values, out);
    }
  } else if (name == "mset") {
    // Checked before the first write, so a bad call sets nothing at all.
    if (argv.size() < 3 || (argv.size() - 1) % 2 != 0) {
      wrongArity(argv[0], out);
    } else {
      for (std::size_t i = 1; i + 1 < argv.size(); i += 2) {
        shard.set(argv[i], argv[i + 1]);
      }
      resp::encodeSimpleString("OK", out);
    }
  } else if (name == "expire") {
    if (argv.size() != 3) {
      wrongArity(argv[0], out);
    } else {
      const auto ttl = asCounter(argv[2]);
      if (!ttl.has_value()) {
        resp::encodeError(kNotAnInteger, out);
      } else if (*ttl > kMaxExpireSeconds) {
        // Same overflow guard as SET ... EX. Checked before the key is looked
        // up, so the answer does not depend on whether the key happens to
        // exist.
        resp::encodeError(kInvalidExpireExpire, out);
      } else if (Value* value = shard.lookup(argv[1]); value == nullptr) {
        resp::encodeInteger(0, out);
      } else if (*ttl <= 0) {
        shard.erase(argv[1]);  // Redis deletes on a non-positive TTL
        resp::encodeInteger(1, out);
      } else {
        value->expires_at = shard.clock().now() + seconds(*ttl);
        resp::encodeInteger(1, out);
      }
    }
  } else if (name == "ttl") {
    if (argv.size() != 2) {
      wrongArity(argv[0], out);
    } else {
      ttlSeconds(shard, argv[1], out);
    }
  } else if (name == "persist") {
    if (argv.size() != 2) {
      wrongArity(argv[0], out);
    } else {
      Value* value = shard.lookup(argv[1]);
      if (value != nullptr && value->expires_at.has_value()) {
        value->expires_at.reset();
        resp::encodeInteger(1, out);
      } else {
        resp::encodeInteger(0, out);
      }
    }
  } else if (name == "info") {
    infoCommand(shard, stats, out);
  } else {
    // Everything else, HELLO included. The connection stays open: this error is
    // the protocol's documented way for a client to learn the server speaks
    // only RESP2 and carry on.
    std::string message = "ERR unknown command '";
    message += quotable(argv[0]);
    message += "'";
    resp::encodeError(message, out);
  }

  return AfterCommand::kKeepOpen;
}

}  // namespace shardkv
