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

#include <map>

#include "commands/router.h"
#include "net/message.h"
#include "net/reply_slots.h"
#include "store/hash.h"
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

}  // namespace

std::string formatDecimal(std::int64_t value) {
  std::array<char, 24> buf{};
  const auto result = std::to_chars(buf.data(), buf.data() + buf.size(), value);
  return std::string(buf.data(), result.ptr);
}

namespace {

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
  const std::string text = formatDecimal(result);
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
  body += "shard0_keys:" + formatDecimal(static_cast<std::int64_t>(shard.size())) + "\r\n";
  body += "\r\n# Clients\r\n";
  // The live count, supplied by the loop -- a shard holds keys and has never
  // heard of a socket. With one loop this is the only count there is; the name
  // is already indexed so that N loops need no new format.
  body += "loop0_connections:" +
          formatDecimal(static_cast<std::int64_t>(stats.connections)) + "\r\n";
  body += "loop0_short_writes:" +
          formatDecimal(static_cast<std::int64_t>(stats.short_writes)) + "\r\n";
  body += "loop0_peer_gone_writes:" +
          formatDecimal(static_cast<std::int64_t>(stats.peer_gone_writes)) + "\r\n";
  body += "loop0_read_pauses:" +
          formatDecimal(static_cast<std::int64_t>(stats.read_pauses)) + "\r\n";
  body += "loop0_accept_failures:" +
          formatDecimal(static_cast<std::int64_t>(stats.accept_failures)) + "\r\n";
  resp::encodeBulkString(body, out);
}

// Executes one command against one shard. Knows nothing about routing,
// threads or slots -- this is the whole of the single-threaded server's
// behaviour, unchanged, and both the local path and an arriving cross-shard
// request go through it.
}  // namespace

AfterCommand runOnShard(Shard& shard, const std::vector<std::string_view>& argv,
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

// ------------------------------------------------------------- routing
//
// Everything above executes against one shard and knows nothing about threads.
// Everything below decides which shard, and what to do when the answer is not
// this one's to give.

namespace {

// Commands that operate on exactly one key, which is always argv[1], with the
// argument counts each accepts.
//
// The counts are here, and not left to the owning shard to discover, because a
// command that cannot be right must not travel: the design says validation
// happens before any message is sent, and `GET k extra` routed to another loop
// only to come back an error is a round trip spent on a question already
// answerable.
const std::map<std::string, std::vector<std::size_t>>& singleKeyArities() {
  static const std::map<std::string, std::vector<std::size_t>> kArities = {
      {"set", {3, 5}},   {"get", {2}},     {"getset", {3}},  {"append", {3}},
      {"strlen", {2}},   {"incr", {2}},    {"decr", {2}},    {"incrby", {3}},
      {"decrby", {3}},   {"type", {2}},    {"expire", {3}},  {"ttl", {2}},
      {"persist", {2}},
  };
  return kArities;
}

bool isSingleKeyCommand(const std::string& name) {
  return singleKeyArities().count(name) != 0;
}

// Whether this argument count is one the command accepts at all. A count that
// is wrong is answered here; a count that is right is still checked again on
// the shard, which owns the finer syntax (SET's EX keyword, for instance).
bool singleKeyArityAccepted(const std::string& name, std::size_t argc) {
  const auto it = singleKeyArities().find(name);
  if (it == singleKeyArities().end()) return true;
  for (const std::size_t accepted : it->second) {
    if (argc == accepted) return true;
  }
  return false;
}

// Commands answered here, without touching any shard.
bool isConnectivityCommand(const std::string& name) {
  return name == "ping" || name == "echo" || name == "quit" || name == "command";
}

// Groups the keys of a multi-key command by the shard that owns them, keeping
// each key's original position so the reply can be rebuilt in the order the
// client wrote it.
struct Group {
  std::vector<std::string> argv;
  std::vector<std::uint32_t> indices;
};

std::map<std::size_t, Group> groupByShard(const std::vector<std::string_view>& argv,
                                          std::size_t first, std::size_t stride,
                                          std::size_t shards) {
  std::map<std::size_t, Group> groups;
  std::uint32_t position = 0;
  for (std::size_t i = first; i < argv.size(); i += stride, ++position) {
    Group& group = groups[shardForKey(argv[i], shards)];
    // Owned copies, always. The read buffer these views point into may be
    // reused long before the reply comes back.
    for (std::size_t j = 0; j < stride; ++j) {
      group.argv.emplace_back(argv[i + j]);
    }
    group.indices.push_back(position);
  }
  return groups;
}

// Everything in the INFO body that does not depend on a loop.
//
// Which is very little, and that is the point. Every counter here used to be
// taken from whichever loop happened to answer and printed under a fixed
// "loop0_" label -- wrong twice over, in the value and in the name. They now
// arrive with the fan-out, and only the version and the shard count, which no
// loop owns, are written here.
std::string infoHeader(std::size_t shards) {
  std::string body;
  body += "# Server\r\n";
  body += "shardkv_version:0.2.0\r\n";
  // From the router, which is the thing that actually decides how many shards
  // there are. Taking it from LoopStats meant two sources for one fact, and two
  // sources can disagree: a router over four shards with default stats reported
  // one while faithfully fanning out over four.
  body += "shards:" + formatDecimal(static_cast<std::int64_t>(shards)) + "\r\n";
  return body;
}

// The numbers a loop knows about itself, gathered for the fan-out.
LoopFacts factsFrom(const LoopStats& stats, std::size_t shards) {
  LoopFacts facts;
  facts.connections = stats.connections;
  facts.loops = shards;
  facts.short_writes = stats.short_writes;
  facts.peer_gone_writes = stats.peer_gone_writes;
  facts.cross_shard_requests = stats.cross_shard_requests;
  facts.read_pauses = stats.read_pauses;
  facts.accept_failures = stats.accept_failures;
  facts.pinned = stats.pinned;
  return facts;
}

// Delivers straight into a slot, for work whose keys turned out to be local.
struct SlotDeliverer {
  ReplySlots* slots;
  std::uint32_t slot;

  void whole(std::string resp) { slots->fill(slot, std::move(resp)); }
  void part(std::uint32_t index, std::optional<std::string> value) {
    slots->contribute(slot, index, std::move(value));
  }
  void count(std::int64_t n) { slots->contributeCount(slot, n); }
  void status() { slots->contributeCount(slot, 0); }
};

}  // namespace

void executeCrossShardRequest(Shard& shard, const CrossShardRequest& request,
                              ReplySlots& slots, LoopFacts facts) {
  SlotDeliverer deliverer{&slots, request.slot};
  runCrossShardRequest(shard, request, facts, deliverer);
}

// Per-loop, not global: a message already carries the loop it came from, so
// (loop, counter) is unique with no shared atomic. thread_local is exactly
// per-loop, since a loop is a thread.
std::uint64_t nextConnectionIdForTest() {
  static thread_local std::uint64_t counter = 0;
  return ++counter;
}

AfterCommand dispatch(ShardRouter& router, ReplySlots& slots, std::uint32_t slot,
                      std::uint64_t conn_id, const std::vector<std::string_view>& argv,
                      const LoopStats& stats) {
  // An empty inline line. Not a command, and nothing is owed -- but the slot was
  // reserved, so it has to be closed out or the connection stalls behind it.
  if (argv.empty()) {
    slots.fill(slot, "");
    return AfterCommand::kKeepOpen;
  }

  const std::string name = lowered(argv[0]);
  const std::size_t shards = router.shardCount();

  // Answered here: no key, so no shard.
  if (isConnectivityCommand(name)) {
    std::string out;
    const AfterCommand after = runOnShard(router.local(), argv, out, stats);
    slots.fill(slot, std::move(out));
    return after;
  }

  auto sendGroup = [&](std::size_t shard, Delivery delivery, std::vector<std::string> group_argv,
                       std::vector<std::uint32_t> indices, std::uint32_t group_tag) {
    CrossShardRequest request;
    request.origin_loop = router.localShard();
    request.conn_id = conn_id;
    request.slot = slot;
    request.group = group_tag;
    request.delivery = delivery;
    request.argv = std::move(group_argv);
    request.indices = std::move(indices);
    router.send(shard, std::move(request));
  };

  if (isSingleKeyCommand(name)) {
    // A command that cannot be right does not travel. This covers both too few
    // arguments to have a key at all and too many to be the command it claims
    // to be.
    if (!singleKeyArityAccepted(name, argv.size())) {
      std::string out;
      runOnShard(router.local(), argv, out, stats);
      slots.fill(slot, std::move(out));
      return AfterCommand::kKeepOpen;
    }

    const std::size_t owner = shardForKey(argv[1], shards);
    if (owner == router.localShard()) {
      std::string out;
      runOnShard(router.local(), argv, out, stats);
      slots.fill(slot, std::move(out));
      return AfterCommand::kKeepOpen;
    }

    std::vector<std::string> owned;
    owned.reserve(argv.size());
    for (const auto& part : argv) owned.emplace_back(part);
    sendGroup(owner, Delivery::kWhole, std::move(owned), {}, 0);
    return AfterCommand::kKeepOpen;
  }

  // ---------------------------------------------------- multi-key commands
  //
  // Arity and syntax are checked here, before a single group is sent, so a
  // rejected command reaches no shard at all.

  const bool is_mget = name == "mget";
  const bool is_mset = name == "mset";
  const bool is_del = name == "del";
  const bool is_exists = name == "exists";

  if (is_mget || is_mset || is_del || is_exists) {
    if (argv.size() < 2 || (is_mset && (argv.size() < 3 || (argv.size() - 1) % 2 != 0))) {
      std::string out;
      wrongArity(argv[0], out);
      slots.fill(slot, std::move(out));
      return AfterCommand::kKeepOpen;
    }

    const std::size_t stride = is_mset ? 2 : 1;
    auto groups = groupByShard(argv, 1, stride, shards);

    Aggregate aggregate;
    aggregate.remaining = static_cast<std::uint32_t>(groups.size());
    if (is_mget) {
      aggregate.kind = AggregateKind::kArray;
      aggregate.parts.resize(argv.size() - 1);
      // Per KEY, not per group: an array aggregate completes when every element
      // has arrived, and one group may carry several of them. Counting groups
      // here finished the aggregate on a group's first element and truncated
      // the rest -- which is what MGET spanning one shard did until a test
      // caught it.
      aggregate.remaining = static_cast<std::uint32_t>(argv.size() - 1);
    } else if (is_mset) {
      aggregate.kind = AggregateKind::kStatus;
    } else {
      aggregate.kind = AggregateKind::kCount;
    }
    slots.beginAggregate(slot, std::move(aggregate));

    const Delivery delivery = is_mget   ? Delivery::kArrayParts
                              : is_mset ? Delivery::kStatus
                                        : Delivery::kCount;
    const std::uint32_t tag = is_del ? kDeleteGroup : 0;

    for (auto& [shard, group] : groups) {
      if (shard == router.localShard()) {
        CrossShardRequest local;
        local.slot = slot;
        local.group = tag;
        local.delivery = delivery;
        local.argv = std::move(group.argv);
        local.indices = std::move(group.indices);
        SlotDeliverer deliverer{&slots, slot};
        runCrossShardRequest(router.local(), local, factsFrom(stats, shards),
                             deliverer);
      } else {
        sendGroup(shard, delivery, std::move(group.argv), std::move(group.indices), tag);
      }
    }
    return AfterCommand::kKeepOpen;
  }

  // ------------------------------------------------------ fan-out commands
  //
  // Every shard, every time. A DBSIZE that answered for the local shard alone
  // would not be slow, it would be wrong.

  if (name == "dbsize" || name == "flushdb" || name == "info") {
    if (argv.size() != 1) {
      std::string out;
      wrongArity(argv[0], out);
      slots.fill(slot, std::move(out));
      return AfterCommand::kKeepOpen;
    }

    Aggregate aggregate;
    aggregate.remaining = static_cast<std::uint32_t>(shards);
    if (name == "dbsize") {
      aggregate.kind = AggregateKind::kCount;
    } else if (name == "flushdb") {
      aggregate.kind = AggregateKind::kStatus;
    } else {
      aggregate.kind = AggregateKind::kInfo;
      // Six numbers per loop, flat: field * loops + loop. One vector rather
      // than six, because the slot machinery counts contributions and has no
      // opinion about what they mean.
      aggregate.parts.resize(shards * static_cast<std::size_t>(InfoField::kCount));
      aggregate.header = infoHeader(shards);
      aggregate.remaining =
          static_cast<std::uint32_t>(shards * static_cast<std::size_t>(InfoField::kCount));
    }
    slots.beginAggregate(slot, std::move(aggregate));

    const Delivery delivery = (name == "flushdb") ? Delivery::kStatus
                              : (name == "info")  ? Delivery::kLoopInfo
                                                  : Delivery::kCount;

    // Remote first, local last, and the order matters for INFO.
    //
    // A loop's own cross_shard_requests is read when it contributes its part.
    // Contributing in shard order meant reading the counter partway through
    // issuing this very INFO's sends, so the number depended on where the
    // client's loop sat in the ordering -- loop 0 reported none of them, loop 3
    // reported three. Sending everything first makes the reading the same
    // wherever the connection landed.
    for (std::size_t shard = 0; shard < shards; ++shard) {
      if (shard == router.localShard()) continue;
      CrossShardRequest request;
      request.origin_loop = router.localShard();
      request.conn_id = conn_id;
      request.slot = slot;
      request.delivery = delivery;
      if (name == "info") request.indices.push_back(static_cast<std::uint32_t>(shard));
      router.send(shard, std::move(request));
    }

    {
      CrossShardRequest local;
      local.slot = slot;
      local.delivery = delivery;
      if (name == "info") {
        local.indices.push_back(static_cast<std::uint32_t>(router.localShard()));
      }
      SlotDeliverer deliverer{&slots, slot};
      runCrossShardRequest(router.local(), local, factsFrom(stats, shards),
                           deliverer);
    }
    return AfterCommand::kKeepOpen;
  }

  // Everything else, HELLO included.
  std::string out;
  runOnShard(router.local(), argv, out, stats);
  slots.fill(slot, std::move(out));
  return AfterCommand::kKeepOpen;
}

}  // namespace shardkv
