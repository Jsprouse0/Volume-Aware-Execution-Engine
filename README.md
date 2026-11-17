# Volume-Aware Execution Engine

A C++20 market replay and execution-engine project through **Milestone 2**.
`slipstream` listens on separate TCP ports for quotes and trades, tracks one
instrument’s best bid and ask, and reports a trailing, trade-weighted VWAP.
The project currently covers feed ingestion and market state. Order decisions
and latency benchmarking are the next milestones.

## Build and test

Use CMake 3.20+ and a C++20 compiler with signed `__int128` support (64-bit
GCC or Clang on Linux, MinGW-w64 GCC on Windows). The core uses the standard
library and native sockets.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

On this Windows machine, CMake and Ninja are bundled with CLion. If they are not on
PATH, these PowerShell commands use the installed tools:

```powershell
$cmake = 'C:\Program Files\JetBrains\CLion 2026.2.3\bin\cmake\win\x64\bin\cmake.exe'
$ctest = 'C:\Program Files\JetBrains\CLion 2026.2.3\bin\cmake\win\x64\bin\ctest.exe'
& $cmake -S . -B build -G Ninja '-DCMAKE_MAKE_PROGRAM=C:/Program Files/JetBrains/CLion 2026.2.3/bin/ninja/win/x64/ninja.exe' -DCMAKE_CXX_COMPILER=C:/msys64/mingw64/bin/g++.exe -DCMAKE_BUILD_TYPE=Debug
& $cmake --build build
& $ctest --test-dir build --output-on-failure
```

In CLion, reload the CMake project and run **All Tests** from the run
configuration dropdown. CTest supplies the sample CSV path automatically.
For the TCP demo with a broken embedded terminal, use separate CMake Application
configurations for `slipstream`, `md_client`, and `oe_client`. Set each
working directory to the repository root, copy the arguments below into
**Program arguments**, and start the server first.

## M2 TCP demo

Run these in three sessions from the repository root. Wait for the server's
`LISTEN` line before starting the clients. The clients send every symbol so
the server can demonstrate filtering.

```sh
./build/slipstream --symbol SYNTH4 --vwap-window-ms 300
./build/md_client --tcp --csv examples/short_replay.csv --date 2026-10-27
./build/oe_client --tcp --csv examples/short_replay.csv --date 2026-10-27
```

In Windows PowerShell use `.\build\slipstream.exe`, `.\build\md_client.exe`,
and `.\build\oe_client.exe` with the same arguments. MD defaults to
`127.0.0.1:14200` and OE to `127.0.0.1:14300`. Change them on the server with
`--md-host`/`--md-port` and `--oe-host`/`--oe-port`; pass the matching
`--host` and `--port` to each client. Hosts are numeric IPv4 addresses.

The server prints one VWAP line per selected trade, then a summary after both
feeds reach EOF. With this example, the second trade lands exactly 300 ms
after the first in event time. Both are included at the window boundary.

```text
SUMMARY symbol=SYNTH4 quotes=2 trades=2 ignored_quotes=1 ignored_trades=1 window_qty=60 vwap_x10000=341883 bid_px_x10000=341700 bid_qty=800 ask_px_x10000=341800 ask_qty=1000
```

That VWAP is `34.1883`. Run with `--vwap-window-ms 299` and the result becomes
`34.1800` on 10 units. To exercise all 10,000 rows quickly, set the server's
window to `30000`, then use `--csv QandT.csv --no-wait` on both clients.
Omit `--no-wait` for real-time delivery. The full replay is about an hour.
With the 30,000 ms window, the full run reports 1,572 matching quotes, 928
matching trades, 5,333 ignored quotes, and 2,167 ignored trades. The final
SYNTH4 window has 1,725 units and a VWAP of `33.5171`.

## M1 file demo

Run from the repository root in PowerShell:

```powershell
.\build\md_client.exe --csv examples/short_replay.csv --date 2026-10-27 --symbol SYNTH4 --out build/quotes.bin
.\build\oe_client.exe --csv examples/short_replay.csv --date 2026-10-27 --symbol SYNTH4 --out build/trades.bin
Format-Hex build/quotes.bin
Format-Hex build/trades.bin
```

This writes two quotes (96 bytes) and two trades (90 bytes). Each pair is 300 ms
apart. Binary files retain CSV epoch timestamps, not actual file-write timestamps.
Inspect the scheduling test to see the exact deadlines asserted without OS jitter.

The supplied full dataset spans about an hour. For an immediate offline conversion:

```powershell
.\build\md_client.exe --csv QandT.csv --date 2026-10-27 --out build/all_quotes.bin --no-wait
.\build\oe_client.exe --csv QandT.csv --date 2026-10-27 --out build/all_trades.bin --no-wait
```

Real time is the default. `--no-wait` explicitly disables sleeping for inspection.
`--symbol` is optional; by default clients retain all symbols. Clock-style CSV
timestamps require an explicit UTC `--date`; numeric timestamps already represent
epoch nanoseconds. This sample carries no date or timezone, so the date is a replay
choice rather than recovered historical information.

## Replay and VWAP semantics

The MD executable sends Quote frames to a binary file sink. The OE executable
sends Trade frames to a binary file sink and has an offline receive mode:

```powershell
.\build\oe_client.exe --decode build/orders.bin
```

`orders.bin` must contain actual NewOrder/ExecReport frames, not the replayed Trade
file. `OeReceiver::receive()` accepts arbitrary byte chunks and calls a handler for
complete inbound messages. Tests exercise that receive path a byte at a time.
In TCP mode, OE half-closes its send side at the end of replay and drains the
read side using the same decoder. Live order handling during replay is a
future extension. MD sends only quotes and OE sends only trades in M2.

Both clients use the first **unfiltered CSV row** as their timestamp origin.
Each run captures its own `steady_clock` start, then uses absolute deadlines:

```text
deadline = replay_start + (row.ts_ns - csv.origin_ns)
```

Filtering preserves the original offsets. Overdue rows are sent without changing
later deadlines. OS scheduling, blocking sends, and independent process start
times can affect arrival time. TCP gives order within each connection. There is
no global merge across the two feeds.

VWAP uses the selected symbol's trade event time and includes trades in
`[trade timestamp - window, trade timestamp]`. Only its trade stream advances
the window. The fixed-point quotient truncates toward zero at display precision.
A fixed-capacity ring holds 65,536 live prints. Exceeding capacity fails the
session with a diagnostic. The inline comments and walkthrough show the bounds
for signed 128-bit notional and 64-bit volume.

The server runs one reader loop using blocking sockets. Readiness selects which
connection to read. Each stream owns its own frame decoder. The VWAP ring
allocates at setup. The codec uses strings and callback wrappers, and per-trade
logging performs I/O. The project makes no allocation-free or latency claim.

