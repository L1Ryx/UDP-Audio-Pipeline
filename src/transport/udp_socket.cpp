#include "udp_audio/transport/udp_socket.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace udp_audio::transport {
namespace {

std::error_code last_socket_error() noexcept {
  return std::error_code(errno, std::generic_category());
}

sockaddr_in to_sockaddr(const Endpoint& endpoint, std::error_code& error) noexcept {
  sockaddr_in address{};
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
  address.sin_len = static_cast<std::uint8_t>(sizeof(address));
#endif
  address.sin_family = AF_INET;
  address.sin_port = htons(endpoint.port);

  if (inet_pton(AF_INET, endpoint.address.c_str(), &address.sin_addr) != 1) {
    error = last_socket_error();
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

UdpSocket::UdpSocket(int fd) noexcept : fd_(fd) {}

UdpSocket::~UdpSocket() {
  close();
}

UdpSocket::UdpSocket(UdpSocket&& other) noexcept : fd_(other.fd_) {
  other.fd_ = -1;
}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
  if (this != &other) {
    close();
    fd_ = other.fd_;
    other.fd_ = -1;
  }
  return *this;
}

UdpSocket UdpSocket::open_ipv4(std::error_code& error) noexcept {
  error.clear();
  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    error = last_socket_error();
    return UdpSocket{};
  }

  UdpSocket socket(fd);
  if (!socket.set_non_blocking(true, error)) {
    socket.close();
    return UdpSocket{};
  }

  return socket;
}

bool UdpSocket::valid() const noexcept {
  return fd_ >= 0;
}

int UdpSocket::native_handle() const noexcept {
  return fd_;
}

bool UdpSocket::bind(const Endpoint& endpoint, std::error_code& error) noexcept {
  error.clear();
  auto address = to_sockaddr(endpoint, error);
  if (error) {
    return false;
  }

  const auto* raw = reinterpret_cast<const sockaddr*>(&address);
  if (::bind(fd_, raw, sizeof(address)) != 0) {
    error = last_socket_error();
    return false;
  }

  return true;
}

bool UdpSocket::set_non_blocking(bool enabled, std::error_code& error) noexcept {
  error.clear();
  const int flags = fcntl(fd_, F_GETFL, 0);
  if (flags < 0) {
    error = last_socket_error();
    return false;
  }

  const int next_flags = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
  if (fcntl(fd_, F_SETFL, next_flags) != 0) {
    error = last_socket_error();
    return false;
  }

  return true;
}

std::optional<Endpoint> UdpSocket::local_endpoint(std::error_code& error) const noexcept {
  error.clear();
  sockaddr_in address{};
  socklen_t length = sizeof(address);
  auto* raw = reinterpret_cast<sockaddr*>(&address);

  if (getsockname(fd_, raw, &length) != 0) {
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
  const auto sent = ::sendto(fd_, bytes.data(), bytes.size(), 0, raw, sizeof(address));
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
  socklen_t remote_length = sizeof(remote);
  auto* raw = reinterpret_cast<sockaddr*>(&remote);
  const auto received = ::recvfrom(fd_, buffer.data(), buffer.size(), 0, raw, &remote_length);

  if (received < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return std::nullopt;
    }

    error = last_socket_error();
    return std::nullopt;
  }

  return ReceiveResult{
    .bytes_received = static_cast<std::size_t>(received),
    .remote = from_sockaddr(remote),
  };
}

void UdpSocket::close() noexcept {
  if (fd_ >= 0) {
    static_cast<void>(::close(fd_));
    fd_ = -1;
  }
}

}  // namespace udp_audio::transport
