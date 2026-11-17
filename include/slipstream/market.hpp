#pragma once

#include "slipstream/messages.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace slipstream {

class RollingVwap {
public:
    static constexpr std::size_t capacity = 65'536;

    explicit RollingVwap(std::uint64_t window_ns);

    // Trade event time must be nondecreasing. Equal timestamps retain feed order.
    void add(const Trade& trade);
    std::optional<std::int64_t> price() const;
    std::uint64_t volume() const { return volume_; }
    std::size_t size() const { return size_; }

private:
    struct Print {
        std::uint64_t ts_ns{};
        std::uint32_t qty{};
        std::int64_t px{};
    };

    // GCC/Clang on 64-bit targets, including MinGW. __extension__ keeps the
    // extension local and allows -Wpedantic on the rest of the project.
    __extension__ typedef __int128 Notional;

    std::vector<Print> prints_;
    std::uint64_t window_ns_;
    std::optional<std::uint64_t> last_ts_;
    std::size_t head_{};
    std::size_t size_{};
    std::uint64_t volume_{};
    Notional notional_{};
};

struct MarketCounts {
    std::uint64_t quotes{};
    std::uint64_t trades{};
    std::uint64_t ignored_quotes{};
    std::uint64_t ignored_trades{};
};

// Single instrument state. Both replay and future live transports can call it.
class MarketState {
public:
    MarketState(std::string symbol, std::uint64_t window_ns);

    bool on_quote(const Quote& quote);
    bool on_trade(const Trade& trade);

    const std::optional<Quote>& book() const { return book_; }
    const RollingVwap& vwap() const { return vwap_; }
    const MarketCounts& counts() const { return counts_; }

private:
    std::string symbol_;
    std::optional<Quote> book_;
    RollingVwap vwap_;
    MarketCounts counts_;
};

} // namespace slipstream
