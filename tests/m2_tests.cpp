#include "slipstream/market.hpp"
#include "tcp_socket.hpp"

#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {

using namespace slipstream;
using namespace std::chrono_literals;

void check(bool condition, const char* description) {
    if (!condition) {
        throw std::runtime_error(description);
    }
}

Trade trade(std::uint64_t ts, std::uint32_t qty, std::int64_t px) {
    return {"SYNTH4", ts, qty, px, '?', 1};
}

void window_tests() {
    RollingVwap window(300);
    check(!window.price(), "empty window must have no VWAP");

    window.add(trade(10, 50, 341900));
    window.add(trade(310, 10, 341800));
    check(window.volume() == 60 && window.price() == 341883, "left endpoint and weighted price");

    window.add(trade(310, 5, 341800));
    check(window.size() == 3 && window.volume() == 65, "equal timestamps are distinct prints");

    window.add(trade(311, 0, 0));
    check(window.volume() == 15 && window.price() == 341800, "zero-size tick advances time and evicts");

    window.add(trade(611, 0, 0));
    check(!window.price(), "long gap empties window");

    bool rejected = false;
    try {
        window.add(trade(610, 1, 1));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "regressing event time must be rejected");

    RollingVwap extreme(10);
    constexpr auto qty = std::numeric_limits<std::uint32_t>::max();
    constexpr auto high = std::numeric_limits<std::int64_t>::max();
    constexpr auto low = std::numeric_limits<std::int64_t>::min();
    extreme.add(trade(1, qty, high));
    check(extreme.price() == high, "multiply in 128 bits before adding");
    extreme.add(trade(2, qty, low));
    check(extreme.price() == 0, "signed notional and truncation toward zero");
    extreme.add(trade(12, 0, 0));
    check(extreme.price() == low, "wide signed eviction");
}

void capacity_tests() {
    RollingVwap window(1);
    window.add(trade(0, 1, 100));

    for (std::size_t i = 1; i < RollingVwap::capacity; ++i) {
        window.add(trade(1, 1, 200));
    }

    bool rejected = false;
    try {
        window.add(trade(1, 1, 300));
    } catch (const std::overflow_error&) {
        rejected = true;
    }

    check(rejected && window.size() == RollingVwap::capacity, "ring must refuse loss of a live print");
    window.add(trade(2, 1, 200)); // Evict head, then write at its old slot.
    check(window.size() == RollingVwap::capacity && window.price() == 200, "wrapped tail after one eviction");
    window.add(trade(4, 3, 400)); // Evict through the wrapped slot.
    check(window.size() == 1 && window.volume() == 3 && window.price() == 400, "evict through wrap");
}

void sample_test(const char* path) {
    std::ifstream input(path);
    const auto data = read_csv(input, parse_date_midnight("2026-10-27"));
    constexpr std::uint64_t duration = 30'000'000'000;
    MarketState market("SYNTH4", duration);
    std::vector<Trade> matching_trades;
    std::array<bool, 5> symbols{};
    std::optional<Quote> last_quote;

    for (const auto& row : data.rows) {
        if (const auto* quote = std::get_if<Quote>(&row)) {
            if (market.on_quote(*quote)) {
                last_quote = *quote;
            } else {
                symbols.at(static_cast<std::size_t>(quote->symbol.back() - '1')) = true;
            }
            continue;
        }

        const auto& print = std::get<Trade>(row);
        if (!market.on_trade(print)) {
            continue;
        }

        matching_trades.push_back(print);
        std::uint64_t volume{};
        std::int64_t notional{};

        // Independent reference for the bounded synthetic sample. Recompute
        // each window from scratch rather than sharing the ring's update logic.
        for (const auto& candidate : matching_trades) {
            if (print.ts_ns - candidate.ts_ns <= duration) {
                volume += candidate.qty;
                notional += candidate.px * candidate.qty;
            }
        }

        check(volume != 0 && market.vwap().volume() == volume, "sample window volume");
        check(market.vwap().price() == notional / static_cast<std::int64_t>(volume), "sample window VWAP");
    }

    const auto count = market.counts();
    check(count.quotes + count.ignored_quotes == 6905, "sample quote totals");
    check(count.trades + count.ignored_trades == 3095, "sample trade totals");
    check(count.quotes > 0 && count.trades > 0 && count.ignored_trades > 0, "filter routes both feeds");
    check(symbols == std::array{true, true, true, false, true}, "all four other symbols ignored");
    check(market.book() == last_quote, "last matching quote owns the L1 book");

    std::cout << "SYNTH4 reference matched " << count.trades << " rolling windows\n";
}

using Ports = std::pair<std::uint16_t, std::uint16_t>;

// Bind ephemeral ports before signaling the test client. No timing sleeps and
// no hard-coded ports shared with other tests.
struct LoopbackServer {
    std::promise<Ports> listening;
    std::future<Ports> ports = listening.get_future();
    std::future<void> result;

    explicit LoopbackServer(FeedHandler handler) {
        result = std::async(std::launch::async, [this, handler = std::move(handler)] {
            TcpFeedTransport transport({"127.0.0.1", 0}, {"127.0.0.1", 0});
            bool notified = false;

            try {
                transport.run(handler, [&](std::uint16_t md, std::uint16_t oe) {
                    listening.set_value({md, oe});
                    notified = true;
                });
            } catch (...) {
                if (!notified) {
                    listening.set_exception(std::current_exception());
                }
                throw;
            }
        });
    }
};

void loopback_test() {
    const Message quote = Quote{"SYNTH4", 10, 8, 341800, 9, 341900};
    const Message print = trade(10, 8, 341900);
    std::vector<Message> messages;
    std::promise<void> quote_received;
    auto quoted = quote_received.get_future();
    LoopbackServer server([&](Feed feed, const Message& message) {
        messages.push_back(message);
        if (feed == Feed::quotes) {
            quote_received.set_value();
        }
    });

    const auto [md_port, oe_port] = server.ports.get();
    tcp::Runtime runtime;
    const auto oe = tcp::connect({"127.0.0.1", oe_port});
    std::array<std::byte, max_frame_size> packed{};
    const auto size = encode(print, packed);
    tcp::send_all(oe, std::span(packed).first(2));

    const auto md = tcp::connect({"127.0.0.1", md_port});
    std::array<std::byte, max_frame_size> packed_quote{};
    tcp::send_all(md, std::span(packed_quote).first(encode(quote, packed_quote)));
    tcp::finish_sending(md);

    // MD must dispatch while OE still has only half a header.
    check(quoted.wait_for(3s) == std::future_status::ready, "partial OE cannot stall MD");
    tcp::send_all(oe, std::span(packed).subspan(2, size - 2));
    tcp::finish_sending(oe);
    server.result.get();

    check(messages == std::vector{quote, print}, "MD and OE frames must remain independent");
}

void bad_stream_test(bool truncated) {
    LoopbackServer server([](Feed, const Message&) {
        throw std::runtime_error("invalid frame reached handler");
    });

    const auto [md_port, oe_port] = server.ports.get();
    tcp::Runtime runtime;
    const auto md = tcp::connect({"127.0.0.1", md_port});
    const auto oe = tcp::connect({"127.0.0.1", oe_port});
    tcp::finish_sending(oe);
    std::array<std::byte, max_frame_size> packed{};
    const auto size = encode(trade(1, 1, 1), packed);
    tcp::send_all(md, std::span(packed).first(truncated ? 2 : size));
    tcp::finish_sending(md);

    bool expected = false;
    try {
        server.result.get();
    } catch (const std::runtime_error& error) {
        const std::string_view message = error.what();
        expected = message.starts_with("MD: ") &&
            message.find(truncated ? "EOF inside a frame" : "unexpected message type") != message.npos;
    }

    check(expected, "truncated stream and wrong feed types must fail with context");
}

} // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 2, "sample CSV argument required");
        window_tests();
        capacity_tests();
        sample_test(argv[1]);
        loopback_test();
        bad_stream_test(true);
        bad_stream_test(false);
        std::cout << "PASS: VWAP, ring, symbol filtering, and TCP loopback\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
