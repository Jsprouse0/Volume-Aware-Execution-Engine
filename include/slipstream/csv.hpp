#pragma once
#include "slipstream/messages.hpp"
#include <istream>
#include <optional>
#include <string_view>
#include <vector>

namespace slipstream {
std::int64_t parse_price(std::string_view text);
std::uint64_t parse_timestamp(std::string_view text,
                             std::optional<std::uint64_t> midnight_ns = std::nullopt);
std::uint64_t parse_date_midnight(std::string_view date); // YYYY-MM-DD, UTC
struct CsvData {
    std::uint64_t origin_ns{};
    std::vector<Message> rows;
};
// Strict, unquoted CSV dialect used by QandT.csv. Skips preamble before header.
// Trade IDs are stable, 1-based ordinals across all trade rows before filtering.
CsvData read_csv(std::istream& input,
                 std::optional<std::uint64_t> midnight_ns = std::nullopt);
} // namespace slipstream
