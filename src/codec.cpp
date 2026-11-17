#include "slipstream/codec.hpp"
#include <algorithm>
#include <bit>
#include <type_traits>

namespace slipstream {
namespace {
std::size_t body_size(std::uint8_t type) {
    switch (type) {
    case 1: return 44;
    case 2: return 41; // Sum of the supplied fields; heading incorrectly says 49.
    case 3: return 8;
    case 4: return 9;
    case 10: return 50; // Sum of the supplied fields; heading incorrectly says 42.
    case 11: return 30;
    default: throw ProtocolError("unknown message type");
    }
}

struct Writer {
    std::span<std::byte> bytes;
    std::size_t pos{};

    template<class T> void integer(T value) {
        using U = std::make_unsigned_t<T>;
        const U bits = std::bit_cast<U>(value);

        for (std::size_t i = 0; i < sizeof(T); ++i)
            bytes[pos++] = std::byte((bits >> (8 * i)) & 0xff);
    }

    void character(char value) { integer(static_cast<std::uint8_t>(value)); }
    void symbol(const std::string& value) {
        if (value.empty() || value.size() > 12)
            throw ProtocolError("symbol must contain 1..12 ASCII characters");

        for (const unsigned char c : value)
            if (c < 0x21 || c > 0x7e) throw ProtocolError("invalid symbol character");

        for (std::size_t i = 0; i < 12; ++i) character(i < value.size() ? value[i] : '\0');
    }
};

struct Reader {
    std::span<const std::byte> bytes;
    std::size_t pos{};
    template<class T> T integer() {
        using U = std::make_unsigned_t<T>;
        U bits{};
        for (std::size_t i = 0; i < sizeof(T); ++i)
            bits |= static_cast<U>(std::to_integer<unsigned char>(bytes[pos++])) << (8 * i);
        return std::bit_cast<T>(bits);
    }

    char character() { return static_cast<char>(integer<std::uint8_t>()); }
    std::string symbol() {
        std::string result;
        bool padding = false;

        for (int i = 0; i < 12; ++i) {
            const auto c = integer<std::uint8_t>();
            if (c == 0) padding = true;
            else {
                if (padding || c < 0x21 || c > 0x7e) throw ProtocolError("invalid symbol padding/ASCII");
                result.push_back(static_cast<char>(c));
            }
        }

        if (result.empty()) throw ProtocolError("empty symbol");
        return result;
    }
};

void validate(const Message& message) {
    std::visit([](const auto& m) {
        using T = std::decay_t<decltype(m)>;
        if constexpr (std::is_same_v<T, Trade>) {
            if (m.aggressor != 'B' && m.aggressor != 'S' && m.aggressor != '?')
                throw ProtocolError("invalid aggressor");
        } else if constexpr (std::is_same_v<T, SessionControl>) {
            if (m.state > 2) throw ProtocolError("invalid session state");
        } else if constexpr (std::is_same_v<T, NewOrder>) {
            if ((m.status != 'A' && m.status != 'R') || (m.side != 'B' && m.side != 'S'))
                throw ProtocolError("invalid order status/side");
        } else if constexpr (std::is_same_v<T, ExecReport>) {
            if (m.status > 3 || m.reason_code > 4) throw ProtocolError("invalid execution status/reason");
        }
    }, message);
}

std::size_t validate_header(std::span<const std::byte> header) {
    Reader r{header};
    const auto length = r.integer<std::uint16_t>();
    const auto type = r.integer<std::uint8_t>();
    if (r.integer<std::uint8_t>() != 1) throw ProtocolError("unsupported protocol version");
    if (length != body_size(type)) throw ProtocolError("body length disagrees with message type");
    return header_size + length;
}
} // namespace

std::uint8_t message_type(const Message& message) {
    constexpr std::array<std::uint8_t, 6> types{1, 2, 3, 4, 10, 11};
    return types[message.index()];
}
std::size_t encoded_size(const Message& message) { return header_size + body_size(message_type(message)); }
std::size_t encode(const Message& message, std::span<std::byte> output) {
    const auto size = encoded_size(message);
    if (output.size() < size) throw ProtocolError("encoder output buffer too small");
    validate(message);
    Writer w{output};
    w.integer(static_cast<std::uint16_t>(size - header_size));
    w.integer(message_type(message));
    w.integer(std::uint8_t{1});
    std::visit([&](const auto& m) {
        using T = std::decay_t<decltype(m)>;
        if constexpr (std::is_same_v<T, slipstream::Quote>) {
            w.symbol(m.symbol); w.integer(m.ts_ns); w.integer(m.bid_qty);
            w.integer(m.bid_px); w.integer(m.ask_qty); w.integer(m.ask_px);
        } else if constexpr (std::is_same_v<T, Trade>) {
            w.symbol(m.symbol); w.integer(m.ts_ns); w.integer(m.qty);
            w.integer(m.px); w.character(m.aggressor); w.integer(m.id);
        } else if constexpr (std::is_same_v<T, Heartbeat>) {
            w.integer(m.ts_ns);
        } else if constexpr (std::is_same_v<T, SessionControl>) {
            w.integer(m.ts_ns); w.integer(m.state);
        } else if constexpr (std::is_same_v<T, NewOrder>) {
            w.integer(m.client_order_id); w.symbol(m.symbol); w.character(m.status);
            w.integer(m.ts_ns); w.integer(m.trade_id); w.character(m.side);
            w.integer(m.qty); w.integer(m.limit_px);
        } else {
            w.integer(m.client_order_id); w.integer(m.ts_ns); w.integer(m.status);
            w.integer(m.filled_qty); w.integer(m.avg_px); w.integer(m.reason_code);
        }
    }, message);
    return w.pos;
}
Message decode(std::span<const std::byte> frame) {
    if (frame.size() < header_size) throw ProtocolError("incomplete frame header");
    if (frame.size() != validate_header(frame.first(header_size))) throw ProtocolError("incomplete or extra frame bytes");
    Reader r{frame, header_size};
    Message result;
    switch (std::to_integer<unsigned char>(frame[2])) {
    case 1: {
        Quote m;
        m.symbol = r.symbol(); m.ts_ns = r.integer<std::uint64_t>(); m.bid_qty = r.integer<std::uint32_t>();
        m.bid_px = r.integer<std::int64_t>(); m.ask_qty = r.integer<std::uint32_t>(); m.ask_px = r.integer<std::int64_t>();
        result = m; break;
    }
    case 2: {
        Trade m;
        m.symbol = r.symbol(); m.ts_ns = r.integer<std::uint64_t>(); m.qty = r.integer<std::uint32_t>();
        m.px = r.integer<std::int64_t>(); m.aggressor = r.character(); m.id = r.integer<std::int64_t>();
        result = m; break;
    }
    case 3: result = Heartbeat{r.integer<std::uint64_t>()}; break;
    case 4: {
        SessionControl m;
        m.ts_ns = r.integer<std::uint64_t>(); m.state = r.integer<std::uint8_t>(); result = m; break;
    }
    case 10: {
        NewOrder m;
        m.client_order_id = r.integer<std::uint64_t>(); m.symbol = r.symbol(); m.status = r.character();
        m.ts_ns = r.integer<std::uint64_t>(); m.trade_id = r.integer<std::int64_t>(); m.side = r.character();
        m.qty = r.integer<std::uint32_t>(); m.limit_px = r.integer<std::int64_t>(); result = m; break;
    }
    case 11: {
        ExecReport m;
        m.client_order_id = r.integer<std::uint64_t>(); m.ts_ns = r.integer<std::uint64_t>();
        m.status = r.integer<std::uint8_t>(); m.filled_qty = r.integer<std::uint32_t>();
        m.avg_px = r.integer<std::int64_t>(); m.reason_code = r.integer<std::uint8_t>(); result = m; break;
    }
    default: throw ProtocolError("unknown message type");
    }
    validate(result);
    return result;
}
void StreamDecoder::feed(std::span<const std::byte> chunk, const Handler& on_message) {
    if (failed_) throw ProtocolError("decoder failed; create a new decoder for a new stream");
    while (!chunk.empty()) {
        const auto count = std::min(target_ - used_, chunk.size());
        std::copy_n(chunk.begin(), count, buffer_.begin() + static_cast<std::ptrdiff_t>(used_));
        used_ += count;
        chunk = chunk.subspan(count);
        try {
            if (used_ == header_size && target_ == header_size)
                target_ = validate_header(std::span(buffer_).first(header_size));
            if (used_ != target_) continue;
            const auto message = decode(std::span(buffer_).first(used_));
            used_ = 0;
            target_ = header_size;
            // A handler exception terminates this stream too: remaining chunk is unconsumed.
            on_message(message);
        } catch (...) {
            failed_ = true;
            throw;
        }
    }
}
void StreamDecoder::finish() const {
    if (failed_) throw ProtocolError("decoder failed");
    if (used_ != 0) throw ProtocolError("EOF inside a frame");
}
} // namespace slipstream
