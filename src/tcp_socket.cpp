#include "tcp_socket.hpp"

#ifndef _WIN32
#include <arpa/inet.h>
#include <cerrno>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace slipstream::tcp {
namespace {

int last_error() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool interrupted(int error) {
#ifdef _WIN32
    return error == WSAEINTR;
#else
    return error == EINTR;
#endif
}

[[noreturn]] void fail(const char* operation, int error = last_error()) {
#ifdef _WIN32
    throw std::system_error(error, std::system_category(), operation);
#else
    throw std::system_error(error, std::generic_category(), operation);
#endif
}

sockaddr_in ipv4_address(const Endpoint& endpoint) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(endpoint.port);

    if (inet_pton(AF_INET, endpoint.host.c_str(), &address.sin_addr) != 1) {
        throw std::invalid_argument("expected a numeric IPv4 address: " + endpoint.host);
    }

    return address;
}

Socket open_stream() {
    Socket socket{::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};

    if (!socket) {
        fail("socket");
    }

    return socket;
}

} // namespace

Runtime::Runtime() {
#ifdef _WIN32
    WSADATA data{};
    const int error = WSAStartup(MAKEWORD(2, 2), &data);

    if (error != 0) {
        fail("WSAStartup", error);
    }
#endif
}

Runtime::~Runtime() {
#ifdef _WIN32
    WSACleanup();
#endif
}

Socket::~Socket() {
    close();
}

Socket::Socket(Socket&& other) noexcept
    : handle_(std::exchange(other.handle_, invalid_handle)) {}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = std::exchange(other.handle_, invalid_handle);
    }

    return *this;
}

void Socket::close() noexcept {
    if (!*this) {
        return;
    }

#ifdef _WIN32
    ::closesocket(handle_);
#else
    ::close(handle_);
#endif

    handle_ = invalid_handle;
}

Socket listen(const Endpoint& endpoint) {
    const auto address = ipv4_address(endpoint);
    auto socket = open_stream();
    const int enabled = 1;

#ifdef _WIN32
    constexpr int bind_option = SO_EXCLUSIVEADDRUSE;
#else
    constexpr int bind_option = SO_REUSEADDR;
#endif

    if (::setsockopt(socket.native(), SOL_SOCKET, bind_option,
                     reinterpret_cast<const char*>(&enabled), sizeof(enabled)) != 0) {
        fail("setsockopt listener");
    }

    if (::bind(socket.native(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        fail("bind");
    }

    if (::listen(socket.native(), 1) != 0) {
        fail("listen");
    }

    return socket;
}

Socket accept(const Socket& listener) {
    for (;;) {
        Socket client{::accept(listener.native(), nullptr, nullptr)};

        if (client) {
            return client;
        }

        const int error = last_error();
        if (!interrupted(error)) {
            fail("accept", error);
        }
    }
}

Socket connect(const Endpoint& endpoint) {
    const auto address = ipv4_address(endpoint);
    auto socket = open_stream();

    if (::connect(socket.native(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        fail("connect");
    }

    return socket;
}

std::uint16_t local_port(const Socket& socket) {
    sockaddr_in address{};
#ifdef _WIN32
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif

    if (::getsockname(socket.native(), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        fail("getsockname");
    }

    return ntohs(address.sin_port);
}

std::array<bool, 2> wait_readable(std::array<Handle, 2> handles) {
    for (;;) {
#ifdef _WIN32
        fd_set reads;
        FD_ZERO(&reads);

        for (const auto handle : handles) {
            if (handle != invalid_handle) {
                FD_SET(handle, &reads);
            }
        }

        if (::select(0, &reads, nullptr, nullptr, nullptr) > 0) {
            return {handles[0] != invalid_handle && FD_ISSET(handles[0], &reads),
                    handles[1] != invalid_handle && FD_ISSET(handles[1], &reads)};
        }
#else
        pollfd reads[]{{handles[0], POLLIN, 0}, {handles[1], POLLIN, 0}};

        if (::poll(reads, 2, -1) > 0) {
            // HUP/ERR are also actionable: recv reveals EOF or the error.
            return {reads[0].revents != 0, reads[1].revents != 0};
        }
#endif

        const int error = last_error();
        if (!interrupted(error)) {
            fail("wait for readable TCP socket", error);
        }
    }
}

std::span<const std::byte> receive(const Socket& socket, std::span<std::byte> buffer) {
    for (;;) {
        const auto size = std::min(buffer.size(),
                                   static_cast<std::size_t>(std::numeric_limits<int>::max()));
        const auto received = ::recv(socket.native(), reinterpret_cast<char*>(buffer.data()),
                                     static_cast<int>(size), 0);

        if (received >= 0) {
            return buffer.first(static_cast<std::size_t>(received));
        }

        const int error = last_error();
        if (!interrupted(error)) {
            fail("recv", error);
        }
    }
}

void send_all(const Socket& socket, std::span<const std::byte> bytes) {
#ifdef _WIN32
    constexpr int flags = 0;
#else
    constexpr int flags = MSG_NOSIGNAL;
#endif

    while (!bytes.empty()) {
        const auto size = std::min(bytes.size(),
                                   static_cast<std::size_t>(std::numeric_limits<int>::max()));
        const auto sent = ::send(socket.native(), reinterpret_cast<const char*>(bytes.data()),
                                 static_cast<int>(size), flags);

        if (sent > 0) {
            bytes = bytes.subspan(static_cast<std::size_t>(sent));
        } else if (sent == 0) {
            throw std::runtime_error("send made no progress");
        } else {
            const int error = last_error();
            if (!interrupted(error)) {
                fail("send", error);
            }
        }
    }
}

void finish_sending(const Socket& socket) {
#ifdef _WIN32
    constexpr int send_side = SD_SEND;
#else
    constexpr int send_side = SHUT_WR;
#endif

    if (::shutdown(socket.native(), send_side) != 0) {
        fail("shutdown send side");
    }
}

} // namespace slipstream::tcp
