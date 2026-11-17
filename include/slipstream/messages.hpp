#pragma once
#include <cstdint>
#include <string>
#include <variant>

namespace slipstream {
struct Quote {
    std::string symbol;
    std::uint64_t ts_ns{};
    std::uint32_t bid_qty{};
    std::int64_t bid_px{};
    std::uint32_t ask_qty{};
    std::int64_t ask_px{};
    bool operator==(const Quote&) const = default;
};

struct Trade {
    std::string symbol;
    std::uint64_t ts_ns{};
    std::uint32_t qty{};
    std::int64_t px{};
    char aggressor{'?'};
    std::int64_t id{};
    bool operator==(const Trade&) const = default;
};

struct Heartbeat {
    std::uint64_t ts_ns{};
    bool operator==(const Heartbeat&) const = default;
};

struct SessionControl {
    std::uint64_t ts_ns{};
    std::uint8_t state{};
    bool operator==(const SessionControl&) const = default;
};

struct NewOrder {
    std::uint64_t client_order_id{};
    std::string symbol;
    char status{'A'};
    std::uint64_t ts_ns{};
    std::int64_t trade_id{};
    char side{'B'};
    std::uint32_t qty{};
    std::int64_t limit_px{};
    bool operator==(const NewOrder&) const = default;
};

struct ExecReport {
    std::uint64_t client_order_id{};
    std::uint64_t ts_ns{};
    std::uint8_t status{};
    std::uint32_t filled_qty{};
    std::int64_t avg_px{};
    std::uint8_t reason_code{};
    bool operator==(const ExecReport&) const = default;
};

using Message = std::variant<Quote, Trade, Heartbeat, SessionControl, NewOrder, ExecReport>;
inline std::uint64_t timestamp(const Message& message) {
    return std::visit([](const auto& value) { return value.ts_ns; }, message);
}

} // namespace slipstream
