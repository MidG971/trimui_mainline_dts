#!/bin/bash
# SPDX-License-Identifier: (GPL-2.0-only OR MIT)
# Copyright (C) 2026 Midgy BALON
#
# Build the AIC8800 WiFi + Bluetooth kernel modules for the Trimui Smart Pro S
# against a mainline kernel tree (tested: v7.2-rc1). Fetches the AICSemi / Radxa
# BSP (SDIO tree, pinned), applies our v7.x port + a compat shim, and builds:
#   aic8800_bsp.ko, aic8800_fdrv.ko (WiFi), aic8800_btlpm.ko (Bluetooth).
#
# The AIC8800 has no mainline driver, so these are out-of-tree modules. The RF
# firmware and the userspace `hciattach ... aic` come from the device, not this
# repo (see README.md) — they are not redistributed here.
#
# Usage:
#   ./build-aic8800.sh <kernel-src-dir> [work-dir]
# Example:
#   ./build-aic8800.sh /path/to/linux-v7.2 /tmp/aic8800-build
#
# The kernel tree must be configured + built (or at least `modules_prepare`) with
# CONFIG_CFG80211 and CONFIG_BT enabled (build-trimui-kernel.sh gives you this).
set -euo pipefail

KSRC="$(cd "${1:?usage: build-aic8800.sh <kernel-src-dir> [work-dir]}" && pwd)"
WORK="${2:-$PWD/aic8800-build}"
HERE="$(cd "$(dirname "$0")" && pwd)"
export ARCH=arm64 CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"

# Pinned AIC8800 BSP: SDK V5.0 (2026-01-23), fdrv 6.4.3.0 — the version that ships
# in the Trimui stock firmware. Re-pin + re-test the port patch if you bump this.
REPO_URL="https://github.com/radxa-pkg/aic8800.git"
PIN="bd11969265809a0fc948f1107c8256bbb2c1aa60"
DRV="src/SDIO/driver_fw/driver/aic8800"

mkdir -p "$WORK"; cd "$WORK"
if [ ! -e aic8800/.git ]; then
	echo "== cloning AIC8800 BSP (radxa-pkg/aic8800) =="
	git clone "$REPO_URL" aic8800
fi
cd aic8800
git checkout -q "$PIN" 2>/dev/null || { git fetch --depth 200 origin && git checkout -q "$PIN"; }

echo "== applying Trimui v7.x port (aic8800-7.2.patch) =="
if git apply --reverse --check "$HERE/aic8800-7.2.patch" >/dev/null 2>&1; then
	echo "  (already applied)"
else
	git checkout -q -- .			# clean tree before (re)applying
	git apply "$HERE/aic8800-7.2.patch"
fi
cp "$HERE/aic_compat72.h" "$DRV/aic8800_fdrv/aic_compat72.h"

echo "== building modules against $KSRC =="
cd "$DRV"
make KDIR="$KSRC" ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" \
     KCFLAGS="-include $PWD/aic8800_fdrv/aic_compat72.h"

echo
echo "== built modules =="
find "$PWD" -name '*.ko' -printf '  %p\n'
cat <<'EOF'

Next (on the device):
  * copy the three .ko + the aic8800{d80,dc}/ firmware to the target
    (firmware -> /lib/firmware/, from your device's stock rootfs; not shipped here)
  * modprobe aic8800_bsp           # bus/base (auto-loaded as a dep)
  * modprobe aic8800_fdrv          # WiFi (SDIO, &mmc1)
  * modprobe aic8800_btlpm         # Bluetooth power/sleep
  * BT HCI: hciattach -n ttyS1 aic # via &uart1 (needs the vendor `aic` hciattach)
See README.md.
EOF
