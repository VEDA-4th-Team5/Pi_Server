#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
MODULE_NAME="parking_alert"
MODULE_SOURCE="${PROJECT_ROOT}/driver/parking_alert/${MODULE_NAME}.ko"
MODULE_DEST="/lib/modules/$(uname -r)/extra/${MODULE_NAME}.ko"
MODULES_LOAD_FILE="/etc/modules-load.d/parking-alert.conf"
UDEV_RULE_SOURCE="${PROJECT_ROOT}/driver/parking_alert/99-parking-alert.rules"
UDEV_RULE_DEST="/etc/udev/rules.d/99-parking-alert.rules"

if [[ "${EUID}" -ne 0 ]]; then
    exec sudo -- "$0" "$@"
fi

make -C "${PROJECT_ROOT}/driver/parking_alert"
install -D -m 0644 "${MODULE_SOURCE}" "${MODULE_DEST}"
printf '%s\n' "${MODULE_NAME}" > "${MODULES_LOAD_FILE}"
install -m 0644 "${UDEV_RULE_SOURCE}" "${UDEV_RULE_DEST}"

depmod -a
udevadm control --reload-rules
modprobe "${MODULE_NAME}"
udevadm trigger --name-match=parking_alert || true

if [[ ! -c /dev/parking_alert ]]; then
    echo "parking_alert module loaded but /dev/parking_alert was not created" >&2
    exit 1
fi

echo "parking_alert driver installed and loaded: /dev/parking_alert"
