#!/usr/bin/env bash
# sober-exec-launch: entrypoint invoked by the .desktop file.
#
# Starts the GUI, and if Sober is installed as a flatpak but not currently
# running, offers to launch it with the payload preloaded. Otherwise just
# opens the GUI and lets the user handle Sober themselves.

set -e

PAYLOAD_SO="/usr/lib/sober-exec/libsoberpay.so"
APP_ID="org.vinegarhq.Sober"
APP_DATA="${HOME}/.var/app/${APP_ID}/data"
PAYLOAD_DST="${APP_DATA}/sober-exec/libsoberpay.so"

have_flatpak_sober() {
    command -v flatpak >/dev/null 2>&1 && \
        flatpak info "${APP_ID}" >/dev/null 2>&1
}

sober_running() {
    pgrep -f "RobloxPlayerBeta" >/dev/null 2>&1
}

install_payload() {
    mkdir -p "${APP_DATA}/sober-exec"
    cp -f "${PAYLOAD_SO}" "${PAYLOAD_DST}"
    chmod 0755 "${PAYLOAD_DST}"
}

launch_sober() {
    install_payload
    setsid flatpak run \
        --env=LD_PRELOAD="${PAYLOAD_DST}" \
        --env=SOBER_EXEC=1 \
        --filesystem=xdg-runtime-dir \
        "${APP_ID}" >/dev/null 2>&1 &
}

if have_flatpak_sober && ! sober_running; then
    launch_sober
fi

exec sober-exec-gui "$@"
