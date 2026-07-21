#!/bin/bash
# SPDX-License-Identifier: (GPL-2.0-only OR MIT)
# Copyright (C) 2026 Midgy BALON
#
# Assemble + build the mainline Trimui Smart Pro S kernel (Allwinner A523).
# Applies our patch series + drops in the out-of-tree-style driver sources, then
# builds Image + dtbs + modules. Run on the build host (compiler-rock3b).
#
# Usage:
#   ./build-trimui-kernel.sh <kernel-src-dir> [repo-dir]
# Example:
#   ./build-trimui-kernel.sh /root/trimui-display/linux-rc /root/trimui_mainline_dts
#
# Assumes a clean-ish v7.1+ tree. Re-running is safe (patches/copies are
# idempotent: it skips a patch that's already applied).
set -euo pipefail

KSRC="${1:?usage: build-trimui-kernel.sh <kernel-src-dir> [repo-dir]}"
REPO="${2:-$(cd "$(dirname "$0")/.." && pwd)}"
export ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
JOBS="$(nproc)"

P="$REPO/kernel/patches"
D="$REPO/kernel/drivers"
DTS="$KSRC/arch/arm64/boot/dts/allwinner"

echo "== kernel: $KSRC =="
head -3 "$KSRC/Makefile" | tr '\n' ' '; echo

echo "== drop in driver sources =="
cp -v "$D/phy-sun55i-dsi-combo.c"        "$KSRC/drivers/phy/allwinner/"
cp -v "$D/pwm-sun20i.c"                  "$KSRC/drivers/pwm/"
cp -v "$D/panel-trimui-smart-pro-s.c"    "$KSRC/drivers/gpu/drm/panel/"
# NB: the audio codec is NOT a dropped-in driver anymore — it is Chen-Yu Tsai's
# A523 variant patched into the existing sun4i-codec.c (patches 0026-0031).

echo "== apply patch series (0001..NNNN) =="
for p in "$P"/0*.patch; do
	if patch -d "$KSRC" -p1 --forward --dry-run <"$p" >/dev/null 2>&1; then
		patch -d "$KSRC" -p1 --forward <"$p"
		echo "  applied $(basename "$p")"
	else
		echo "  skip (already applied?) $(basename "$p")"
	fi
done

echo "== board DTS + panel fragment =="
cp -v "$REPO/dts/sun55i-a523-trimui-smart-pro-s.dts" "$DTS/"
cp -v "$REPO/dts/trimui-panel.dtsi"                  "$DTS/"
# match the actual #include directive, not the header-comment mention of the file
grep -q '#include "trimui-panel.dtsi"' "$DTS/sun55i-a523-trimui-smart-pro-s.dts" \
	|| printf '\n#include "trimui-panel.dtsi"\n' >>"$DTS/sun55i-a523-trimui-smart-pro-s.dts"
grep -q 'sun55i-a523-trimui-smart-pro-s.dtb' "$DTS/Makefile" \
	|| echo 'dtb-$(CONFIG_ARCH_SUNXI) += sun55i-a523-trimui-smart-pro-s.dtb' >>"$DTS/Makefile"

echo "== config =="
cd "$KSRC"
[ -f .config ] || make defconfig
# Board drivers + handheld tuning, from the versioned fragment (validated with
# merge_config). AIC8800 (WiFi/BT) is an out-of-tree module, built separately.
./scripts/kconfig/merge_config.sh -m .config "$REPO/kernel/trimui.config"
make olddefconfig

echo "== build: Image + dtbs + modules =="
make -j"$JOBS" Image dtbs modules

echo
echo "== artifacts =="
ls -la arch/arm64/boot/Image \
       "$DTS/sun55i-a523-trimui-smart-pro-s.dtb" 2>/dev/null
echo "modules: $(find . -name '*.ko' -newer .config 2>/dev/null | grep -cE 'sun4i|sun6i|sun55i|sun20i|trimui|combo' || true) display/pwm/panel .ko built"
echo "Done. Image = arch/arm64/boot/Image ; DTB = $DTS/sun55i-a523-trimui-smart-pro-s.dtb"
