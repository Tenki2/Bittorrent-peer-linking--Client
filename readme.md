# Bittorrent-peer-linking

`btclient` is a small libtorrent-based BitTorrent CLI that  writes structured per-session telemetry for later linkage analysis.

## Build

Dependencies:

- CMake and a C++17 compiler
- `libtorrent-rasterbar`
- `libcurl`
- `nlohmann_json`

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
- `--destination-url` or `--brain-url` to POST final artifacts as multipart form data.
- `--no-console-status` to suppress the periodic human-readable status line.

## Docker

```bash
sudo docker build -t btclient:dev .
sudo docker tag btclient:dev ghcr.io/tenki2/btclient:dev
sudo docker push ghcr.io/tenki2/btclient:dev
```



## Artifacts

By default each run writes artifacts under:

```text
./artifacts/pending/<session_id>/
```

Files:

- `state.json`: small lifecycle manifest used to decide whether a session is still being captured, ready to upload, or archived. Upload failures are also recorded here for later retry/debugging.
- `session_events.ndjson`: raw newline-delimited runtime event log with UTC and monotonic timestamps.
- `session_summary.json`: final structured session summary with global metrics and per-peer derived features.

The NDJSON stream preserves the raw timeline, while the JSON summary collects derived metrics such as per-peer request gaps, block latencies, connection durations, rate summaries, and state-transition counts.

## Artifact lifecycle

New sessions create `state.json` with `state: "capturing"`. After the event log is closed and the summary JSON is written, the state changes to `ready_to_upload`.

When `--destination-url` or `--brain-url` is configured, the client uploads the ready session artifacts and then moves the whole session directory to:

```text
./artifacts/archive/<session_id>/
```

On startup, the client first inspects `./artifacts/pending/`. Any session with `state: "ready_to_upload"` is uploaded before a new BitTorrent session starts. Uploads include `state.json`, `session_events.ndjson`, and `session_summary.json`. If an upload fails, `state.json` keeps `state: "ready_to_upload"` and records `last_upload_failure`, `upload_failures`, `upload_attempt_count`, and `last_upload_attempt_utc`. The process exits with code `3` and leaves the pending directory ready for a later retry. Directories with `state: "capturing"` are treated as incomplete captures and are not uploaded automatically.

Local-only runs without a destination URL still complete successfully. They leave the finished session in `pending/` with `state: "ready_to_upload"` so a later run with a destination URL can upload and archive it.
