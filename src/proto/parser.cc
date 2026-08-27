#include "proto/parser.h"

#include <cctype>
#include <cstdint>
#include <limits>
#include <optional>

namespace shardkv {
namespace {

constexpr std::string_view kCrlf = "\r\n";

// Reads a CRLF-terminated line starting at `pos`.
//
// Returns nullopt when the terminator has not arrived yet -- which the caller
// turns into kNeedMore, not an error. A bare LF is a malformed header rather
// than a short read: the byte after the content is present and it is not CR.
enum class LineStatus { kOk, kNeedMore, kMalformed };

LineStatus readLine(std::string_view in, std::size_t pos, std::string_view& line,
                    std::size_t& next) {
  const std::size_t lf = in.find('\n', pos);
  if (lf == std::string_view::npos) return LineStatus::kNeedMore;
  if (lf == pos || in[lf - 1] != '\r') return LineStatus::kMalformed;
  line = in.substr(pos, lf - 1 - pos);
  next = lf + 1;
  return LineStatus::kOk;
}

// A non-negative decimal integer, with no sign, no leading plus and no leading
// whitespace. Redis rejects "*+3" and so does this.
//
// Overflow is a parse failure rather than a wrap: the value is a length, and a
// wrapped length is how a parser is talked into reading someone else's memory.
std::optional<std::int64_t> parseCount(std::string_view s) {
  if (s.empty()) return std::nullopt;

  std::size_t i = 0;
  bool negative = false;
  if (s[0] == '-') {
    negative = true;
    i = 1;
    if (s.size() == 1) return std::nullopt;
  }

  std::int64_t value = 0;
  for (; i < s.size(); ++i) {
    const char c = s[i];
    if (c < '0' || c > '9') return std::nullopt;
    const int digit = c - '0';
    if (value > (std::numeric_limits<std::int64_t>::max() - digit) / 10) {
      return std::nullopt;
    }
    value = value * 10 + digit;
  }
  return negative ? -value : value;
}

bool isInlineSpace(char c) { return c == ' ' || c == '\t'; }

// An inline command: whitespace-separated words ending at the first newline.
// Detected because no RESP request begins with '*', which is what the protocol
// specification says the server should key on.
ParseStatus parseInline(std::string_view in, std::vector<std::string_view>& argv,
                        std::size_t& consumed) {
  const std::size_t lf = in.find('\n');
  if (lf == std::string_view::npos) return ParseStatus::kNeedMore;

  // Tolerate a bare LF here: telnet sends it, and the specification offers the
  // inline form precisely so that a telnet session works.
  std::size_t end = lf;
  if (end > 0 && in[end - 1] == '\r') --end;

  std::string_view line = in.substr(0, end);
  std::size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && isInlineSpace(line[i])) ++i;
    const std::size_t start = i;
    while (i < line.size() && !isInlineSpace(line[i])) ++i;
    if (i > start) argv.push_back(line.substr(start, i - start));
  }

  // An empty line yields an empty argv. The caller skips it: not a command, not
  // an error, and nothing to reply to.
  consumed = lf + 1;
  return ParseStatus::kOk;
}

}  // namespace

ParseStatus parse(std::string_view in, std::vector<std::string_view>& argv_out,
                  std::size_t& consumed_out) {
  argv_out.clear();
  consumed_out = 0;

  if (in.empty()) return ParseStatus::kNeedMore;
  if (in[0] != '*') return parseInline(in, argv_out, consumed_out);

  std::size_t pos = 0;
  std::string_view line;
  std::size_t next = 0;

  switch (readLine(in, pos, line, next)) {
    case LineStatus::kNeedMore:
      return ParseStatus::kNeedMore;
    case LineStatus::kMalformed:
      return ParseStatus::kProtocolError;
    case LineStatus::kOk:
      break;
  }

  const auto count = parseCount(line.substr(1));
  if (!count.has_value() || *count < 0) return ParseStatus::kProtocolError;
  if (static_cast<std::size_t>(*count) > kMaxMultibulkElements) {
    return ParseStatus::kProtocolError;
  }
  pos = next;

  // Deliberately NOT argv_out.reserve(*count).
  //
  // The declared count is attacker-controlled, and `*1048576\r\n` with no
  // elements behind it would reserve a million string_views -- sixteen
  // megabytes of metadata per connection, for a command that has not arrived
  // and may never arrive. Repeat across connections and that is the whole
  // machine.
  //
  // It is the same rule the payload already follows: nothing is sized by a
  // declared length until the bytes are actually here. A small reserve is
  // enough to keep the ordinary case free of growth, and the vector is reused
  // for the life of the connection anyway, so it reaches its working size once.
  constexpr std::size_t kArgvReserveHint = 8;
  argv_out.reserve(std::min(static_cast<std::size_t>(*count), kArgvReserveHint));

  for (std::int64_t element = 0; element < *count; ++element) {
    switch (readLine(in, pos, line, next)) {
      case LineStatus::kNeedMore:
        argv_out.clear();
        return ParseStatus::kNeedMore;
      case LineStatus::kMalformed:
        argv_out.clear();
        return ParseStatus::kProtocolError;
      case LineStatus::kOk:
        break;
    }

    if (line.empty() || line[0] != '$') {
      argv_out.clear();
      return ParseStatus::kProtocolError;
    }

    const auto length = parseCount(line.substr(1));
    if (!length.has_value() || *length < 0) {
      argv_out.clear();
      return ParseStatus::kProtocolError;
    }
    if (static_cast<std::size_t>(*length) > kMaxBulkLength) {
      argv_out.clear();
      return ParseStatus::kProtocolError;
    }

    const std::size_t body = static_cast<std::size_t>(*length);
    const std::size_t body_start = next;

    // The declared length is checked against what has actually arrived. Nothing
    // is allocated on the strength of it: a client may say 400MB, and until
    // 400MB is in the buffer this is simply kNeedMore.
    //
    // The terminator is judged byte by byte as those bytes arrive, rather than
    // waiting for both. If the first one is present and is not CR, no
    // continuation can make the frame valid, so it is an error now -- waiting
    // for a second byte would be waiting to confirm something already known.
    const std::size_t terminator = body_start + body;
    if (in.size() > terminator && in[terminator] != '\r') {
      argv_out.clear();
      return ParseStatus::kProtocolError;
    }
    if (in.size() < terminator + kCrlf.size()) {
      argv_out.clear();
      return ParseStatus::kNeedMore;
    }
    if (in[terminator + 1] != '\n') {
      argv_out.clear();
      return ParseStatus::kProtocolError;
    }

    argv_out.push_back(in.substr(body_start, body));
    pos = body_start + body + kCrlf.size();
  }

  consumed_out = pos;
  return ParseStatus::kOk;
}

}  // namespace shardkv
