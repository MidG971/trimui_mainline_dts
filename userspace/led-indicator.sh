#!/bin/sh
# SPDX-License-Identifier: (GPL-2.0-only OR MIT)
# Copyright (C) 2026 Midgy BALON
#
# RGB LED ring "device is on" status indicator for the TrimUI Smart Pro S.
#
# The 17-LED WS2812 ring is driven by the mainline sun50i-a100-ledc controller
# (leds_sun50i_a100), which exposes each LED at
#   /sys/class/leds/rgb:indicator-{0..16}
# as an led_class_multicolor device (multi_intensity = "R G B", brightness 0-255).
#
# Edit COLOR / BRIGHT to taste. Installed as a boot service via
# led-indicator.service.
#
# NOTE (hardware): on the current unit only the LEFT half of the ring lights.
# The lit LEDs show the correct colour, so the bit-timing/data path is fine --
# the dead half is a hardware issue (a mid-chain break, or the right-half LEDs
# on a segment/rail that is not enabled), not the driver. Writing all 17 here is
# harmless and future-proofs the script for when that is resolved.

COLOR="0 255 0"   # R G B intensity ratio (0-255 each) -- green
BRIGHT=64         # overall brightness 0-255 (low = battery-friendly)

for n in $(seq 0 16); do
	L="/sys/class/leds/rgb:indicator-$n"
	[ -e "$L/multi_intensity" ] && echo "$COLOR"  > "$L/multi_intensity" 2>/dev/null
	[ -e "$L/brightness" ]      && echo "$BRIGHT" > "$L/brightness"      2>/dev/null
done
