#!/bin/sh
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

sec "3. PMIC IDENTITY  (axp2202 vs axp717 — THE key ambiguity)"
echo "For each i2c bus that ACKed 0x34 above, read the IC-type register."
echo "AXP717 vs AXP2202 differ in their ID register value — record it."
if have i2cget; then
  for b in $(i2cdetect -l 2>/dev/null | sed -n 's/^i2c-\([0-9]\+\).*/\1/p'); do
    v=$(i2cget -y "$b" 0x34 0x03 2>/dev/null) && \
      echo "  bus $b  pmic@0x34 reg0x03 (IC type) = $v"
  done
  echo "Also probe CPU-regulator candidates (which one ACKs = populated):"
  for b in $(i2cdetect -l 2>/dev/null | sed -n 's/^i2c-\([0-9]\+\).*/\1/p'); do
    for a in 0x36 0x41 0x60; do
      i2cget -y "$b" "$a" 0x00 >/dev/null 2>&1 && \
        echo "  bus $b addr $a ACK  (0x36=axp1530 0x41=tcs4838 0x60=sy8827g)"
    done
  done
else
  echo "i2cget not present — note which addresses i2cdetect showed."
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
