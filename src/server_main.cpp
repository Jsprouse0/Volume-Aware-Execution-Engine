#include "slipstream/feed_transport.hpp"
#include "slipstream/market.hpp"

#include <charconv>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {

using namespace slipstream;

struct Settings {
    Endpoint md{"127.0.0.1", 14200};
    Endpoint oe{"127.0.0.1", 14300};
    std::string symbol;
    std::uint64_t window_ns{30'000'000'000ULL};
};

void usage() {
    std::cout << "slipstream --symbol SYMBOL [--vwap-window-ms 30000]\n"
                 "           [--md-host 127.0.0.1] [--md-port 14200]\n"
                 "           [--oe-host 127.0.0.1] [--oe-port 14300] [--transport tcp]\n";
}

std::uint64_t window_ns(std::string_view text) {
    constexpr std::uint64_t ns_per_ms = 1'000'000;
    std::uint64_t ms{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), ms);

    if (error != std::errc{} || end != text.data() + text.size() || ms == 0 ||
        ms > std::numeric_limits<std::uint64_t>::max() / ns_per_ms) {
        throw std::invalid_argument("VWAP window must be a positive millisecond count that fits in nanoseconds");
    }

    return ms * ns_per_ms;
}

Settings read_settings(int argc, char** argv) {
    Settings settings;

    for (int i = 1; i < argc; ++i) {
        const std::string_view key = argv[i];

        if (++i == argc) {
            throw std::invalid_argument("missing value for " + std::string(key));
        }

        const std::string_view value = argv[i];

        if (key == "--symbol") {
            settings.symbol = value;
        } else if (key == "--vwap-window-ms") {
            settings.window_ns = window_ns(value);
        } else if (key == "--md-host") {
            settings.md.host = value;
        } else if (key == "--md-port") {
            settings.md.port = parse_port(value);
        } else if (key == "--oe-host") {
            settings.oe.host = value;
        } else if (key == "--oe-port") {
            settings.oe.port = parse_port(value);
        } else if (key == "--transport" && value == "tcp") {
            continue;
        } else {
            throw std::invalid_argument("unknown option or unsupported value: " + std::string(key));
        }
    }

    if (settings.symbol.empty()) {
        throw std::invalid_argument("--symbol is required");
    }

    return settings;
}

void print_vwap(const RollingVwap& window) {
    std::cout << " window_qty=" << window.volume() << " vwap_x10000=";

    if (const auto price = window.price()) {
        std::cout << *price;
    } else {
        std::cout << "n/a";
    }
}

void run(FeedTransport& transport, const Settings& settings) {
    MarketState market(settings.symbol, settings.window_ns);

    auto on_message = [&](Feed feed, const Message& message) {
        if (feed == Feed::quotes) {
            market.on_quote(std::get<Quote>(message));
            return;
        }

        const auto& trade = std::get<Trade>(message);

        if (market.on_trade(trade)) {
            std::cout << "VWAP symbol=" << trade.symbol << " ts_ns=" << trade.ts_ns
                      << " trade_id=" << trade.id;
            print_vwap(market.vwap());
            std::cout << '\n';
        }
    };

    auto on_listen = [&](std::uint16_t md_port, std::uint16_t oe_port) {
        std::cout << "LISTEN MD " << settings.md.host << ':' << md_port
                  << " OE " << settings.oe.host << ':' << oe_port << std::endl;
    };

    transport.run(on_message, on_listen);

    const auto& counts = market.counts();
    std::cout << "SUMMARY symbol=" << settings.symbol << " quotes=" << counts.quotes
              << " trades=" << counts.trades << " ignored_quotes=" << counts.ignored_quotes
              << " ignored_trades=" << counts.ignored_trades;
    print_vwap(market.vwap());

    if (const auto& quote = market.book()) {
        std::cout << " bid_px_x10000=" << quote->bid_px << " bid_qty=" << quote->bid_qty
                  << " ask_px_x10000=" << quote->ask_px << " ask_qty=" << quote->ask_qty;
    }

    std::cout << '\n';
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        usage();
        return 0;
    }

    try {
        const auto settings = read_settings(argc, argv);
        slipstream::TcpFeedTransport transport(settings.md, settings.oe);
        run(transport, settings);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "slipstream: " << error.what() << '\n';
        return 1;
    }
}
