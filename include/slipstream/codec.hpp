#pragma once
#include "slipstream/messages.hpp"
#include <array>
#include <cstddef>
#include <functional>
#include <span>
#include <stdexcept>

namespace slipstream {
class ProtocolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
inline constexpr std::size_t header_size = 4;
inline constexpr std::size_t max_frame_size = 54;
std::uint8_t message_type(const Message& message);
std::size_t encoded_size(const Message& message);
// Returns bytes_written. Throws if output is too small or fields are invalid.
std::size_t encode(const Message& message, std::span<std::byte> output);
Message decode(std::span<const std::byte> frame);

// One decoder per stream. A chunk may contain any number of partial/full frames.
// Delivers complete messages synchronously; keeps at most one partial frame.
class StreamDecoder {
public:
    using Handler = std::function<void(const Message&)>;
    void feed(std::span<const std::byte> chunk, const Handler& on_message);
    void finish() const; // Call on EOF: a partial frame is a truncated stream.
    std::size_t buffered_bytes() const { return used_; }
private:
    std::array<std::byte, max_frame_size> buffer_{};
    std::size_t used_{};
    std::size_t target_{header_size};
    bool failed_{};
};
} // namespace slipstream
