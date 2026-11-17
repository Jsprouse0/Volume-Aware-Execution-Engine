#include "slipstream/replay.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {
int checks{};
void check(bool condition, const char* text) {
    ++checks;
    if (!condition) throw std::runtime_error(text);
}
#define CHECK(...) check((__VA_ARGS__), #__VA_ARGS__)
template<class F> void rejects(F action) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    CHECK(rejected);
}
using namespace slipstream;
std::vector<std::byte> bytes(const Message& message) {
    std::vector<std::byte> frame(encoded_size(message));
    CHECK(encode(message, frame) == frame.size());
    return frame;
}
std::vector<Message> examples() {
    return {Quote{"SYNTH1", 0x0102030405060708ULL, 0x11223344u, -2, 0xaabbccddu, 1012500},
            Trade{"ABCDEFGHIJKL", 17, 123, 342000, 'S', -9}, Heartbeat{42}, SessionControl{43, 2},
            NewOrder{9, "SYNTH4", 'A', 44, 3, 'B', 100, 342000}, ExecReport{9, 45, 2, 50, 342001, 0}};
}
void codec_tests() {
    const auto messages = examples();
    const std::array<std::size_t, 6> sizes{48, 45, 12, 13, 54, 34};
    for (std::size_t i = 0; i < messages.size(); ++i) {
        auto frame = bytes(messages[i]);
        CHECK(frame.size() == sizes[i]);
        CHECK(decode(frame) == messages[i]);
        std::array<std::byte, 3> short_output{};
        rejects([&] { encode(messages[i], short_output); });
        frame.push_back(std::byte{});
        rejects([&] { decode(frame); });
    }
    // Independent, literal byte fixture: catches paired encoder/decoder mistakes.
    const std::array<unsigned char, 48> expected{
        0x2c,0,1,1, 'S','Y','N','T','H','1',0,0,0,0,0,0,
        8,7,6,5,4,3,2,1, 0x44,0x33,0x22,0x11,
        0xfe,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xdd,0xcc,0xbb,0xaa, 0x14,0x73,0x0f,0,0,0,0,0};
    const auto quote = bytes(messages[0]);
    CHECK(std::equal(quote.begin(), quote.end(), expected.begin(), [](auto a, auto b) { return std::to_integer<unsigned char>(a) == b; }));
    // A second fixture nails down the disputed Trade body length and ID offset.
    const std::array<unsigned char, 45> expected_trade{
        0x29,0,2,1, 'A','B','C','D','E','F','G','H','I','J','K','L',
        17,0,0,0,0,0,0,0, 123,0,0,0, 0xf0,0x37,5,0,0,0,0,0, 'S',
        0xf7,0xff,0xff,0xff,0xff,0xff,0xff,0xff};
    const auto trade = bytes(messages[1]);
    CHECK(std::equal(trade.begin(), trade.end(), expected_trade.begin(), [](auto a, auto b) { return std::to_integer<unsigned char>(a) == b; }));
    const std::array<unsigned char, 54> expected_order{
        0x32,0,10,1, 9,0,0,0,0,0,0,0,
        'S','Y','N','T','H','4',0,0,0,0,0,0, 'A',
        44,0,0,0,0,0,0,0, 3,0,0,0,0,0,0,0, 'B',
        100,0,0,0, 0xf0,0x37,5,0,0,0,0,0};
    const auto order = bytes(messages[4]);
    CHECK(std::equal(order.begin(), order.end(), expected_order.begin(), [](auto a, auto b) { return std::to_integer<unsigned char>(a) == b; }));
    auto frame = quote;
    frame[3] = std::byte{2}; rejects([&] { decode(frame); });
    frame = quote; frame[2] = std::byte{99}; rejects([&] { decode(frame); });
    frame = quote; frame[0] = std::byte{255}; rejects([&] { decode(frame); });
    frame = quote; frame[11] = std::byte{'X'}; rejects([&] { decode(frame); });
    rejects([&] { bytes(Quote{"TOO-LONG-SYMBOL", 0, 0, 0, 0, 0}); });
    rejects([&] { bytes(Trade{"X", 0, 0, 0, 'X', 0}); });
    rejects([&] { bytes(SessionControl{0, 3}); });
    rejects([&] { bytes(NewOrder{0, "X", 'X', 0, 0, 'B', 0, 0}); });
    rejects([&] { bytes(ExecReport{0, 0, 4, 0, 0, 0}); });
    for (const auto price : {std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()}) {
        const Message m = Trade{"X", std::numeric_limits<std::uint64_t>::max(), std::numeric_limits<std::uint32_t>::max(), price, '?', price};
        CHECK(decode(bytes(m)) == m);
    }
}
void split_tests() {
    const auto messages = examples();
    for (const auto& message : messages) {
        const auto frame = bytes(message);
        // Every two-chunk split, including empty chunks and split headers.
        for (std::size_t split = 0; split <= frame.size(); ++split) {
            StreamDecoder decoder;
            std::vector<Message> received;
            auto handler = [&](const auto& m) { received.push_back(m); };
            decoder.feed(std::span(frame).first(split), handler);
            CHECK(received.size() == (split == frame.size() ? 1u : 0u));
            decoder.feed(std::span(frame).subspan(split), handler);
            decoder.finish();
            CHECK(received == std::vector<Message>{message});
            CHECK(decoder.buffered_bytes() == 0);
        }
        for (std::size_t prefix = 1; prefix < frame.size(); ++prefix) {
            StreamDecoder truncated;
            truncated.feed(std::span(frame).first(prefix), [](auto&) {});
            rejects([&] { truncated.finish(); });
        }
    }
    std::vector<std::byte> stream;
    for (const auto& m : messages) {
        auto frame = bytes(m);
        stream.insert(stream.end(), frame.begin(), frame.end());
    }
    // Coalesced frames and all fixed chunk widths, including single-byte recv().
    for (std::size_t width = 1; width <= stream.size(); ++width) {
        StreamDecoder decoder;
        std::vector<Message> received;
        for (std::size_t p = 0; p < stream.size(); p += width)
            decoder.feed(std::span(stream).subspan(p, std::min(width, stream.size() - p)), [&](const auto& m) { received.push_back(m); });
        decoder.finish();
        CHECK(received == messages);
    }
    std::mt19937 rng(2026);
    for (int trial = 0; trial < 100; ++trial) {
        StreamDecoder decoder;
        std::vector<Message> received;
        for (std::size_t p = 0; p < stream.size();) {
            const auto count = std::min<std::size_t>(1 + rng() % 71, stream.size() - p);
            decoder.feed(std::span(stream).subspan(p, count), [&](const auto& m) { received.push_back(m); });
            p += count;
        }
        decoder.finish();
        CHECK(received == messages);
    }
    StreamDecoder bad;
    const std::array<std::byte, 4> huge{std::byte{255}, std::byte{255}, std::byte{1}, std::byte{1}};
    rejects([&] { bad.feed(huge, [](auto&) {}); });
    CHECK(bad.buffered_bytes() == 4); // Reject from header, no 64KB allocation.
    rejects([&] { bad.feed({}, [](auto&) {}); });
    rejects([&] { bad.finish(); });
    StreamDecoder empty;
    empty.feed({}, [](auto&) {});
    empty.finish();
}
const std::string header = "Timestamp,Type,Symbol,BidPrice,BidQty,AskPrice,AskQty,Price,Qty,Aggressor\n";
CsvData csv(std::string rows, std::optional<std::uint64_t> base = 0) {
    std::istringstream input("Description of this dataset\n# comment\n\n" + header + rows);
    return read_csv(input, base);
}
void csv_tests(const char* sample_path) {
    CHECK(parse_price("101.2500") == 1012500);
    CHECK(parse_price(" 34.2 ") == 342000);
    CHECK(parse_price("0.0001") == 1);
    CHECK(parse_price("-0.0001") == -1);
    CHECK(parse_price("+1250") == 12500000);
    CHECK(parse_price("922337203685477.5807") == std::numeric_limits<std::int64_t>::max());
    CHECK(parse_price("-922337203685477.5808") == std::numeric_limits<std::int64_t>::min());
    for (const auto bad : {"", "1.23456", "nan", "1e2", "1.", ".5", "--1", "922337203685477.5808", "-922337203685477.5809"})
        rejects([&] { parse_price(bad); });
    CHECK(parse_timestamp("09:30:00.003", 100) == 34'200'003'000'100ULL);
    CHECK(parse_timestamp("00:00:00.000000001", 0) == 1);
    CHECK(parse_timestamp("18446744073709551615") == std::numeric_limits<std::uint64_t>::max());
    CHECK(parse_date_midnight("1970-01-01") == 0);
    CHECK(parse_date_midnight("1970-01-02") == 86'400'000'000'000ULL);
    CHECK(parse_date_midnight("2024-02-29") > 0);
    rejects([] { parse_date_midnight("2023-02-29"); });
    rejects([] { parse_date_midnight("1969-12-31"); });
    rejects([] { parse_date_midnight("9999-01-01"); });
    rejects([] { parse_timestamp("09:30:00.003"); });
    rejects([] { parse_timestamp("24:00:00", 0); });
    rejects([] { parse_timestamp("00:60:00", 0); });
    rejects([] { parse_timestamp("00:00:00.", 0); });
    rejects([] { parse_timestamp("00:00:00.1234567890", 0); });
    rejects([] { parse_timestamp("00:00:01", std::numeric_limits<std::uint64_t>::max()); });
    const auto data = csv("09:30:00.000,Q,SYNTH4,34.18,900,34.19,1100,,,\r\n"
                          "09:30:00.000,T,SYNTH4,,,,,34.19,50,B\n"
                          "09:30:00.300,T,SYNTH4,,,,,34.18,10,\n");
    CHECK(data.rows.size() == 3);
    CHECK(std::get<Quote>(data.rows[0]).bid_px == 341800);
    CHECK(std::get<Trade>(data.rows[1]).id == 1);
    CHECK(std::get<Trade>(data.rows[2]).id == 2);
    CHECK(std::get<Trade>(data.rows[2]).aggressor == '?');
    CHECK(timestamp(data.rows[2]) - timestamp(data.rows[1]) == 300'000'000);
    rejects([] { csv("2,T,X,,,,,1,1,B\n1,T,X,,,,,1,1,S\n"); });
    rejects([] { csv("1,T,X,,,,,1,4294967296,B\n"); });
    rejects([] { csv("1,T,X,,,,,1,1,BUY\n"); });
    rejects([] { csv("1,Z,X,,,,,1,1,B\n"); });
    rejects([] { csv("1,T,TOO_LONG_SYMBOL,,,,,1,1,B\n"); });
    rejects([] { csv("1,T,X,,,,,1,1\n"); });
    rejects([] { csv(""); });
    rejects([] { std::istringstream input("no header"); read_csv(input); });
    std::ifstream sample(sample_path);
    CHECK(sample.good());
    const auto sample_data = read_csv(sample, parse_date_midnight("2026-10-27"));
    CHECK(sample_data.rows.size() > 1000);
    CHECK(std::get<Quote>(sample_data.rows.front()).symbol == "SYNTH3");
    CHECK(std::get<Trade>(sample_data.rows.back()).qty == 22);
    // Every supplied sample row goes through the binary round trip.
    for (const auto& row : sample_data.rows) CHECK(decode(bytes(row)) == row);
    std::cout << "Sample CSV: " << sample_data.rows.size() << " rows round-tripped\n";
}
void replay_tests() {
    using namespace std::chrono;
    const auto data = csv("00:00:00.000,Q,SYNTH1,1,1,2,1,,,\n"
                          "00:00:00.100,T,SYNTH4,,,,,34.19,50,B\n"
                          "00:00:00.250,T,SYNTH2,,,,,1,1,S\n"
                          "00:00:00.400,T,SYNTH4,,,,,34.18,10,S\n"
                          "00:00:00.400,T,SYNTH4,,,,,34.19,20,B\n");
    using Clock = ReplayClock::Clock;
    const auto start = Clock::time_point{seconds{10}};
    auto now = start;
    std::vector<Clock::time_point> deadlines;
    ReplayClock fake{[&] { return now; }, [&](auto deadline) {
        deadlines.push_back(deadline);
        now = std::max(now, deadline);
    }};
    std::vector<Message> received;
    StreamDecoder peer;
    const auto sent = replay(data, {Feed::trades, "SYNTH4"}, fake, [&](auto frame) {
        // Output work costs 7ms; subsequent deadlines must not drift by 7ms.
        now += milliseconds{7};
        peer.feed(frame, [&](const auto& m) { received.push_back(m); });
    });
    peer.finish();
    CHECK(sent == 3);
    CHECK(received.size() == 3);
    CHECK(deadlines[0] == start + milliseconds{100});
    CHECK(deadlines[1] == start + milliseconds{400});
    CHECK(deadlines[2] == deadlines[1]);
    CHECK(deadlines[1] - deadlines[0] == milliseconds{300});
    CHECK(std::get<Trade>(received[1]).id == 3); // IDs are assigned before filtering.
    deadlines.clear();
    CHECK(replay(data, {Feed::quotes, std::nullopt}, fake, [](auto) {}) == 1);
    CHECK(deadlines.size() == 1);
    CHECK(replay(data, {Feed::quotes, "ABSENT"}, fake, [](auto) {}) == 0);
    auto backwards = data;
    std::reverse(backwards.rows.begin(), backwards.rows.end());
    rejects([&] { replay(backwards, {}, fake, [](auto) {}); });
    auto oversized = data;
    oversized.rows = {Heartbeat{std::numeric_limits<std::uint64_t>::max()}, Trade{"X", std::numeric_limits<std::uint64_t>::max(), 1, 1, '?', 1}};
    rejects([&] { replay(oversized, {Feed::trades, {}}, fake, [](auto) {}); });
    OeReceiver oe;
    std::vector<Message> inbound;
    const auto messages = examples();
    Message rejected_order = std::get<NewOrder>(messages[4]);
    std::get<NewOrder>(rejected_order).status = 'R';
    for (const auto& m : {messages[4], messages[5], rejected_order}) {
        const auto frame = bytes(m);
        for (const auto b : frame) oe.receive(std::span(&b, 1), [&](const auto& value) { inbound.push_back(value); });
    }
    oe.finish();
    CHECK(inbound == std::vector<Message>({messages[4], messages[5], rejected_order}));
    OeReceiver wrong_direction;
    rejects([&] { wrong_direction.receive(bytes(messages[1]), [](auto&) {}); });
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("expected sample CSV path");
        codec_tests(); split_tests(); csv_tests(argv[1]); replay_tests();
        std::cout << "PASS: " << checks << " checks (codec, splits, CSV, replay, OE receive)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
