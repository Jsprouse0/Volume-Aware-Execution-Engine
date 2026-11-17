#include "slipstream/replay.hpp"
#include <array>
#include <limits>
#include <stdexcept>
#include <thread>
#include <type_traits>

namespace slipstream {
ReplayClock wall_clock() {
    return {[] { return ReplayClock::Clock::now(); },
            [](auto deadline) { std::this_thread::sleep_until(deadline); }};
}

std::size_t replay(const CsvData& data, const ReplayOptions& options, ReplayClock clock, const ByteSink& sink) {
    const auto start = clock.now();

    std::array<std::byte, max_frame_size> frame{};
    std::size_t sent{};
    std::uint64_t previous = data.origin_ns;
    for (const auto& row : data.rows) {
        const auto ts = timestamp(row);

        if (ts < previous) {
            throw std::runtime_error("replay timestamps must be nondecreasing and at "
                                     "least origin");
        }

        previous = ts;
        const bool selected = std::visit([&](const auto& m) {
            using T = std::decay_t<decltype(m)>;

            if constexpr (std::is_same_v<T, Quote> || std::is_same_v<T, Trade>) {
                return (std::is_same_v<T, Quote> == (options.feed == Feed::quotes)) &&
                       (!options.symbol || m.symbol == *options.symbol);
            }

            return false;
        }, row);

        if (!selected) continue;

        const auto offset = ts - data.origin_ns;

        if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            throw std::runtime_error("replay duration exceeds clock range");
        }

        const auto delay = std::chrono::duration_cast<ReplayClock::Clock::duration>(std::chrono::nanoseconds{static_cast<std::int64_t>(offset)});
        if (start.time_since_epoch() > ReplayClock::Clock::duration::max() - delay){
            throw std::runtime_error("replay deadline overflow");
        }

        // Absolute deadlines avoid adding encoding/output time to every interval.
        clock.wait_until(start + delay);
        const auto bytes_written = encode(row, frame);
        sink(std::span(frame).first(bytes_written));

        ++sent;
    }

    return sent;
}

void OeReceiver::receive(std::span<const std::byte> bytes, const StreamDecoder::Handler& handler) {
    decoder_.feed(bytes, [&](const Message& message) {
        if (!std::holds_alternative<NewOrder>(message) &&
            !std::holds_alternative<ExecReport>(message)) {
            throw ProtocolError("OE receive path expects NewOrder or ExecReport");
        }

        handler(message);
    });
}
} // namespace slipstream
