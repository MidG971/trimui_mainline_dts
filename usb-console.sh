#!/bin/sh
# SPDX-License-Identifier: (GPL-2.0-only OR MIT)
# Copyright (C) 2026 Midgy BALON
#
# usb-console.sh — read the mainline kernel boot console over the usb/dp port's
# USB **gadget** serial (CDC-ACM), so you get the boot log WITHOUT the physical
# ttyS0 UART pads (no teardown / soldering).
#
# Requires the bring-up kernel built with the gadget serial console
# (kernel/usb-gadget-console.config) + `console=ttyGS0,115200` on the cmdline.
# The device then enumerates on THIS host as /dev/ttyACM0 (CDC-ACM) or
# /dev/ttyUSB0 (generic g_serial). Run this on the HOST, then (re)boot the device
# — it waits for the port to appear and opens it with a timestamped logfile.
#
# CAVEAT: the gadget console only exists from USB-gadget init onward — it does
# NOT capture SPL / U-Boot proper or the earliest pre-USB kernel lines. For the
# FEL DRAM-training step (before any USB gadget), validate DRAM serial-free with
# `sunxi-fel` memory read/write instead (see docs/HARDWARE-BRINGUP.md).
#
# Usage: sh usb-console.sh [-b BAUD] [-d /dev/ttyACMx] [-o LOGFILE] [-t SECONDS]
#   -b BAUD    baud rate (default 115200)
#   -d DEV     force a device instead of auto-detecting
#   -o FILE    log file (default usb-console-<date>.log)
#   -t SECS    give up waiting after SECS (default: wait forever)

set -u

BAUD=115200
DEV=""
LOG="usb-console-$(date +%F-%H%M%S).log"
TIMEOUT=0

while [ $# -gt 0 ]; do
	case "$1" in
		-b) BAUD=$2; shift 2 ;;
		-d) DEV=$2; shift 2 ;;
		-o) LOG=$2; shift 2 ;;
		-t) TIMEOUT=$2; shift 2 ;;
		-h|--help) sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
		*) echo "unknown option: $1 (see -h)" >&2; exit 2 ;;
	esac
done

# Auto-detect the gadget-serial device the board presents when it enumerates.
find_dev() {
	for d in /dev/ttyACM* /dev/ttyUSB*; do
		[ -e "$d" ] && { echo "$d"; return 0; }
	done
	return 1
}

if [ -z "$DEV" ]; then
	echo "Waiting for the device's USB gadget-serial to appear"
	echo "(/dev/ttyACM* or /dev/ttyUSB*) — (re)boot the Trimui now via the usb/dp port..."
	waited=0
	while ! DEV=$(find_dev); do
		sleep 1
		waited=$((waited + 1))
		if [ "$TIMEOUT" -gt 0 ] && [ "$waited" -ge "$TIMEOUT" ]; then
			echo "Timed out after ${TIMEOUT}s — nothing enumerated." >&2
			echo "Check: kernel has usb-gadget-console.config + console=ttyGS0 cmdline;" >&2
			echo "the usb/dp port is in peripheral/gadget mode; the cable is a DATA cable." >&2
			exit 1
		fi
	done
fi

[ -e "$DEV" ] || { echo "No such device: $DEV" >&2; exit 1; }
echo "Console device: $DEV @ ${BAUD} 8N1   ->  logging to $LOG"
echo "(Ctrl-A Ctrl-X to quit picocom; Ctrl-A k for screen.)"

# Open with the best available terminal, always tee'ing to the logfile.
if command -v picocom >/dev/null 2>&1; then
	exec picocom -b "$BAUD" --imap lfcrlf --logfile "$LOG" "$DEV"
elif command -v screen >/dev/null 2>&1; then
	exec screen -L -Logfile "$LOG" "$DEV" "$BAUD"
elif command -v cu >/dev/null 2>&1; then
	echo "(cu doesn't log itself; capturing via tee)"
	cu -l "$DEV" -s "$BAUD" | tee "$LOG"
else
	# last resort: raw read. Set the line discipline first if stty is present.
	command -v stty >/dev/null 2>&1 && stty -F "$DEV" "$BAUD" cs8 -cstopb -parenb raw -echo 2>/dev/null
	echo "(no picocom/screen/cu — raw cat; install picocom for a proper terminal)"
	cat "$DEV" | tee "$LOG"
fi
