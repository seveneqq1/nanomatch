# nanomatch

Header-only C++20 limit-order matching engine. The hot path (`OrderBook::process`) does not call `malloc` / `new`: every order slot, price level, account, hash bucket, and ring-buffer cell is claimed at construction from 64-byte-aligned pools.

Price-time priority, partial fills, market orders, and cancels. Ingress is a bounded MPSC ring (Vyukov); order slots are a Treiber stack with an ABA tag. Matching itself is single-threaded — that is how a contiguous FIFO book stays linearizable without a lock on the book.

## Layout

```
nanomatch/
├── CMakeLists.txt
├── src/core/types.hpp          # Order, PriceLevel, Trade, Command
├── src/core/memory_pool.hpp    # aligned pools, free list, MPSC/SPSC rings, hashes
├── src/core/order_book.hpp     # matching engine
├── src/main.cpp                # small live book demo
├── src/benchmark/matching_bench.cpp
├── scripts/export_latency_html.py
└── tests/order_book_tests.cpp
```

## Launch

You need **CMake 3.20+** and a **C++20** compiler (Clang 15+, Apple Clang, GCC 12+, or MSVC 19.3+ / Visual Studio 2022). Python 3 is optional and only used to rebuild the latency HTML chart.

Clone, then follow the section for your OS.

```bash
git clone https://github.com/seveneqq1/nanomatch.git
cd nanomatch
```

### macOS (Terminal)

Xcode command-line tools provide the compiler. Install CMake with Homebrew if `cmake --version` fails.

```bash
xcode-select --install
brew install cmake python3

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure

./build/nanomatch_demo
./build/nanomatch_bench docs/latency.html docs/latency.json
python3 scripts/export_latency_html.py -i docs/latency.json -o docs/latency.html
open docs/latency.html
```

### Linux

Debian / Ubuntu:

```bash
sudo apt update
sudo apt install -y cmake g++ python3

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure

./build/nanomatch_demo
./build/nanomatch_bench docs/latency.html docs/latency.json
python3 scripts/export_latency_html.py -i docs/latency.json -o docs/latency.html
xdg-open docs/latency.html
```

Fedora / RHEL: `sudo dnf install cmake gcc-c++ python3`. Arch: `sudo pacman -S cmake gcc python`.

### Windows (PowerShell or cmd)

Install [Visual Studio 2022](https://visualstudio.microsoft.com/) with the **Desktop development with C++** workload (MSVC) and [CMake](https://cmake.org/download/). Then from the repo root:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure

.\build\Release\nanomatch_demo.exe
.\build\Release\nanomatch_bench.exe docs\latency.html docs\latency.json
python scripts\export_latency_html.py -i docs\latency.json -o docs\latency.html
start docs\latency.html
```

Clang/GCC on Windows (MinGW, LLVM, or WSL) can use the same Unix-style commands as Linux, with binaries at `build\nanomatch_demo.exe` when the generator is Ninja or MinGW Makefiles:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
.\build\nanomatch_demo.exe
```

WSL2 is the Linux flow inside Windows: install Ubuntu from the Store, then follow the Linux section.

## Benchmark

`nanomatch_bench` seeds a 1,048,576-slot book, then times **1,000,000** mixed limit / market / cancel commands. Each sample is the monotonic-raw nanosecond delta around `process()`. It prints p50 / p90 / p99 / p99.9 and writes a standalone chart plus JSON.

Times are machine-specific. On Apple Silicon the virtual counter (`cntvct_el0`) typically ticks every ~42 ns, so a `0 ns` sample means the call finished inside one tick. Reproduce on an isolated core with turbo pinned if you are publishing numbers. A sample chart from this host lives in [`docs/latency.html`](docs/latency.html).

## Hot-path design

- **Orders and levels** occupy exactly one cache line (`alignas(64)`, `sizeof == 64`) so a FIFO walk does not pull adjacent producers' data.
- **Indices, not pointers**, in the intrusive lists: the pool can sit anywhere, and a slot is 32 bits instead of a 64-bit pointer plus tag.
- **Id and price maps** are open-addressed tables allocated once. Cancel is a probe plus an O(1) unlink.
- **Bids** are a high-to-low DLL of levels; **asks** low-to-high. The best price is the list head. Matching walks that head, then the FIFO at the level, prefetching the next order.
- **Markets** never rest. Unfilled remainder is canceled. Limits that still have quantity after the opposite side no longer crosses are appended to the tail of their price level (time priority).
- **Reports** go to SPSC rings. If a ring is full the match still happens; `dropped_reports()` counts the overflow. Drain from another thread.

Construct `OrderBook` once at process start. The object itself is small; the pools are the ~100 MiB working set for the default 2²⁰ orders.

```cpp
#include "core/order_book.hpp"

nanomatch::DefaultBook book;  // 1<<20 orders, 1<<14 levels
nanomatch::Command buy{
    /*id=*/1, /*price=*/100, /*qty=*/10, /*user=*/1,
    nanomatch::Side::Buy, nanomatch::CommandType::Limit};
book.process(buy);

nanomatch::Trade t{};
while (book.try_pop_trade(t)) { /* ... */ }
```

Template the capacities down for tests or a constrained binary:

```cpp
using Tiny = nanomatch::OrderBook<2048, 128, 64, 2048>;
```
