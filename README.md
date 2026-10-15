# quant-sys

A low-latency market data parser, order book reconstructor, and matching
engine, built from scratch in C++20 to learn the systems-programming
discipline behind quant/HFT infrastructure: lock-free concurrency, zero-copy
parsing, cache-conscious data structures, and rigorous latency measurement.

Built and benchmarked on WSL2 (Ubuntu 22.04) on a 24-core host with AVX-512
support, GCC 11.4.0, `-O3 -march=native`.

**This is a learning/portfolio project, not a production trading system.**
See "What this is not" at the bottom for an explicit, honest accounting of
the gap between this and anything that could safely touch real markets.

## Architecture

Phases, each building on the last:

1. **Lock-free SPSC ring buffer** (`include/spsc_ring_buffer*.hpp`) --
   cache-line-padded, acquire/release vs. seq_cst memory ordering compared
   head-to-head against a mutex-guarded queue.
2. **ITCH 5.0 market data parser** (`include/itch/`) -- zero-copy,
   `mmap`-based parser for NASDAQ's real binary protocol, covering the full
   order lifecycle (Add, Add w/ MPID, Executed, Executed w/ Price, Cancel,
   Delete, Replace, Stock Directory, System Event).
3. **Order book + matching engine** (`include/book/`) -- flat-array
   price ladder (O(1) tick lookup, no tree), intrusive doubly-linked FIFO
   lists per price level, pool-allocated orders (no malloc on the hot
   path), price-time priority matching.

## Phase 1: Lock-free SPSC ring buffer

`include/spsc_ring_buffer.hpp` (acquire/release) and
`include/spsc_ring_buffer_seqcst.hpp` (seq_cst variant), benchmarked in
`src/bench_ring_buffer.cpp` against `std::mutex` + `std::queue`, with
producer/consumer pinned to separate cores and `_mm_pause()` in spin-wait
loops. 10M operations per run, latency measured with `rdtscp` (cycle-
accurate, not wall-clock).

| | acquire/release | seq_cst | mutex + queue |
|---|---|---|---|
| p50 | 40.6 ns | 81.3 ns | 50.8 ns |
| p99 | 152.4 ns | 213.4 ns | 1076.9 ns |
| p99.9 | 335.3 ns | 548.6 ns | 4084.2 ns |
| max | 1.84 ms | 2.06 ms | 5.23 ms |

**Findings:**
- Lock-free beats mutex by ~7x at p99, ~12x at p99.9 -- the real argument
  for lock-free structures is tail latency, not median throughput.
- seq_cst vs. acquire/release costs ~2x median latency for identical logic
  -- direct, measured evidence that memory ordering choice matters.
- `max` outliers (1.8-5.2 ms) on all three variants are WSL2 virtualization
  scheduling jitter, not the algorithm -- pinning a thread to a vCPU
  doesn't stop the Windows hypervisor from preempting that vCPU. On bare-
  metal Linux with `isolcpus` and real-time priority, these tails would be
  expected to tighten by orders of magnitude. p50/p99/p99.9 are the
  trustworthy numbers from this environment; `max` is not.

## Phase 2: ITCH 5.0 parser

`include/itch/` implements the byte-exact message layouts from NASDAQ's
official TotalView-ITCH 5.0 specification: big-endian field decoding
(including the 48-bit timestamp, which has no native integer type),
zero-copy parsing directly over an `mmap`'d file, and a callback-based
dispatcher (`src/itch/parser.hpp`).

**Correctness:** `src/test_itch_parser.cpp` builds synthetic messages for
all 8 implemented types, writes them with real 2-byte length-prefix
framing, parses them back through the real `mmap` + dispatch path, and
asserts every field round-trips exactly (including the fixed-point price
scaling). All assertions pass.

**Real-data validation:** `src/replay_itch.cpp` was run against a 511MB
decompressed slice of a real NASDAQ TotalView-ITCH production file
(2019-01-30, `emi.nasdaq.com`). Every message-type byte in the resulting
histogram was a real, documented ITCH type (`A C D E F H I L P Q R S U V
X Y`) -- strong independent evidence the byte offsets and framing
assumptions are correct, since any error would have desynced the parser
into garbage type bytes almost immediately.




**Honest caveats:**
- This throughput reflects warm page-cache reads (the file was recently
  decompressed), not disk I/O -- the right thing to measure for a feed
  handler, since production market data arrives over network/RAM, but
  worth stating precisely.
- An earlier version used `std::map<char, uint64_t>` for the message-type
  histogram; replacing it with a flat 256-entry array (O(1), no tree
  traversal) cut elapsed time from 1.21s to 0.48s on the same file --
  2.5x, from fixing our own bookkeeping, not the parser itself. Lesson:
  measure before concluding where time goes.

## Phase 3: Order book reconstruction + matching engine

### Data structures (`include/book/`)

- `order_pool.hpp` -- fixed-capacity pool allocator, index-based handles
  (not pointers), free-list threaded through the same array. No
  malloc/new after construction.
- `price_ladder.hpp` -- flat array indexed directly by price tick. O(1)
  lookup, bounded to a configurable price range per symbol (trades
  unbounded range for speed and cache locality -- see caveats below).
- `order_book.hpp` -- reconstructs live book state from a real ITCH event
  stream (Add/Cancel/Delete/Replace/Executed), incremental best-bid/ask
  tracking.
- `matching_engine.hpp` -- standalone price-time priority matcher:
  incoming orders match against crossing resting liquidity first (best
  price, FIFO within a price level), unfilled remainder rests in the book.

### Real-data order book reconstruction

`src/replay_symbol.cpp` does a two-pass replay of the real 511MB ITCH
file: pass 1 builds a `stock_locate -> ticker` map from Stock Directory
messages (8,713 symbols found, matching the file's own histogram count);
pass 2 replays the full file filtered to one resolved symbol, feeding its
real Add/Cancel/Delete/Replace/Executed events into a live `OrderBook`.

Example, ticker ZS (Zscaler), 2019-01-30 slice:



7 events were rejected (price outside the $1-$2000 ladder range).
Investigating them individually turned out to be a real finding, not a
bug: 6 of 7 were "stub quotes" -- market-maker obligation quotes
intentionally priced at extremes (e.g. a sell at $199,999.99 paired with
a buy at $0.01) with no genuine trading intent, a known real-world market
microstructure pattern. The 7th was a genuine low-priced odd-lot order
just below the configured floor. The bounded price ladder correctly
isolated both cases instead of corrupting book state.

### A real bug, found by testing

While writing the matching engine's FIFO-priority test, `submit_order()`
hung in an infinite loop. Root cause: `append_to_level()` only linked a
new order into a price level's intrusive list when the level was
*empty* -- appending a second order onto an already-occupied level never
set its `prev`/`next` pointers, silently orphaning it. The order was
counted in `order_count` but unreachable from `head`. When the first
order in the level was later consumed, the level's `head` was set to
`kNullIndex` (the orphaned order's untouched `next`), permanently losing
the second order while `order_count` still reported it present --
producing an infinite loop in any code that trusted `order_count` to
decide whether to keep scanning a level.

The identical bug existed in both `matching_engine.hpp` and
`order_book.hpp` (copy-pasted). Both were fixed to properly link into an
existing tail. Phase 3a's real-data test (`replay_symbol`) happened to
produce *identical* output before and after the fix for this particular
symbol/slice -- meaning this specific run never exercised the broken
path, not that the bug was harmless. This is the clearest lesson from the
whole project: passing tests, and even successful real-data runs, only
prove what they actually exercise.

### Matching engine latency benchmark

`src/bench_matching_engine.cpp`: 2,000,000 synthetic orders (random walk
mid-price, mixed resting/crossing), single-threaded (matching is
inherently sequential per symbol), pinned to one core, `rdtscp`-timed per
`submit_order()` call.

First run (`std::unordered_map` for order lookup, default growth):


After adding `order_locations_.reserve(max_orders)` in the constructor
(eliminating hash-map rehash stalls during the run):


**One fix, measured before and after: p50 improved 25%, p99 improved
33%, max improved 97% (31x).** This is the project's clearest example of
"measure, find the real bottleneck, fix it, re-measure" rather than
guessing.

**Honest caveat on the remaining `max`:** 1.49ms is still ~16,500x the
median. This is very likely the same WSL2 scheduler jitter documented in
Phase 1 -- consistent in shape and order of magnitude -- but this
specific run was not independently isolated to confirm that over disk
I/O, page faults, or other causes. p50/p90/p99/p99.9 are the
trustworthy, reproducible numbers; `max` on this platform is not.


## Where the time actually goes

A native `-march=native` run of `bench_matching_engine` (2,000,000
orders, same seed) is about 89 ns at p50. Splitting that workload
showed two different costs:

- On a tight book the median is the hash-map update. Replacing it with
  an open-addressed table sized to the full pool cap made p50 worse
  (about 90 ns to about 220 ns): the table was hundreds of megabytes
  and no longer fit in cache. That change was reverted. `OrderBook`
  now reserves its map at construction, which is the same rehash fix
  the matcher already had.
- On a gapped book, walking every empty cent to find the next bid or
ask was the tail. `./build-native/bench_gap_scan` is that workload
(400,000 orders, 10-cent grid). Before the bitset its p99 was about
1,980 ns; after `occupied_ticks.hpp`, a native run reported about 340 ns. The dense official bench moved less, because
  that book rarely has a wide hole: p90 179 ns to 148 ns, p99 418 ns
to 376 ns. p50 stayed 89 ns. Cross checks now compare integer tick
prices, so a double rounding step is not part of the match decision.

`max` is still WSL2 scheduler noise. Do not tune for it. `perf` is not
installed in this WSL distro; the numbers above are `rdtscp` percentiles.

## Phase 4: Pre-trade risk layer

`include/book/risk_engine.hpp` + `include/book/risk_gated_engine.hpp`.

Every real trading system treats risk controls as more important than
speed. `RiskGatedEngine` wraps `MatchingEngine` so every order passes a
mandatory pre-trade check before it can touch book state:

- **Max order size / max order value** -- catches oversized orders and
  fat-finger price errors (e.g. an extra zero on the price).
- **Price collar** -- rejects orders priced too far from the live best
  bid/ask. Directly validated against the real stub quotes found in
  Phase 3a real NASDAQ data ($199,999.99 against a $45.61 reference) --
  a genuine production anomaly this project discovered, now defended
  against by name in a test (`test_risk_gated_engine.cpp`).
- **Position limits** -- tracks net long/short position, rejects orders
  that would breach a configured max; verified to update only on
  approved orders, never on rejects.
- **Kill switch** -- a single flag that blocks all new orders
  immediately, independent of every other check.

Rejections carry a specific `RiskRejectReason` (not just a bool) --
`OrderSizeExceedsMax`, `OrderValueExceedsMax`, `PriceOutsideCollar`,
`PositionLimitBreach`, `KillSwitchActive` -- so a rejection is always
explainable, not just a dead end.

**Correctness:** `test_risk_engine.cpp` (13 assertions, each risk check
in isolation) and `test_risk_gated_engine.cpp` (6 assertions, full
integration: confirms rejected orders never reach the matching engine,
and approved orders flow through and update position tracking exactly
as the unwrapped engine did). All 43 assertions across the full project
test suite pass.

## Phase 5: Multi-symbol market reconstruction

`include/book/symbol_router.hpp` (synthetic multi-symbol trading, wraps
`RiskGatedEngine` per symbol) and `include/book/multi_symbol_book.hpp`
(real exchange-event reconstruction, wraps the passive `OrderBook` per
symbol -- correct semantics for replaying real ITCH data, where NASDAQ's
own matching engine already decided what crossed; feeding real Add
messages through our own matcher would be semantically wrong, not just
imprecise).

`src/replay_full_market.cpp` reconstructs every symbol in the real
511MB ITCH file simultaneously -- 8,713 real symbols, 8,693 with
activity, ~12M events applied, in ~12 seconds (~1.38M messages/sec,
lower than Phase 2's single-symbol throughput because this does real
per-message book work -- hash lookup, ladder indexing, intrusive list
updates -- across thousands of live books, not just histogram counting).

Results are plausible across real, familiar names: AAPL $162.07/$162.12,
TSLA $300.32/$300.60, AMZN (pre-split) $1,628.04/$1,628.87, MSFT
$104.67/$104.69 -- all broadly consistent with general knowledge of
where these traded in January 2019, though not independently verified
against a primary historical quote source.

### Two real memory bugs, found by actually running it

Getting this working required finding and fixing two genuine bugs, both
through evidence rather than guessing:

1. **Naive global price range OOM'd immediately.** A first attempt gave
   every symbol the same $1-$2000 ladder (reusing Phase 3a's single-
   symbol range) with 50,000 orders of capacity each. Across 8,713
   symbols that is roughly 80+ GB of flat-array price levels alone --
   killed by the Linux OOM killer on a 15GB machine. Fix: size each
   symbol's price band dynamically around its own first observed price
   (+/-50%), since a $6 stock and a $1,600 stock have no business
   sharing one global range.
2. **Dynamic sizing then OOM'd again, for a different reason.** If a
   symbol's *first* observed message happened to be a stub quote (the
   same $199,999.99-style extreme prices documented in Phase 3a), its
   band exploded to roughly +/-$100,000 -- about 960MB for that one
   symbol alone. Fix: clamp the computed band to an absolute $1,000
   maximum half-width regardless of the percentage calculation. This is
   a direct, concrete case of a finding from one phase (Phase 3a's stub
   quotes) resurfacing as a real bug two phases later -- a good argument
   for documenting anomalies thoroughly even when they seem resolved at
   the time.

### Known limitation

The most liquid symbols (AAPL, TSLA, NFLX, AMD, QQQ) hit or nearly hit
the configured 5,000-order-per-symbol cap in this run, meaning the very
top of real order flow for the busiest names is truncated, safely
(`add_order` cleanly returns false, no corruption) but not completely
captured. A production-shaped version would size each symbol's capacity
from observed activity rather than one shared constant.

**Correctness:** `test_symbol_router.cpp` (6 assertions: per-symbol
isolation of books, fills, positions, and kill switches) and
`test_multi_symbol_book.cpp` (4 assertions, including a direct
regression test for the global-range truncation bug). All 53 assertions
across the full project test suite pass.

## Phase 5.5: Locate-indexed routing, and a corrected optimization claim

`include/book/locate_indexed_book.hpp`. Phase 5's `MultiSymbolBook`
routes orders by hashing a `std::string` ticker symbol on every single
message. `stock_locate` -- the actual identifier ITCH messages carry --
is already a small, dense `uint16_t`. Routing directly by locate into a
flat array removes string construction and hashing from the hot path
entirely.

**First measurement, Add-order-only (`bench_lookup_cost.cpp`, Stages
1-5): a 2.1x speedup** (1.86M -> 3.84M msgs/sec for identical real book
mutation work, isolating only the lookup mechanism). This looked like a
clean, complete win.

**It was not the full story.** Wiring the fix into
`replay_full_market.cpp` and re-running against the real file showed
only a ~1.12x improvement (1.38M -> 1.55M msgs/sec) -- nowhere near the
isolated 2.1x. Rather than report the more flattering isolated number,
the discrepancy was treated as a hypothesis to test: the Add-only
benchmark never exercised Cancel/Delete/Execute/Replace, which are
roughly half of all real message volume (Phase 2's histogram: ~8.04M
non-add vs ~7.3M add messages) and involve real intrusive-list removal
and pool deallocation -- genuine mutation cost that has nothing to do
with how the symbol was looked up, and so cannot benefit from a faster
lookup.

Adding Stages 6-7 (full real message mix, both indexing schemes, same
process, same file) confirmed it directly: **~1.45x speedup** when all
five message types are included, not 2.1x. The Add-only test overstated
the real win because it excluded over half of the actual workload.

**The honest number for this optimization is ~1.45x** (controlled,
same-process, full-message-mix comparison), not the more impressive
2.1x an incomplete benchmark produced. Reporting the smaller, correctly-
measured number here is the point: an easy, flattering result is not
automatically the correct one, and the only way to know the difference
is to test the workload that actually matters, not the one that is
easiest to isolate.

## Phase 6: Sharded full-market replay

`include/book/sharded_replay.hpp`. One parser thread walks the tape in
order and pushes each book event onto a Phase-1 SPSC ring chosen by
`stock_locate % N`. Each worker thread owns that shard's
`LocateIndexedBook`. Locates never share a book, so there is no lock on
the hot path — only lock-free hand-off and `_mm_pause` when a ring is
full.

`--shards 1` is the default (parser plus one worker). `--shards 8` is
the 24-core host using a third of the cores. Applied counts and drop
counts must match the single-thread book; `test_sharded_replay` checks
1/2/4 shards against a sequential replay of the same synthetic tape.
This is still historical reconstruction. It does not send an order.

## Building and running

Linux or WSL (Ubuntu 22.04), from the repo root. Test binaries keep
`assert` enabled. Release replay and bench binaries define `NDEBUG`.

```bash
bash scripts/ci.sh
```

Same steps without Docker: `QUANT_CI_SKIP_DOCKER=1 bash scripts/ci.sh`, or
the CMake lines that script runs (`-DQUANT_SANITIZE=ON`, then `ctest`).

| CMake option | Default | Effect |
|---|---|---|
| `CMAKE_BUILD_TYPE` | `Release` | `-O3` |
| `QUANT_NATIVE_ARCH` | `OFF` | `ON` adds `-march=native` for WSL latency benches. Leave off for a portable build (`-march=x86-64-v3`). |
| `QUANT_SANITIZE` | `OFF` | AddressSanitizer and UndefinedBehaviorSanitizer. |

```bash
./build/replay_itch <path-to-itch-file>
./build/replay_symbol <path-to-itch-file> <TICKER>
./build/replay_full_market <path-to-itch-file>
./build/bench_ring_buffer
./build/bench_matching_engine
```

Full-market replay prints `price_out_of_band`, `pool_exhausted`, and
`unknown_order` drop counts. A run that drops events did not reconstruct
a complete book.

Docker runs the same tests in a non-root image. It does not copy market
data into the image. Put ITCH files in `data/` (gitignored) and mount
that directory:

```bash
docker build -t quantsys:local .
docker run --rm quantsys:local

docker compose build
docker compose run --rm quantsys
docker compose run --rm --entrypoint /src/build/research_imbalance quantsys \
  /data/your-itch-file AAPL /data/AAPL_imbalance.csv
```

Real ITCH sample data: https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/ --
free, several GB per trading day, standard 2-byte length-prefixed framing.
Do not commit those files.

## Imbalance study

`research_imbalance` replays one ticker through a single book with a large
order pool (the full-market cap of 5,000 orders per symbol truncates liquid
names). Every 100ms of ITCH time, if the book is two-sided, it records mid,
spread, top-of-book imbalance `(bidSize - askSize) / (bidSize + askSize)`,
and microprice. The forward mid is the first later sample at least one
second ahead. The cost haircut is `max(spread, 0) / 2`.

```bash
./build/research_imbalance <path-to-itch-file> AAPL
```

That writes `data/<date>/AAPL_imbalance.csv`, `AAPL_decisions.csv`, and
`AAPL_features.csv`, each with a `.manifest.json` run record (git commit,
input size and FNV-1a hash, parameters, drop counts). Short and truncated
ITCH frames are counted and not parsed.

The printed report includes regular-hours median spread, the one-cent
fraction, next-level gaps, drop counts, imbalance deciles after a
half-spread haircut, and a walk-forward least-squares baseline. Half the
spread is a lower bound, not a fill. Queue position and adverse selection
are not in that table.

`--maker-rebate` and `--taker-fee` are per-share parameters, default 0,
recorded in the run record. They are not Nasdaq's schedule.

### Fill model and features

On each regular-hours one-cent sample the study places a hypothetical
100-share bid at the touch and a 100-share ask at the touch. Queue ahead
starts as displayed size at that tick. Executions and cancels of orders
that arrived earlier reduce it. The order fills when later volume at that
price covers 100 shares, or when the opposite touch reaches its price
(trade-through). It is pulled after 10 seconds or a move of more than one
tick. Markouts at +1s, +5s, and +30s are signed mid minus fill (bid) or
fill minus mid (ask).

Not modeled, and printed every run: hidden liquidity (`P` trades never
touch the displayed queue), the order's own effect on other traders, and
latency.

Features written per sample, using only events at or before that time:
top imbalance, microprice minus mid, spread, 1s order-flow imbalance
(Cont, Kukanov, Stoikov 2014), 5-tick depth imbalance, 1s signed trade
volume, 1s mid change, minutes since the open. Targets are forward mid
changes at 1s, 5s, and 30s.

### Held-out-day evaluator

`research_evaluate` takes a list of features CSVs. Per ticker and horizon
it holds out one day, standardizes and fits ridge on the others (lambda
from an inner hold-out of training days), and scores:

- cross the spread when `|pred|` exceeds half-spread plus taker fee
- rest passively on the predicted side, taking the fill-model outcome

Baselines: no trade, imbalance-only, and the outer-decile rule with
thresholds from training days only (the older whole-day decile cut is
look-ahead). Output is `data/eval/<TICKER>_by_day.csv` plus a printed
mean across days with a seeded bootstrap 95% interval. A positive mean
after cost is not permission to send orders.

On the ten default days and five names, leave-one-day-out ridge **mean
after cost is negative** for every ticker and horizon. Seeded bootstrap
95% intervals sit entirely below zero for AAPL and MSFT at 1s/5s/30s, for
AMD at 1s/5s, for INTC at 5s, and for QQQ at 1s. The remaining intervals
(AMD 30s, INTC 1s/30s, QQQ 5s/30s) include zero. Imbalance-only is
usually no trades (`|pred|` below half-spread). Train-only outer-decile
and passive fill-model markouts are more negative. One-symbol studies
report `pool_exhausted=0` and `Frames: short=0`; they do not raise the
full-market pool cap. Half-spread is a lower bound, not a fill. Queue
position is not a ridge feature. Hidden liquidity, own size, latency, and
Nasdaq's fee schedule are not modeled. This is not a tradable edge.

The evaluator also reports (1) correlation on samples at least one horizon
apart, because 100 ms rows share the same second of mid; (2) mean after
cost per sample, counting no-trade rows as zero; (3) the same cross rule
restricted to one-cent spreads; (4) leave-one-feature-out after-cost
means; (5) mean standardized ridge weights. Overlapping-sample
correlation is not evidence of a forecast.

### Scripts

```bash
bash scripts/ci.sh                          # sanitized build, ctest, docker
ITCH_DIR=/home/ael/itch scripts/fetch_itch_day.sh 12302019
scripts/check_golden.sh 12302019 /home/ael/itch   # count-only replay check
ITCH_DIR=/home/ael/itch scripts/run_study.sh      # 10 days x 5 names
```

`fetch_itch_day.sh` retries, checks free disk, skips days already present,
and refuses ITCH v2 `S0*` files. Default `ITCH_DIR` is `/tmp/itch`; WSL
wipes `/tmp` on reboot, so use a persistent path. CSVs stay under `data/`
(gitignored). Default days: 01302019 03272019 07302019 08302019 10182019
10302019 12302019 01302020 07132021 08132021. Default tickers: AAPL MSFT
AMD INTC QQQ.

Full-market replay sizes each locate's pool from a pass-1 peak of adds
minus deletes, capped by `--memory-budget-mb` (default 4096). Locates the
budget cuts are named. `--fixed-cap N` restores the old uniform cap.

### Viewer

Open `viewer/index.html` (locally, or a directory server) and choose from
disk: imbalance CSV, decisions CSV, by-day evaluator CSV, features CSV,
and a `.manifest.json`. The page does not fetch anything. Empty and
header-only files show the empty state.

## What this is not

Being direct about the gap between this project and anything that could
safely or legally touch real markets, because no amount of code speed
closes it:

- No connectivity to anything real. This parses static files and runs
  synthetic in-process order flow. Real exchange connectivity needs
  colocation, exchange membership or a broker/FIX relationship, licensed
  real-time market data feeds (often thousands of dollars/month), and
  compliance with exchange and SEC/FINRA rules.
- Risk controls exist but are intentionally basic. Phase 4 adds order
  size/value limits, a price collar, net position limits, and a kill
  switch -- real risk-control *shapes*, tested in isolation and in
  integration. What is still missing from anything production-grade:
  margin/capital checks against actual account balance, real-time P&L
  tracking, multi-symbol aggregate exposure limits, regulatory reporting
  (e.g. Reg SHO, large trader reporting), and human-in-the-loop approval
  workflows for limit changes. A real desk also treats the risk
  configuration itself as sensitive, audited, and change-controlled --
  none of that process exists here, only the mechanism.
- No order gateway / FIX engine. There is no code here that could send
  a real order to a real exchange or broker. `research_imbalance` is a
  historical hypothesis report. It does not become a strategy because a
  decile is positive.
- Single symbol, single thread, bounded price range. Deliberate scope
  choices for this project, not production constraints solved.
- "Fast" is relative and WSL2-measured. All latency numbers were taken
  in a virtualized dev environment on one machine. They demonstrate
  real, reproducible relative improvements (lock-free vs. mutex, flat
  array vs. tree, reserved vs. growing hash map) -- not an absolute
  claim about being competitive with any real trading infrastructure,
  which also depends enormously on network path, colocation distance,
  and exchange-side matching engine behavior, none of which this
  project touches.

What this project is good for: demonstrating, with real measurements and
a real bug found and fixed, the systems-programming fundamentals
(lock-free concurrency, zero-copy parsing, cache-conscious data
structures, rigorous latency benchmarking) that quant/HFT infrastructure
engineering roles actually interview for.
