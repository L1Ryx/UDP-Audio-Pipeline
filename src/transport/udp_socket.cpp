#include "udp_audio/transport/udp_socket.hpp"

#include <array>
#include <cerrno>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace udp_audio::transport {
namespace {

#if defined(_WIN32)
constexpr std::intptr_t kInvalidSocket = static_cast<std::intptr_t>(INVALID_SOCKET);

class WsaRuntime {
 public:
  WsaRuntime() noexcept {
    WSADATA data{};
    initialized_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }

  ~WsaRuntime() {
    if (initialized_) {
      WSACleanup();
    }
  }

  [[nodiscard]] bool initialized() const noexcept { return initialized_; }

 private:
  bool initialized_ = false;
};

bool ensure_socket_runtime(std::error_code& error) noexcept {
  static WsaRuntime runtime;
  if (!runtime.initialized()) {
    error = std::error_code(WSAENETDOWN, std::system_category());
    return false;
  }
  return true;
}

SOCKET to_socket(std::intptr_t fd) noexcept {
  return static_cast<SOCKET>(fd);
}

std::error_code last_socket_error() noexcept {
  return std::error_code(WSAGetLastError(), std::system_category());
}
#else
constexpr std::intptr_t kInvalidSocket = -1;

bool ensure_socket_runtime(std::error_code& error) noexcept {
  error.clear();
  return true;
}

int to_socket(std::intptr_t fd) noexcept {
  return static_cast<int>(fd);
}

std::error_code last_socket_error() noexcept {
  return std::error_code(errno, std::generic_category());
}
#endif

#if defined(_WIN32)
using SocketBufferLength = int;
#else
using SocketBufferLength = std::size_t;
#endif

SocketBufferLength socket_buffer_length(std::size_t size) noexcept {
  return static_cast<SocketBufferLength>(size);
}

sockaddr_in to_sockaddr(const Endpoint& endpoint, std::error_code& error) noexcept {
  sockaddr_in address{};
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
  address.sin_len = static_cast<std::uint8_t>(sizeof(address));
#endif
  address.sin_family = AF_INET;
  address.sin_port = htons(endpoint.port);

  const int converted = inet_pton(AF_INET, endpoint.address.c_str(), &address.sin_addr);
  if (converted != 1) {
    error = converted == 0 ? std::make_error_code(std::errc::invalid_argument)
                           : last_socket_error();
  }

  return address;
}

Endpoint from_sockaddr(const sockaddr_in& address) {
  std::array<char, INET_ADDRSTRLEN> buffer{};
  const char* converted = inet_ntop(AF_INET, &address.sin_addr, buffer.data(), buffer.size());

  return Endpoint{
    .address = converted == nullptr ? std::string{} : std::string(converted),
    .port = ntohs(address.sin_port),
  };
}

}  // namespace

UdpSocket::UdpSocket(std::intptr_t fd) noexcept : fd_(fd) {}

UdpSocket::~UdpSocket() {
  close();
}

UdpSocket::UdpSocket(UdpSocket&& other) noexcept : fd_(other.fd_) {
  other.fd_ = kInvalidSocket;
}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
  if (this != &other) {
    close();
    fd_ = other.fd_;
    other.fd_ = kInvalidSocket;
  }
  return *this;
}

UdpSocket UdpSocket::open_ipv4(std::error_code& error) noexcept {
  error.clear();
  if (!ensure_socket_runtime(error)) {
    return UdpSocket{};
  }

  const auto fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  const auto native = static_cast<std::intptr_t>(fd);
  if (native == kInvalidSocket) {
    error = last_socket_error();
    return UdpSocket{};
  }

  UdpSocket socket(native);
  if (!socket.set_non_blocking(true, error)) {
    socket.close();
    return UdpSocket{};
  }

  return socket;
}

bool UdpSocket::valid() const noexcept {
  return fd_ != kInvalidSocket;
}

std::intptr_t UdpSocket::native_handle() const noexcept {
  return fd_;
}

bool UdpSocket::bind(const Endpoint& endpoint, std::error_code& error) noexcept {
  error.clear();
  auto address = to_sockaddr(endpoint, error);
  if (error) {
    return false;
  }

  const auto* raw = reinterpret_cast<const sockaddr*>(&address);
  if (::bind(to_socket(fd_), raw, sizeof(address)) != 0) {
    error = last_socket_error();
    return false;
  }

  return true;
}

bool UdpSocket::set_non_blocking(bool enabled, std::error_code& error) noexcept {
  error.clear();
#if defined(_WIN32)
  u_long mode = enabled ? 1UL : 0UL;
  if (ioctlsocket(to_socket(fd_), FIONBIO, &mode) != 0) {
    error = last_socket_error();
    return false;
  }
#else
  const int flags = fcntl(to_socket(fd_), F_GETFL, 0);
  if (flags < 0) {
    error = last_socket_error();
    return false;
  }

  const int next_flags = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
  if (fcntl(to_socket(fd_), F_SETFL, next_flags) != 0) {
    error = last_socket_error();
    return false;
  }
#endif

  return true;
}

std::optional<Endpoint> UdpSocket::local_endpoint(std::error_code& error) const noexcept {
  error.clear();
  sockaddr_in address{};
#if defined(_WIN32)
  int length = sizeof(address);
#else
  socklen_t length = sizeof(address);
#endif
  auto* raw = reinterpret_cast<sockaddr*>(&address);

  if (getsockname(to_socket(fd_), raw, &length) != 0) {
    error = last_socket_error();
    return std::nullopt;
  }

  return from_sockaddr(address);
}

std::size_t UdpSocket::send_to(std::span<const std::byte> bytes,
                               const Endpoint& endpoint,
                               std::error_code& error) noexcept {
  error.clear();
  auto address = to_sockaddr(endpoint, error);
  if (error) {
    return 0;
  }

  const auto* raw = reinterpret_cast<const sockaddr*>(&address);
  const auto sent = ::sendto(to_socket(fd_), reinterpret_cast<const char*>(bytes.data()),
                             socket_buffer_length(bytes.size()), 0, raw, sizeof(address));
  if (sent < 0) {
    error = last_socket_error();
    return 0;
  }

  return static_cast<std::size_t>(sent);
}

std::optional<ReceiveResult> UdpSocket::receive_from(std::span<std::byte> buffer,
                                                     std::error_code& error) noexcept {
  error.clear();
  sockaddr_in remote{};
#if defined(_WIN32)
  int remote_length = sizeof(remote);
#else
  socklen_t remote_length = sizeof(remote);
#endif
  auto* raw = reinterpret_cast<sockaddr*>(&remote);
  const auto received = ::recvfrom(to_socket(fd_), reinterpret_cast<char*>(buffer.data()),
                                   socket_buffer_length(buffer.size()), 0, raw, &remote_length);

  if (received < 0) {
#if defined(_WIN32)
    if (WSAGetLastError() == WSAEWOULDBLOCK) {
      return std::nullopt;
    }
#else
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return std::nullopt;
    }
#endif

    error = last_socket_error();
    return std::nullopt;
  }

  return ReceiveResult{
    .bytes_received = static_cast<std::size_t>(received),
    .remote = from_sockaddr(remote),
  };
}

void UdpSocket::close() noexcept {
  if (fd_ != kInvalidSocket) {
#if defined(_WIN32)
    static_cast<void>(closesocket(to_socket(fd_)));
#else
    static_cast<void>(::close(to_socket(fd_)));
#endif
    fd_ = kInvalidSocket;
  }
}

}  // namespace udp_audio::transport
