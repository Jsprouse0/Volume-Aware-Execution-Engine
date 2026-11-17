#pragma once

#include "slipstream/replay.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace slipstream {

struct Endpoint {
    std::string host{"127.0.0.1"};
    std::uint16_t port{};
};

using FeedHandler = std::function<void(Feed, const Message&)>;
using ListenHandler = std::function<void(std::uint16_t md_port, std::uint16_t oe_port)>;

class FeedTransport {
public:
    virtual ~FeedTransport() = default;

    // Synchronous, single consumer. Returns after both feeds reach clean EOF.
    virtual void run(const FeedHandler& on_message, const ListenHandler& on_listen) = 0;
};

class TcpFeedTransport final : public FeedTransport {
public:
    TcpFeedTransport(Endpoint md, Endpoint oe);
    void run(const FeedHandler& on_message, const ListenHandler& on_listen) override;

private:
    Endpoint md_;
    Endpoint oe_;
};

std::uint16_t parse_port(std::string_view text);

// Use the M1 replay scheduler and change only where the encoded bytes go.
std::size_t replay_tcp(const CsvData& data, const ReplayOptions& options,
                       ReplayClock clock, const Endpoint& destination,
                       const StreamDecoder::Handler& on_oe_message);

} // namespace slipstream
