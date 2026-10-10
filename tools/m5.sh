#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Flash or tail the Fire, addressing it only by its stable by-id path.
# Kernel names (/dev/ttyACM*) renumber across replugs and would target
# whichever board happens to hold that number.
#
# Usage: tools/m5.sh {upload|logs|run} [config.yaml]
set -euo pipefail

DEVICE_ID="usb-1a86_USB_Single_Serial_5864000646-if00"
DEVICE="/dev/serial/by-id/${DEVICE_ID}"

if [[ $# -lt 1 ]]; then
    echo "usage: $0 {upload|logs|run} [config.yaml]" >&2
    exit 2
fi
action="$1"
config="${2:-$(dirname "$0")/../esphome/hydronic-s3.yaml}"

if [[ ! -e $DEVICE ]]; then
    echo "Fire not present at $DEVICE" >&2
    echo "Attached instead:" >&2
    ls /dev/serial/by-id/ >&2 || echo "  (none)" >&2
    exit 1
fi

esphome="$(dirname "$0")/../work/venv/bin/esphome"
[[ -x $esphome ]] || esphome=esphome

extra=()
[[ $action == upload || $action == run ]] && extra=(--upload_speed 921600)

exec "$esphome" "$action" "$config" --device "$DEVICE" "${extra[@]}"
