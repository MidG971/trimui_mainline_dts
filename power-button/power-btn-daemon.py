#!/usr/bin/env python3
# TrimUI Smart Pro S power-button daemon (display-free gestures).
#
#   tap (< HOLD_MIN)   -> SLEEP (suspend s2idle); wake on next PEK press
#   hold (>= HOLD_MIN) -> POWER OFF (clean `systemctl poweroff`)
#   hold ~4s+          -> AXP hardware off (done in the PMIC; supersedes poweroff)
#
# No RESTART gesture: `systemctl reboot` hits the broken warm-reboot path on this
# board (it tears down then hangs, needing a manual power-cycle). Re-add a
# double-tap -> reboot when warm reboot is fixed.
#
# HOLD_MIN is deliberately high so a normal ~1-2s press SLEEPS and never powers
# off by accident; a clean poweroff takes a deliberate long hold (and holding to
# ~4s instead hits the AXP hardware cut-off). The on-screen power MENU was
# abandoned (the DSI panel won't reliably render a frame; see docs/DISPLAY-*).
#
# PEK found by NAME (input event numbers shift between boots). `systemctl
# suspend` hands control back around resume, so the next release after suspend
# (the press that woke the device) is hard-ignored.
import struct, time, subprocess, glob

PEK_NAME = "axp20x-pek"
EV_KEY, KEY_POWER = 1, 116
FMT = "llHHi"; SZ = struct.calcsize(FMT)
HOLD_MIN = 2.5   # held >= this -> clean poweroff; < this -> sleep

def log(m): print("power-btn: " + m, flush=True)

def find_pek():
    for np in glob.glob("/sys/class/input/event*/device/name"):
        try:
            if open(np).read().strip() == PEK_NAME:
                return "/dev/input/" + np.split("/")[4]
        except OSError:
            continue
    return None

def open_dev():
    while True:
        dev = find_pek()
        if dev:
            try:
                f = open(dev, "rb", buffering=0)
                log("watching %s (%s)" % (dev, PEK_NAME))
                return f
            except OSError as e:
                log("found %s but can't open (%s)" % (dev, e))
        else:
            log("PEK '%s' not found yet" % PEK_NAME)
        time.sleep(2)

def main():
    f = open_dev(); press_t = None; ignore_next_release = False
    log("started (tap=sleep, hold>=%.1fs=poweroff, ~4s=AXP hw-off)" % HOLD_MIN)
    while True:
        data = f.read(SZ)
        if not data or len(data) < SZ:
            continue
        _, _, etype, code, value = struct.unpack(FMT, data)
        if etype != EV_KEY or code != KEY_POWER:
            continue
        now = time.monotonic()
        if value == 1:
            press_t = now
        elif value == 0:
            if ignore_next_release:
                ignore_next_release = False; press_t = None
                log("ignored wake press"); continue
            if press_t is None:
                continue
            dur = now - press_t; press_t = None
            if dur >= HOLD_MIN:
                log("hold %.2fs -> poweroff" % dur)
                subprocess.run(["systemctl", "poweroff"])
            else:
                log("tap %.2fs -> suspend" % dur)
                ignore_next_release = True
                subprocess.run(["systemctl", "suspend"])
                log("resumed (wake press will be ignored)")

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
