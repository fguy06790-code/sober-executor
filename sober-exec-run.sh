#!/usr/bin/env bash
# sober-exec-run.sh — launch Sober with the executor payload preloaded.
#
# Sober ships as the flatpak `org.vinegarhq.Sober`. We can't LD_PRELOAD from
# outside the sandbox directly, but we can:
#   1. Copy the payload into a path visible inside the sandbox
#      ($XDG_DATA_HOME/sober-exec is bind-mounted at the same path by the
#       app's filesystem permissions, OR we drop it under the app's own
#       data dir which is always exposed).
#   2. Invoke flatpak with --env to set LD_PRELOAD for the client process.
#   3. Grant --filesystem=xdg-runtime-dir so the unix socket is reachable
#      from the host (this is default for most flatpak apps; Sober already
#      has it, but we add it defensively).
#
# After launch, use ./build/sober-cli exec <file.lua> to run scripts.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PAYLOAD_SRC="${SCRIPT_DIR}/build/libsoberpay.so"

if [[ ! -f "${PAYLOAD_SRC}" ]]; then
    echo "payload missing: ${PAYLOAD_SRC}" >&2
    echo "run 'make' first, dipshit." >&2
    exit 1
fi

APP="org.vinegarhq.Sober"
APP_DATA="${HOME}/.var/app/${APP}/data"
PAYLOAD_DST_DIR="${APP_DATA}/sober-exec"
PAYLOAD_DST="${PAYLOAD_DST_DIR}/libsoberpay.so"

mkdir -p "${PAYLOAD_DST_DIR}"
cp "${PAYLOAD_SRC}" "${PAYLOAD_DST}"
chmod 0755 "${PAYLOAD_DST}"

# Path visible inside the sandbox: same path as the host sees under the
# app's data directory (~/.var/app/<id>/data is mounted at the same path).
IN_SANDBOX_PATH="${PAYLOAD_DST}"

echo "[sober-exec] payload: ${IN_SANDBOX_PATH}"
echo "[sober-exec] launching ${APP}..."

exec flatpak run \
    --env=LD_PRELOAD="${IN_SANDBOX_PATH}" \
    --env=SOBER_EXEC=1 \
    --filesystem=xdg-runtime-dir \
    "${APP}" "$@"
