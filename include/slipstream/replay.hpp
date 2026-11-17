#pragma once
#include "slipstream/csv.hpp"
#include "slipstream/codec.hpp"
#include <chrono>
#include <functional>
#include <optional>
#include <span>
#include <string>

namespace slipstream {
enum class Feed { quotes, trades };
struct ReplayOptions {
    Feed feed{Feed::quotes};
    std::optional<std::string> symbol;
};
// Injecting time lets tests verify real scheduling semantics without sleeping.
struct ReplayClock {
    using Clock = std::chrono::steady_clock;
    std::function<Clock::time_point()> now;
    std::function<void(Clock::time_point)> wait_until;
};
ReplayClock wall_clock();
using ByteSink = std::function<void(std::span<const std::byte>)>;
std::size_t replay(const CsvData& data, const ReplayOptions& options,
                   ReplayClock clock, const ByteSink& sink);

// The receive half of the OE client. No matching, risk, or order state in M1.
class OeReceiver {
public:
    void receive(std::span<const std::byte> bytes, const StreamDecoder::Handler& handler);
    void finish() const { decoder_.finish(); }
private:
    StreamDecoder decoder_;
};
} // namespace slipstream
