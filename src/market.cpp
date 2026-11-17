#include "slipstream/market.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace slipstream {

RollingVwap::RollingVwap(std::uint64_t window_ns)
    : prints_(capacity), window_ns_(window_ns) {
    if (window_ns == 0) {
        throw std::invalid_argument("VWAP window must be positive");
    }
}

void RollingVwap::add(const Trade& trade) {
    if (last_ts_ && trade.ts_ns < *last_ts_) {
        throw std::invalid_argument("trade timestamps must be nondecreasing");
    }

    // The trailing window includes its left endpoint. Use elapsed time to
    // avoid unsigned underflow near the start of the timestamp range.
    while (size_ != 0 && trade.ts_ns - prints_[head_].ts_ns > window_ns_) {
        const auto& oldest = prints_[head_];
        notional_ -= static_cast<Notional>(oldest.px) * oldest.qty;
        volume_ -= oldest.qty;
        head_ = (head_ + 1) % capacity;
        --size_;
    }

    if (trade.qty != 0) {
        if (size_ == capacity) {
            throw std::overflow_error("VWAP ring full: live prints exceed 65536; cannot report an exact window");
        }

        const auto tail = (head_ + size_) % capacity;
        prints_[tail] = Print{trade.ts_ns, trade.qty, trade.px};
        ++size_;

        // Each term has magnitude < 2^95 and there are at most 2^16 terms.
        // Multiplication and sum therefore fit signed 128 bits. Volume < 2^48.
        notional_ += static_cast<Notional>(trade.px) * trade.qty;
        volume_ += trade.qty;
    }

    last_ts_ = trade.ts_ns;
}

std::optional<std::int64_t> RollingVwap::price() const {
    if (volume_ == 0) {
        return std::nullopt;
    }

    // A weighted average with nonnegative weights lies within its input range.
    return static_cast<std::int64_t>(notional_ / volume_);
}

MarketState::MarketState(std::string symbol, std::uint64_t window_ns)
    : symbol_(std::move(symbol)), vwap_(window_ns) {
    const bool invalid_ascii = std::any_of(symbol_.begin(), symbol_.end(),
        [](unsigned char c) { return c < 0x21 || c > 0x7e; });

    if (symbol_.empty() || symbol_.size() > 12 || invalid_ascii) {
        throw std::invalid_argument("symbol must contain 1..12 printable non-space ASCII characters");
    }
}

bool MarketState::on_quote(const Quote& quote) {
    if (quote.symbol != symbol_) {
        ++counts_.ignored_quotes;
        return false;
    }

    if (book_ && quote.ts_ns < book_->ts_ns) {
        throw std::invalid_argument("quote timestamps must be nondecreasing");
    }

    book_ = quote;
    ++counts_.quotes;
    return true;
}

bool MarketState::on_trade(const Trade& trade) {
    if (trade.symbol != symbol_) {
        ++counts_.ignored_trades;
        return false;
    }

    vwap_.add(trade);
    ++counts_.trades;
    return true;
}

} // namespace slipstream
