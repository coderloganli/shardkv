#include "net/listener.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

namespace shardkv {
namespace {

[[noreturn]] void fail(const char* what) {
  throw std::runtime_error(std::string(what) + ": " + std::strerror(errno));
}

void setNonBlocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    fail("fcntl(O_NONBLOCK)");
  }
}

}  // namespace

Listener::Listener(std::uint16_t port) {
  fd_ = UniqueFd(::socket(AF_INET, SOCK_STREAM, 0));
  if (!fd_) fail("socket");

  const int one = 1;
  if (::setsockopt(fd_.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
    fail("setsockopt(SO_REUSEADDR)");
  }

  // Set now, though with a single listener it changes nothing observable. The
  // next task gives every loop its own listener on this same port and lets the
  // kernel hash arriving connections across them; this file should not have to
  // change for that.
  if (::setsockopt(fd_.get(), SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) < 0) {
    fail("setsockopt(SO_REUSEPORT)");
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = ::htons(port);
  addr.sin_addr.s_addr = ::htonl(INADDR_ANY);
  if (::bind(fd_.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    fail("bind");
  }

  if (::listen(fd_.get(), SOMAXCONN) < 0) fail("listen");

  // Read the port back, because 0 means the kernel chose one -- which is what
  // the tests bind so that concurrent runs do not collide.
  sockaddr_in bound{};
  socklen_t len = sizeof(bound);
  if (::getsockname(fd_.get(), reinterpret_cast<sockaddr*>(&bound), &len) < 0) {
    fail("getsockname");
  }
  port_ = ::ntohs(bound.sin_port);

  setNonBlocking(fd_.get());
}

int Listener::fd() const { return fd_.get(); }

std::uint16_t Listener::port() const { return port_; }

UniqueFd Listener::accept() {
  const int fd = ::accept(fd_.get(), nullptr, nullptr);
  if (fd < 0) {
    // Nothing pending, or the connection died between the event and the
    // accept. Neither is an error worth reporting.
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED ||
        errno == EINTR) {
      return UniqueFd();
    }
    // EMFILE and ENFILE land here: the process is out of descriptors. The
    // caller drops the event and carries on rather than dying, so that a
    // descriptor limit degrades service instead of ending it.
    return UniqueFd();
  }
  setNonBlocking(fd);
  return UniqueFd(fd);
}

}  // namespace shardkv
