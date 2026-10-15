# quantSys agent contract

Historical NASDAQ ITCH research engine. Reconstruct books, measure microstructure
hypotheses, and report costs and dropped events. This is not a live trading system.

## Non-goals

- Do not add FIX, OUCH, broker APIs, or any path that submits an order.
- Do not implement spoofing, layering, quote stuffing, or other manipulation.
- A positive backtest is not permission to trade. Half-spread is not a fill model.
- Do not commit ITCH files, CSVs under `data/`, or secrets.

## Build and test

From a Linux or WSL shell at the repo root:

```bash
bash scripts/ci.sh
```

That is a sanitized configure, build, `ctest`, and `docker build`. Set
`QUANT_CI_SKIP_DOCKER=1` if docker is not on PATH. Reported numbers need a
run record (`*.manifest.json` next to the CSV) so the commit, input hash,
parameters, and drop counts travel with them. WSL wipes `/tmp` on reboot;
keep ITCH tapes on a persistent path (`ITCH_DIR`) and CSVs under `data/`.

Portable release (Docker, default): `-march=x86-64-v3`. WSL latency benches:
`-DQUANT_NATIVE_ARCH=ON`. Do not treat WSL `max` latency as a tuning target.

## Engineering

- Measure before changing a hot path. Keep `src/bench_*.cpp` as the regression guard.
- No heap allocation on a book hot path after construction. RAII for every resource.
- New behavior starts with a failing test. Do not weaken or delete tests to go green.
- Count dropped adds, cancels, and executes. A silent `false` is not a successful replay.
- Bounds-check ITCH input. Log rejects at the replay boundary, not per message in a tight loop.
- Skills that apply: C/C++ systems, performance, Docker, tests, error handling,
  verification, docs, secure coding, plan-before-building. Skip unrelated stacks.

## Later

`viewer/index.html` reads study CSVs and run records from disk. It is not
part of the engine and has no network. Live connectivity requires a
separate decision, a market-data license, and a broker account.
