#pragma once

#include "slipstream/feed_transport.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <array>
#include <cstddef>
#include <span>

namespace slipstream::tcp {

#ifdef _WIN32
using Handle = SOCKET;
inline constexpr Handle invalid_handle = INVALID_SOCKET;
#else
using Handle = int;
inline constexpr Handle invalid_handle = -1;
#endif

class Runtime {
public:
    Runtime();
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
};

class Socket {
public:
    Socket() = default;
    explicit Socket(Handle handle) : handle_(handle) {}
    ~Socket();

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    Handle native() const { return handle_; }
    explicit operator bool() const { return handle_ != invalid_handle; }
    void close() noexcept;

private:
    Handle handle_{invalid_handle};
};

Socket listen(const Endpoint& endpoint);
Socket accept(const Socket& listener);
Socket connect(const Endpoint& endpoint);
std::uint16_t local_port(const Socket& socket);

// Wait with no timer. An invalid handle represents a feed that is finished.
std::array<bool, 2> wait_readable(std::array<Handle, 2> handles);

std::span<const std::byte> receive(const Socket& socket, std::span<std::byte> buffer);
void send_all(const Socket& socket, std::span<const std::byte> bytes);
void finish_sending(const Socket& socket);

} // namespace slipstream::tcp
