#!/bin/sh
# SPDX-License-Identifier: (GPL-2.0-only OR MIT)
# Copyright (C) 2026 Midgy BALON
# ============================================================================
# Trimui Smart Pro S (TG5050 / Allwinner A523) — Day-1 hardware recon
# ----------------------------------------------------------------------------
# Run on the STOCK firmware to capture the facts the mainline port needs.
# It is READ-ONLY: it does not flash, write, or modify anything.
#
# Get a shell first (adbd autostarts on the stock OS):
#     adb push recon.sh /tmp/recon.sh
#     adb shell 'sh /tmp/recon.sh' | tee recon-out.txt
# (or run over the serial console on ttyS0 and capture the log)
#
# Goal: resolve PORTING-NOTES.md §3 — PMIC chip ID, CPU regulator populated,
# AIC8800 variant, gamepad/LRADC input, battery, storage layout.
# ============================================================================

sec() { printf '\n\n========== %s ==========\n' "$1"; }
have() { command -v "$1" >/dev/null 2>&1; }
dump() { [ -e "$1" ] && { printf '--- %s ---\n' "$1"; cat "$1" 2>/dev/null; echo; }; }

sec "0. IDENTITY / KERNEL / CMDLINE"
uname -a
dump /proc/cmdline
dump /etc/os-release
[ -r /proc/version ] && cat /proc/version

sec "1. DEVICE TREE (vendor ground truth)"
echo "[model/compatible]"
dump /proc/device-tree/model
dump /proc/device-tree/compatible
echo "[copy the live FDT off the device for offline diffing:]"
if [ -r /sys/firmware/fdt ]; then
  cp /sys/firmware/fdt /tmp/live.dtb 2>/dev/null && echo "  -> saved /tmp/live.dtb (adb pull it)"
fi
echo "[device-tree top-level nodes]"; ls /proc/device-tree 2>/dev/null

sec "2. I2C BUSES  (PMIC lives at 0x34 on the r_i2c0 / s_twi0 bus)"
if have i2cdetect; then
  i2cdetect -l
  for b in $(i2cdetect -l 2>/dev/null | sed -n 's/^i2c-\([0-9]\+\).*/\1/p'); do
    echo "----- scan bus i2c-$b -----"
    i2cdetect -y -r "$b" 2>/dev/null
  done
else
  echo "i2cdetect not present. Buses:"; ls -d /dev/i2c-* 2>/dev/null
fi

sec "3. PMIC IDENTITY  (axp717 vs axp2202 — THE key ambiguity)"
echo "Board silk + Allwinner reference design say AXP717C, whose register map matches"
echo "mainline's x-powers,axp717 driver. AXP717C has NO chip-ID/version register, so we"
echo "identify by BEHAVIOUR: dump the whole PMIC and check the AXP717 map decodes sanely."
echo "If 0x83/84/85 (DCDC1/2/3 V-set) + regulator_summary (sec 9) match the known rails"
echo "(esp. DCDC3 ~= 1.10V DRAM) -> axp717-compatible. A dump that does NOT decode as"
echo "AXP717 => a genuinely different AXP2202 and mainline needs a new driver. Capture BOTH."
if have i2cget; then
  for b in $(i2cdetect -l 2>/dev/null | sed -n 's/^i2c-\([0-9]\+\).*/\1/p'); do
    i2cget -y "$b" 0x34 0x00 >/dev/null 2>&1 || continue
    echo "===== PMIC @ i2c-$b addr 0x34 ====="
    if have i2cdump; then
      echo "[full register dump 0x00-0xff — read-only]"; i2cdump -y "$b" 0x34 b 2>/dev/null
    fi
    echo "[key AXP717C registers]"
    for r in 0x00 0x01 0x03 0x0b 0x19 0x80 0x81 0x82 0x83 0x84 0x85 0x90 0x91 0xa4; do
      v=$(i2cget -y "$b" 0x34 "$r" 2>/dev/null)
      echo "  reg $r = $v"
    done
    echo "  expect (AXP717C): 0x83/84/85 = DCDC1/2/3 V-set; DCDC3 ~1.10V (DRAM);"
    echo "                    0x00/0x01 = PMU status; 0x03 is RESERVED (nonzero => suspect)."
  done
  echo "CPU big-cluster regulator (which addr ACKs = populated; expect 0x41=tcs4838):"
  for b in $(i2cdetect -l 2>/dev/null | sed -n 's/^i2c-\([0-9]\+\).*/\1/p'); do
    for a in 0x36 0x41 0x60; do
      i2cget -y "$b" "$a" 0x00 >/dev/null 2>&1 || continue
      echo "  bus $b addr $a ACK  (0x36=axp1530 0x41=tcs4838 0x60=sy8827g)"
      # tcs4838 is a FAN53555-family buck with NO public datasheet. Capturing its
      # registers here (read-only) pins down the exact die + voltage table so the
      # mainline fan53555.c variant can be finished. See docs/TCS4838-NOTES.md.
      if [ "$a" = 0x41 ] && have i2cdump; then
        echo "  [tcs4838@0x41 full register dump — read-only, for the mainline driver]"
        i2cdump -y "$b" "$a" b 2>/dev/null
        echo "  [fan53555-family decode] ID1(0x03)=$(i2cget -y "$b" "$a" 0x03 2>/dev/null)" \
             "ID2(0x04)=$(i2cget -y "$b" "$a" 0x04 2>/dev/null)" \
             "VSEL0(0x00)=$(i2cget -y "$b" "$a" 0x00 2>/dev/null)" \
             "VSEL1(0x01)=$(i2cget -y "$b" "$a" 0x01 2>/dev/null)" \
             "altVSEL(0x10/0x11)=$(i2cget -y "$b" "$a" 0x10 2>/dev/null)/$(i2cget -y "$b" "$a" 0x11 2>/dev/null)"
      fi
    done
  done
else
  echo "i2cget not present — install i2c-tools; note which addresses i2cdetect showed."
fi

sec "4. POWER SUPPLY / BATTERY  (confirm 5000mAh design, voltages, charger)"
for ps in /sys/class/power_supply/*; do dump "$ps/uevent"; done

sec "5. WIFI / BT  (expect AIC8800 — confirm D80 vs DC variant)"
echo "[loaded modules]"; lsmod 2>/dev/null | grep -iE 'aic|cfg80211|mac80211|btlpm|wifi'
echo "[aic firmware present]"; ls -1 /lib/firmware/aic8800* 2>/dev/null; ls -1 /lib/firmware/ 2>/dev/null | grep -i aic
echo "[mmc / sdio devices]"; ls -l /sys/bus/sdio/devices 2>/dev/null; \
  for d in /sys/bus/sdio/devices/*; do dump "$d/uevent"; done
echo "[dmesg aic/sdio/mmc]"; dmesg 2>/dev/null | grep -iE 'aic|sdio|mmc1|wlan|bluetooth|hci' | tail -40

sec "6. INPUT  (gamepad daemon + LRADC keys + AXP power key)"
echo "[input devices — look for the gamepad source: gpio-keys? lradc? hidraw? MCU?]"
dump /proc/bus/input/devices
echo "[/dev/input]"; ls -l /dev/input 2>/dev/null
echo "[is the gamepad a USB/serial MCU?]"; ls -l /dev/hidraw* /dev/ttyS* 2>/dev/null
echo "[trimui input daemon]"; ps 2>/dev/null | grep -iE 'trimui|input' | grep -v grep
echo "[dmesg input/lradc/key]"; dmesg 2>/dev/null | grep -iE 'lradc|gpio-key|input:|joystick|gamepad' | tail -30

sec "7. USB topology  (which port hosts what; OTG gadget config)"
have lsusb && lsusb
echo "[usb gadget functions in use]"; ls /sys/kernel/config/usb_gadget/ 2>/dev/null
dump /sys/kernel/debug/usb/devices

sec "7B. USB-C / DISPLAYPORT ALT MODE  (the 'usb/dp' port: PD + DP-out)"
echo "Vendor HW has a full DP-alt-mode chain: TCON3 -> drm_edp (allwinner,drm-dp, 4-lane)"
echo "-> husb311 TCPC (usb-c-connector altmode SVID 0xff01 = DisplayPort) + ps8743 USB3/DP"
echo "mux (twi5). Capture the live Type-C / DP state (DP-out rides on the display port)."
echo "[type-c ports / partners / alt modes (SVID ff01 = DisplayPort)]"
for t in /sys/class/typec/*; do
  dump "$t/uevent"
  for am in "$t"/*/svid "$t"/*/*/svid; do
    [ -e "$am" ] && { printf '  altmode %s = ' "$am"; cat "$am" 2>/dev/null; }
  done
done
ls -l /sys/class/typec 2>/dev/null
echo "[husb311 TCPC + ps8743 mux on i2c]"; ls -l /sys/bus/i2c/devices 2>/dev/null | grep -iE 'husb|ps87|tcpc'
echo "[DRM connectors — is a DP/eDP connector exposed?]"
for s in /sys/class/drm/*/status; do dump "$s"; done
echo "[dmesg: typec / tcpm / husb311 / ps8743 / dp / edp]"
dmesg 2>/dev/null | grep -iE 'husb311|ps8743|tcpm|typec|alt.?mode|displayport|[^a-z]dp[^a-z]|edp' | tail -40

sec "8. STORAGE  (eMMC + SD layout — needed to plan a safe full backup)"
dump /proc/partitions
echo "[mmc devices]"; ls -l /sys/class/mmc_host 2>/dev/null; \
  for m in /sys/block/mmcblk*; do printf '%s size: ' "$m"; cat "$m/size" 2>/dev/null; done
echo "[mounts]"; mount 2>/dev/null
echo "[partition names (GPT/Allwinner)]"; ls -l /dev/disk/by-name 2>/dev/null || ls -l /dev/mmcblk*p* 2>/dev/null

sec "9. CLOCKS / PMIC REGULATORS (runtime regulator tree)"
dump /sys/kernel/debug/regulator/regulator_summary
have cpufreq-info && cpufreq-info
for c in /sys/devices/system/cpu/cpufreq/policy*/scaling_available_frequencies; do dump "$c"; done

sec "10. FULL DMESG (tail) + loaded modules"
dmesg 2>/dev/null | tail -120
echo "[all modules]"; lsmod 2>/dev/null

echo
echo "########## DONE.  adb pull /tmp/live.dtb  and save this whole log. ##########"
