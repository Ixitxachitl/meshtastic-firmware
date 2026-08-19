#!/usr/bin/env bash
# Build the SF32LB52x firmware. Everything is overridable for other machines:
#
#   SIFLI_WS    west workspace holding zephyr/ and modules/ (Zephyr v4.4.0)
#   WEST        the west executable
#   BOARD       Zephyr board name
#   BUILD_DIR   cmake build directory
#
# Flashing needs sftool over the board's CH343P USB-UART: `west flash -d <dir>`.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SIFLI_WS="${SIFLI_WS:-/workspaces/sifli-ws}"
WEST="${WEST:-/workspaces/zephyr-venv/bin/west}"
BOARD="${BOARD:-t_display_sf32}"
BUILD_DIR="${BUILD_DIR:-/tmp/mt-build}"

export ZEPHYR_BASE="${SIFLI_WS}/zephyr"

exec "${WEST}" build -b "${BOARD}" "${REPO_ROOT}/zephyr/sifli" -d "${BUILD_DIR}" \
	"$@" -- -DBOARD_ROOT="${REPO_ROOT}/zephyr/sifli"
