#include "proto/encoder.h"

#include <charconv>
#include <array>

namespace shardkv {
namespace resp {
namespace {

void appendNumber(std::int64_t value, std::string& out) {
  std::array<char, 24> buf{};
  const auto result = std::to_chars(buf.data(), buf.data() + buf.size(), value);
  out.append(buf.data(), result.ptr);
}

}  // namespace

void encodeSimpleString(std::string_view s, std::string& out) {
  out += '+';
  out.append(s);
  out += "\r\n";
}

void encodeError(std::string_view message, std::string& out) {
  out += '-';
  out.append(message);
  out += "\r\n";
}

void encodeInteger(std::int64_t value, std::string& out) {
  out += ':';
  appendNumber(value, out);
  out += "\r\n";
}

void encodeBulkString(std::string_view s, std::string& out) {
  out += '$';
  appendNumber(static_cast<std::int64_t>(s.size()), out);
  out += "\r\n";
  out.append(s);
  out += "\r\n";
}

// $-1: no value. An empty bulk string is $0\r\n\r\n, and the two must not be
// confused -- a client returns nil for one and "" for the other.
void encodeNullBulkString(std::string& out) { out += "$-1\r\n"; }

void encodeArray(const std::vector<std::optional<std::string>>& elements,
                 std::string& out) {
  out += '*';
  appendNumber(static_cast<std::int64_t>(elements.size()), out);
  out += "\r\n";
  for (const auto& element : elements) {
    if (element.has_value()) {
      encodeBulkString(*element, out);
    } else {
      encodeNullBulkString(out);
    }
  }
}

}  // namespace resp
}  // namespace shardkv
