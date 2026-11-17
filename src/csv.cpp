#include "slipstream/csv.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace slipstream {
namespace {
std::string_view trim(std::string_view s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == s.npos) return {};
    return s.substr(begin, s.find_last_not_of(" \t\r\n") - begin + 1);
}
template<class T> T number(std::string_view text) {
    text = trim(text);
    if (text.empty()) throw std::runtime_error("empty integer");
    T result{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    if (error != std::errc{} || end != text.data() + text.size())
        throw std::runtime_error("invalid integer: " + std::string(text));
    return result;
}
std::vector<std::string_view> split(std::string_view line) {
    if (line.find('"') != line.npos) throw std::runtime_error("quoted CSV fields are not supported");
    std::vector<std::string_view> fields;
    for (;;) {
        const auto comma = line.find(',');
        fields.push_back(trim(line.substr(0, comma)));
        if (comma == line.npos) return fields;
        line.remove_prefix(comma + 1);
    }
}
void check_symbol(std::string_view s) {
    if (s.empty() || s.size() > 12 || std::any_of(s.begin(), s.end(), [](unsigned char c) { return c < 0x21 || c > 0x7e; }))
        throw std::runtime_error("symbol must contain 1..12 printable ASCII characters");
}
} // namespace
std::int64_t parse_price(std::string_view text) {
    text = trim(text);
    if (text.empty()) throw std::runtime_error("empty price");
    const bool negative = text.front() == '-';
    if (negative || text.front() == '+') text.remove_prefix(1);
    const auto dot = text.find('.');
    const auto whole = text.substr(0, dot);
    auto fraction = dot == text.npos ? std::string_view{} : text.substr(dot + 1);
    if (whole.empty() || fraction.size() > 4 || (dot != text.npos && fraction.empty()))
        throw std::runtime_error("price requires an integer part and at most four decimal places");
    // Accumulate the scaled magnitude under the signed limit, including INT64_MIN.
    const auto limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + (negative ? 1u : 0u);
    std::uint64_t magnitude{};
    auto digit = [&](char c) {
        if (c < '0' || c > '9' || magnitude > (limit - static_cast<unsigned>(c - '0')) / 10)
            throw std::runtime_error("invalid or overflowing price");
        magnitude = magnitude * 10 + static_cast<unsigned>(c - '0');
    };
    for (const char c : whole) digit(c);
    for (const char c : fraction) digit(c);
    for (std::size_t i = fraction.size(); i < 4; ++i) digit('0');
    if (negative && magnitude == limit) return std::numeric_limits<std::int64_t>::min();
    return negative ? -static_cast<std::int64_t>(magnitude) : static_cast<std::int64_t>(magnitude);
}
std::uint64_t parse_timestamp(std::string_view text, std::optional<std::uint64_t> midnight_ns) {
    text = trim(text);
    if (text.find(':') == text.npos) return number<std::uint64_t>(text);
    if (!midnight_ns) throw std::runtime_error("time-of-day timestamps require --date YYYY-MM-DD (UTC)");
    if (text.size() < 8 || text[2] != ':' || text[5] != ':' || (text.size() > 8 && text[8] != '.'))
        throw std::runtime_error("expected HH:MM:SS[.fraction]");
    const auto h = number<unsigned>(text.substr(0, 2));
    const auto m = number<unsigned>(text.substr(3, 2));
    const auto s = number<unsigned>(text.substr(6, 2));
    if (h > 23 || m > 59 || s > 59) throw std::runtime_error("time-of-day out of range");
    std::uint64_t fraction{};
    if (text.size() > 8) {
        auto digits = text.substr(9);
        if (digits.empty() || digits.size() > 9) throw std::runtime_error("timestamp fraction requires 1..9 digits");
        fraction = number<std::uint64_t>(digits);
        for (std::size_t i = digits.size(); i < 9; ++i) fraction *= 10;
    }
    const auto offset = (static_cast<std::uint64_t>(h) * 3600 + m * 60 + s) * 1'000'000'000 + fraction;
    if (*midnight_ns > std::numeric_limits<std::uint64_t>::max() - offset) throw std::runtime_error("timestamp overflow");
    return *midnight_ns + offset;
}
std::uint64_t parse_date_midnight(std::string_view date) {
    if (date.size() != 10 || date[4] != '-' || date[7] != '-') throw std::runtime_error("expected date YYYY-MM-DD");
    using namespace std::chrono;
    const year_month_day ymd{year{number<int>(date.substr(0, 4))}, month{number<unsigned>(date.substr(5, 2))}, day{number<unsigned>(date.substr(8, 2))}};
    if (!ymd.ok()) throw std::runtime_error("invalid calendar date");
    const auto days = sys_days{ymd}.time_since_epoch().count();
    constexpr std::uint64_t day_ns = 86'400'000'000'000;
    if (days < 0 || static_cast<std::uint64_t>(days) > std::numeric_limits<std::uint64_t>::max() / day_ns)
        throw std::runtime_error("date outside uint64 nanosecond epoch range");
    return static_cast<std::uint64_t>(days) * day_ns;
}
CsvData read_csv(std::istream& input, std::optional<std::uint64_t> midnight_ns) {
    std::string line;
    std::unordered_map<std::string, std::size_t> columns;
    CsvData result;
    std::size_t line_number{};
    std::int64_t trade_id{};
    while (std::getline(input, line)) {
        ++line_number;
        try {
            auto view = trim(line);
            if (view.starts_with("\xef\xbb\xbf")) view.remove_prefix(3);
            if (view.empty() || view.front() == '#') continue;
            if (columns.empty()) {
                if (!view.starts_with("Timestamp,")) continue; // descriptive rows
                const auto fields = split(view);
                for (std::size_t i = 0; i < fields.size(); ++i)
                    if (!columns.emplace(std::string(fields[i]), i).second) throw std::runtime_error("duplicate column");
                for (const auto name : {"Timestamp", "Type", "Symbol", "BidPrice", "BidQty", "AskPrice", "AskQty", "Price", "Qty", "Aggressor"})
                    if (!columns.contains(name)) throw std::runtime_error(std::string("missing column: ") + name);
                continue;
            }
            const auto fields = split(view);
            if (fields.size() != columns.size()) throw std::runtime_error("row has wrong column count");
            auto field = [&](const char* name) { return fields.at(columns.at(name)); };
            const auto ts = parse_timestamp(field("Timestamp"), midnight_ns);
            if (!result.rows.empty() && ts < timestamp(result.rows.back())) throw std::runtime_error("timestamps must be nondecreasing");
            const auto symbol = field("Symbol");
            check_symbol(symbol);
            if (field("Type") == "Q") {
                result.rows.emplace_back(Quote{std::string(symbol), ts, number<std::uint32_t>(field("BidQty")), parse_price(field("BidPrice")), number<std::uint32_t>(field("AskQty")), parse_price(field("AskPrice"))});
            } else if (field("Type") == "T") {
                const auto a = field("Aggressor");
                const char aggressor = a.empty() ? '?' : (a.size() == 1 ? a.front() : '\0');
                if (aggressor != 'B' && aggressor != 'S' && aggressor != '?') throw std::runtime_error("invalid aggressor");
                if (trade_id == std::numeric_limits<std::int64_t>::max()) throw std::runtime_error("trade ID overflow");
                result.rows.emplace_back(Trade{std::string(symbol), ts, number<std::uint32_t>(field("Qty")), parse_price(field("Price")), aggressor, ++trade_id});
            } else throw std::runtime_error("unknown CSV row type");
        } catch (const std::exception& error) {
            throw std::runtime_error("CSV line " + std::to_string(line_number) + ": " + error.what());
        }
    }
    if (input.bad()) throw std::runtime_error("CSV read failed");
    if (columns.empty()) throw std::runtime_error("CSV header not found");
    if (result.rows.empty()) throw std::runtime_error("CSV contains no data rows");
    result.origin_ns = timestamp(result.rows.front());
    return result;
}
} // namespace slipstream
