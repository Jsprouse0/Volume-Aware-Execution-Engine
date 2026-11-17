#include "tcp_socket.hpp"

#include <array>
#include <charconv>
#include <stdexcept>
#include <utility>

namespace slipstream {
namespace {

struct Input {
    Feed feed;
    tcp::Socket listener;
    tcp::Socket peer;
    StreamDecoder decoder;

    Input(Feed stream, const Endpoint& endpoint)
        : feed(stream), listener(tcp::listen(endpoint)) {}

    tcp::Handle readable_handle() const {
        return listener ? listener.native() : peer.native();
    }

    const char* name() const {
        return feed == Feed::quotes ? "MD" : "OE";
    }

    void read_ready(const FeedHandler& on_message) {
        if (listener) {
            peer = tcp::accept(listener);
            listener.close();
            return;
        }

        std::array<std::byte, 4096> buffer;
        const auto bytes = tcp::receive(peer, buffer);

        if (bytes.empty()) {
            decoder.finish();
            peer.close();
            return;
        }

        decoder.feed(bytes, [&](const Message& message) {
            const bool expected = feed == Feed::quotes
                ? std::holds_alternative<Quote>(message)
                : std::holds_alternative<Trade>(message);

            if (!expected) {
                throw ProtocolError("unexpected message type for feed");
            }

            on_message(feed, message);
        });
    }
};

} // namespace

TcpFeedTransport::TcpFeedTransport(Endpoint md, Endpoint oe)
    : md_(std::move(md)), oe_(std::move(oe)) {}

void TcpFeedTransport::run(const FeedHandler& on_message, const ListenHandler& on_listen) {
    tcp::Runtime runtime;
    std::array inputs{Input{Feed::quotes, md_}, Input{Feed::trades, oe_}};
    on_listen(tcp::local_port(inputs[0].listener), tcp::local_port(inputs[1].listener));

    for (;;) {
        const std::array handles{inputs[0].readable_handle(), inputs[1].readable_handle()};

        if (handles[0] == tcp::invalid_handle && handles[1] == tcp::invalid_handle) {
            return;
        }

        const auto ready = tcp::wait_readable(handles);

        // One read per ready feed per turn. A silent peer cannot stall its neighbor.
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            if (!ready[i]) {
                continue;
            }

            try {
                inputs[i].read_ready(on_message);
            } catch (const std::exception& error) {
                throw std::runtime_error(std::string(inputs[i].name()) + ": " + error.what());
            }
        }
    }
}

std::uint16_t parse_port(std::string_view text) {
    if (text.empty()) {
        throw std::invalid_argument("port must be an integer in 1..65535");
    }

    std::uint16_t port{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), port);

    if (error != std::errc{} || end != text.data() + text.size() || port == 0) {
        throw std::invalid_argument("port must be an integer in 1..65535");
    }

    return port;
}

std::size_t replay_tcp(const CsvData& data, const ReplayOptions& options,
                       ReplayClock clock, const Endpoint& destination,
                       const StreamDecoder::Handler& on_oe_message) {
    tcp::Runtime runtime;
    const auto socket = tcp::connect(destination);
    const auto count = replay(data, options, std::move(clock), [&](auto frame) {
        tcp::send_all(socket, frame);
    });

    tcp::finish_sending(socket);

    if (options.feed == Feed::trades) {
        OeReceiver receiver;
        std::array<std::byte, 4096> buffer;

        // M2 sends no application responses. Keep the read half open so
        // the same connection can carry inbound order frames.
        for (auto bytes = tcp::receive(socket, buffer); !bytes.empty();
             bytes = tcp::receive(socket, buffer)) {
            receiver.receive(bytes, on_oe_message);
        }

        receiver.finish();
    }

    return count;
}

} // namespace slipstream
