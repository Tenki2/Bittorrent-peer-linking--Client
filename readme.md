# Bittorrent-peer-linking

`btclient` is a small libtorrent-based BitTorrent CLI that now writes structured per-session telemetry for later linkage analysis.

## Build

From [`src/`](/home/user/Bittorrent-peer-linking/src):

```bash
cmake -S . -B build
cmake --build build
```

## Run

Torrent file example:

```bash
./build/btclient -f big.torrent -t 60 --node-role adversary
```

Magnet example:

```bash
./build/btclient -f "magnet:?xt=urn:btih:..." --snapshot-interval-ms 1000
```

Useful flags:

- `--run-id` and `--session-id` to supply explicit identifiers.
- `--artifacts-dir` to change the base output directory.
- `--event-log` and `--summary-json` to override the default artifact paths.
- `--no-console-status` to suppress the periodic human-readable status line.

## Artifacts

By default each run writes artifacts under:

```text
./artifacts/<run_id>/<session_id>/
```

Files:

- `session_events.ndjson`: raw newline-delimited runtime event log with UTC and monotonic timestamps.
- `session_summary.json`: final structured session summary with global metrics and per-peer derived features.

The NDJSON stream preserves the raw timeline, while the JSON summary collects derived metrics such as per-peer request gaps, block latencies, connection durations, rate summaries, and state-transition counts.
