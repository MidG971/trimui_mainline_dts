#!/bin/bash
# SPDX-License-Identifier: (GPL-2.0-only OR MIT)
# Copyright (C) 2026 Midgy BALON
#
# Build the AIC8800 WiFi + Bluetooth kernel modules for the Trimui Smart Pro S
# against a mainline kernel tree (tested: v7.2-rc1). Fetches warpme/minimyth2's
# AIC8800 SDIO backport (pinned), applies our small v7.1->v7.2 delta, and builds
# the driver OUT-OF-TREE (make M=):
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

# Pinned AIC8800 source: warpme/minimyth2's in-tree SDIO backport, snapshot
# v2025_0926 (driver fdrv 6.4.3.0 — the version that ships in the Trimui stock
# firmware). warpme already carries the modern-kernel fixups; we add only the
# tiny v7.1->v7.2 delta below. Re-pin + re-test the delta if you bump this.
MM2_PIN="97b9429b90db1fca1fe3b93a112fb739b0c5452d"
RAW="https://raw.githubusercontent.com/warpme/minimyth2/${MM2_PIN}/script/kernel/linux-7.1/files"
BASE="3401-net-wireless-backport-aic8800-sdio-v2025_0926_91c9dae5-mm2.patch"
FIX="3401-net-wireless-backport-aic8800-sdio-v2025_0926_91c9dae5-mm2-fix-kernel7.1.patch"
SRC="drivers/net/wireless/aic8800_sdio"

mkdir -p "$WORK"; cd "$WORK"

echo "== fetching warpme AIC8800 patches (pinned ${MM2_PIN}) =="
for f in "$BASE" "$FIX"; do
	[ -s "$f" ] || curl -fsSL -o "$f" "$RAW/$f"
done

echo "== materialising driver source (out-of-tree) =="
# The patches create the aic8800_sdio/ tree (paths prefixed linux-*/, so -p1).
# Two hunks touch drivers/net/wireless/{Kconfig,Makefile}, which don't exist in
# this empty dir -> those hunks are rejected (harmless: we pass CONFIG_ on the
# make line). We assert the driver source appeared instead of trusting patch(1)'s
# exit code.
rm -rf src; mkdir -p src; cd src
patch -p1 -f --no-backup-if-mismatch <"../$BASE" >/dev/null 2>&1 || true
patch -p1 -f --no-backup-if-mismatch <"../$FIX"  >/dev/null 2>&1 || true
[ -f "$SRC/aic8800_fdrv/rwnx_main.c" ] || {
	echo "ERROR: driver source did not materialise from the warpme patches" >&2
	exit 1
}

echo "== applying Trimui v7.2 delta (aic8800-warpme-v7.2.patch) =="
patch -p1 <"$HERE/aic8800-warpme-v7.2.patch"

echo "== building modules (make M=) against $KSRC =="
make -C "$KSRC" -j"$(nproc)" ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" \
	M="$PWD/$SRC" \
	CONFIG_AIC_SDIO_WLAN_SUPPORT=y \
	CONFIG_AIC8800_WLAN_SUPPORT=m \
	CONFIG_AIC8800_BTLPM_SUPPORT=m \
	modules

echo
echo "== built modules =="
find "$PWD/$SRC" -name '*.ko' -printf '  %p\n'
cat <<'EOF'

Next (on the device):
  * copy the three .ko + the aic8800{d80,dc}/ firmware to the target
    (firmware -> /lib/firmware/, from your device's stock rootfs; not shipped here)
  * modprobe aic8800_bsp           # bus/base (auto-loaded as a dep)
  * modprobe aic8800_fdrv          # WiFi (SDIO, &mmc1)
  * modprobe aic8800_btlpm         # Bluetooth power/sleep
  * BT HCI: hciattach -n ttyS1 aic # via &uart1 (vendor ttyAS1 = mainline ttyS1)
See README.md.
EOF
