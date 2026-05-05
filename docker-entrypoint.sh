#!/usr/bin/env bash
set -euo pipefail

: "${BT_TORRENT_SOURCE:=/opt/torrents/big.torrent}"
: "${BT_RUNTIME_SECONDS:=60}"
: "${BT_LISTEN_PORT:=51413}"
: "${BT_NODE_ROLE:=unknown}"
: "${BT_ARTIFACTS_DIR:=/app/artifacts}"
: "${BT_SAVE_PATH:=/app/data}"
: "${BT_SNAPSHOT_INTERVAL_MS:=1000}"
: "${BT_CONSOLE_STATUS:=1}"

: "${BT_ANNOUNCE_IP:=}"
: "${BT_RUN_ID:=}"
: "${BT_SESSION_ID:=}"
: "${BT_DESTINATION_URL:=}"
: "${BT_CLIENT_LABEL:=}"
: "${BT_PROFILE_ID:=}"

: "${BT_PRESEED_SOURCE:=}"
: "${BT_PRESEED_TARGET:=}"

mkdir -p "${BT_ARTIFACTS_DIR}" "${BT_SAVE_PATH}"

# Role-based preseed handling:
# Seeder runs copy the baked-in payload into the active save path.
# Other roles keep the active save path empty/fresh.
if [[ "${BT_NODE_ROLE}" == "adversary" ]]; then
    if [[ -z "${BT_PRESEED_SOURCE}" || -z "${BT_PRESEED_TARGET}" ]]; then
        echo "Adversary role requires BT_PRESEED_SOURCE and BT_PRESEED_TARGET" >&2
        exit 1
    fi

    mkdir -p "$(dirname "${BT_PRESEED_TARGET}")"
    cp -a "${BT_PRESEED_SOURCE}" "${BT_PRESEED_TARGET}"
    echo "Preseeded payload copied to ${BT_PRESEED_TARGET}"
else
    echo "Node role is ${BT_NODE_ROLE}; no preseed payload copied into active save path."
fi

args=(
    -f "${BT_TORRENT_SOURCE}"
    -t "${BT_RUNTIME_SECONDS}"
    -p "${BT_LISTEN_PORT}"
    --node-role "${BT_NODE_ROLE}"
    --artifacts-dir "${BT_ARTIFACTS_DIR}"
    --save-path "${BT_SAVE_PATH}"
    --snapshot-interval-ms "${BT_SNAPSHOT_INTERVAL_MS}"
)

if [[ -n "${BT_ANNOUNCE_IP}" ]]; then
    args+=( -i "${BT_ANNOUNCE_IP}" )
fi

if [[ -n "${BT_RUN_ID}" ]]; then
    args+=( --run-id "${BT_RUN_ID}" )
fi

if [[ -n "${BT_SESSION_ID}" ]]; then
    args+=( --session-id "${BT_SESSION_ID}" )
fi

if [[ -n "${BT_DESTINATION_URL}" ]]; then
    args+=( --destination-url "${BT_DESTINATION_URL}" )
fi

if [[ -n "${BT_CLIENT_LABEL}" ]]; then
    args+=( --client-label "${BT_CLIENT_LABEL}" )
fi

if [[ -n "${BT_PROFILE_ID}" ]]; then
    args+=( --profile-id "${BT_PROFILE_ID}" )
fi

if [[ "${BT_CONSOLE_STATUS}" == "0" ]]; then
    args+=( --no-console-status )
fi

echo "Launching btclient with:"
printf '  %q' /usr/local/bin/btclient "${args[@]}"
printf '\n'

exec /usr/local/bin/btclient "${args[@]}"