#include "slipstream/feed_transport.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

using namespace slipstream;

#ifdef SLIPSTREAM_OE_CLIENT
constexpr bool is_oe = true;
#else
constexpr bool is_oe = false;
#endif

struct Settings {
    std::string csv_path;
    std::string output_path;
    std::string decode_path;
    Endpoint server{"127.0.0.1", is_oe ? std::uint16_t{14300} : std::uint16_t{14200}};
    std::optional<std::uint64_t> midnight;
    ReplayOptions replay{is_oe ? Feed::trades : Feed::quotes, std::nullopt};
    bool tcp{};
    bool no_wait{};
};

void usage() {
    const auto* executable = is_oe ? "oe_client" : "md_client";
    std::cout << executable << " --csv FILE (--out FILE | --tcp [--host IP] [--port PORT])\n"
              << "          [--date YYYY-MM-DD] [--symbol SYMBOL] [--no-wait]\n";

    if (is_oe) {
        std::cout << executable << " --decode FILE\n";
    }

    std::cout << "Clock timestamps require --date (UTC); numeric timestamps are epoch nanoseconds.\n"
                 "Replay runs in real time unless --no-wait is supplied.\n";
}

Settings read_settings(int argc, char** argv) {
    Settings settings;
    bool endpoint_given = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view key = argv[i];

        if (key == "--no-wait") {
            settings.no_wait = true;
            continue;
        }

        if (key == "--tcp") {
            settings.tcp = true;
            continue;
        }

        if (++i == argc) {
            throw std::invalid_argument("missing value for " + std::string(key));
        }

        const std::string_view value = argv[i];

        if (key == "--csv") {
            settings.csv_path = value;
        } else if (key == "--out") {
            settings.output_path = value;
        } else if (key == "--date") {
            settings.midnight = parse_date_midnight(value);
        } else if (key == "--symbol") {
            settings.replay.symbol = value;
        } else if (key == "--host") {
            settings.server.host = value;
            endpoint_given = true;
        } else if (key == "--port") {
            settings.server.port = parse_port(value);
            endpoint_given = true;
        } else if (key == "--decode" && is_oe) {
            settings.decode_path = value;
        } else {
            throw std::invalid_argument("unknown option: " + std::string(key));
        }
    }

    if (!settings.decode_path.empty()) {
        if (argc != 3) {
            throw std::invalid_argument("--decode takes a file and runs by itself");
        }

        return settings;
    }

    const bool file_output = !settings.output_path.empty();

    if (settings.csv_path.empty() || settings.tcp == file_output) {
        throw std::invalid_argument("use --csv with exactly one of --out FILE or --tcp");
    }

    if (endpoint_given && !settings.tcp) {
        throw std::invalid_argument("--host and --port require --tcp");
    }

    return settings;
}

void print_oe_message(const Message& message) {
    if (const auto* order = std::get_if<NewOrder>(&message)) {
        std::cout << "NewOrder id=" << order->client_order_id << " symbol=" << order->symbol
                  << " status=" << order->status << " trade_id=" << order->trade_id
                  << " side=" << order->side << " qty=" << order->qty
                  << " limit_px_x10000=" << order->limit_px << '\n';
    } else if (const auto* report = std::get_if<ExecReport>(&message)) {
        std::cout << "ExecReport id=" << report->client_order_id << " status=" << unsigned(report->status)
                  << " filled_qty=" << report->filled_qty << " avg_px_x10000=" << report->avg_px
                  << " reason=" << unsigned(report->reason_code) << '\n';
    }
}

void inspect_oe(const std::string& path) {
    std::ifstream input(path, std::ios::binary);

    if (!input) {
        throw std::runtime_error("cannot open inbound binary file: " + path);
    }

    OeReceiver receiver;
    std::array<std::byte, 17> chunk;

    while (input.read(reinterpret_cast<char*>(chunk.data()), chunk.size()) || input.gcount() != 0) {
        const auto size = static_cast<std::size_t>(input.gcount());
        receiver.receive(std::span(chunk).first(size), print_oe_message);
    }

    if (input.bad()) {
        throw std::runtime_error("inbound binary read failed");
    }

    receiver.finish();
}

std::size_t replay_file(const Settings& settings, const CsvData& data, const ReplayClock& clock) {
    if (std::filesystem::exists(settings.output_path) &&
        std::filesystem::equivalent(settings.csv_path, settings.output_path)) {
        throw std::runtime_error("output must not overwrite the input CSV");
    }

    std::ofstream output(settings.output_path, std::ios::binary);

    if (!output) {
        throw std::runtime_error("cannot open binary output: " + settings.output_path);
    }

    const auto count = replay(data, settings.replay, clock, [&](auto bytes) {
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        output.flush();

        if (!output) {
            throw std::runtime_error("binary output write failed");
        }
    });

    output.close();

    if (!output) {
        throw std::runtime_error("binary output close failed");
    }

    return count;
}

void run(const Settings& settings) {
    if (!settings.decode_path.empty()) {
        inspect_oe(settings.decode_path);
        return;
    }

    std::ifstream input(settings.csv_path);

    if (!input) {
        throw std::runtime_error("cannot open CSV: " + settings.csv_path);
    }

    const auto data = read_csv(input, settings.midnight);
    auto clock = wall_clock();

    if (settings.no_wait) {
        clock.wait_until = [](auto) {};
    }

    const auto count = settings.tcp
        ? replay_tcp(data, settings.replay, clock, settings.server, print_oe_message)
        : replay_file(settings, data, clock);

    std::cerr << "Replayed " << count << (is_oe ? " trades" : " quotes")
              << (settings.no_wait ? " (no waiting)" : " (wall-time relative)") << '\n';
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--help")) {
        usage();
        return argc == 1 ? 1 : 0;
    }

    try {
        run(read_settings(argc, argv));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
