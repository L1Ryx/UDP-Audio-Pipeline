#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <system_error>

namespace udp_audio::transport {

struct Endpoint {
  std::string address = "127.0.0.1";
  std::uint16_t port = 0;

  [[nodiscard]] static Endpoint loopback(std::uint16_t port) {
    return Endpoint{.address = "127.0.0.1", .port = port};
  }

  [[nodiscard]] static Endpoint any(std::uint16_t port) {
    return Endpoint{.address = "0.0.0.0", .port = port};
  }
};

struct ReceiveResult {
  std::size_t bytes_received = 0;
  Endpoint remote{};
};

class UdpSocket {
 public:
  UdpSocket() = default;
  ~UdpSocket();

  UdpSocket(const UdpSocket&) = delete;
  UdpSocket& operator=(const UdpSocket&) = delete;

  UdpSocket(UdpSocket&& other) noexcept;
  UdpSocket& operator=(UdpSocket&& other) noexcept;

  [[nodiscard]] static UdpSocket open_ipv4(std::error_code& error) noexcept;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::intptr_t native_handle() const noexcept;

  bool bind(const Endpoint& endpoint, std::error_code& error) noexcept;
  bool set_non_blocking(bool enabled, std::error_code& error) noexcept;

  [[nodiscard]] std::optional<Endpoint> local_endpoint(std::error_code& error) const noexcept;

  [[nodiscard]] std::size_t send_to(std::span<const std::byte> bytes,
                                    const Endpoint& endpoint,
                                    std::error_code& error) noexcept;

  [[nodiscard]] std::optional<ReceiveResult> receive_from(std::span<std::byte> buffer,
                                                          std::error_code& error) noexcept;

  void close() noexcept;

 private:
  explicit UdpSocket(std::intptr_t fd) noexcept;

  std::intptr_t fd_ = -1;
};

}  // namespace udp_audio::transport

