# Firmware mining results — pre-arrival (2026-06-04)

Source: stock firmware `trimui_tg5050_20251218_v1.0.1` (`IMAGEWTY` Allwinner image).
The rootfs is stored **uncompressed/plaintext** inside the `.awimg`, so facts below
were string-mined directly (no device needed). Cross-checked against the decompiled
vendor DTS `vendor/trimui_smart_pro_source.dts`. These resolve most of
PORTING-NOTES.md §3 — but i2c chip IDs still need on-device confirmation.

## Resolved

| Question | Answer | Evidence |
|---|---|---|
| **WiFi/BT chip** | **AICSemi AIC8800** (D80 or DC variant) — SDIO WiFi on mmc1 + UART BT | rootfs `modprobe aic8800_bsp.ko / aic8800_fdrv.ko / aic8800_btlpm.ko`; firmware dirs `aic8800d80/`, `aic8800dc/`. The 545 "BCM4"/"Broadcom" hits were just udev `hwdb` (`ID_MODEL_FROM_DATABASE=…`) — a red herring. |
| **AIC8800 mainline** | **Not in mainline.** Out-of-tree module, actively maintained (Radxa pkg V5.0, Jan 2026, builds on 6.19). Plan = build external module against our kernel. | Web: github radxa-pkg/aic8800, shenmintao/aic8800d80 |
| **Battery** | **5000 mAh** design, 1000 mA charge current, RDC ≈147 | vendor DTS `pmu_battery_cap=<0x1388>`, `pmu_runtime_chgcur=<0x3e8>`, `pmu_battery_rdc=<0x93>` |
| **Power key** | AXP2202 **PEK** (PMIC power-key child) | vendor DTS `pmu_powkey_*` |
| **Volume / side keys** | **LRADC** `lradc@2009800`, `allwinner,keyboard_1350mv`, **3 keys** | vendor DTS line ~5234. Mainline `sun4i-lradc-keys` may bind if A523 LRADC is compatible. |
| **Main gamepad (D-pad/ABXY/L/R)** | Handled by userspace **`trimui_inputd`** daemon (turbo-fire, remap, mouse-emu, USB-HID-gadget passthrough). Underlying kernel source still TBD on HW. | rootfs `/tmp/trimui_inputd/turbo_*`, `pad_button_get_mode_group` |
| **Shell access (day 1)** | **`adb shell` over USB-C** — adbd autostarts. Serial console on **ttyS0** as fallback. | rootfs `# start adbd daemon`, `adbd &`; `console=ttyS0` |
| **PMIC driver intent** | Vendor drives the PMIC as **axp2202** (282 hits vs 3 for axp717c). Silk = AXP717C. | rootfs strings; still must read i2c ID reg on HW |

## Still must confirm on hardware (recon.sh covers all of these)
- PMIC i2c **ID register** at 0x34 on r_i2c0 → axp2202 vs axp717 (decides mainline driver/binding).
- Which **CPU regulator** is populated: 0x36 axp1530 / 0x41 tcs4838 / 0x60 sy8827g (whichever ACKs).
- AIC8800 **exact variant** (D80 vs DC) from the firmware that actually loads.
- The **main-gamepad** kernel source: gpio-keys? an i2c/uart MCU? hidraw? (`cat /proc/bus/input/devices`).
- eMMC/SD **partition layout** for planning a safe full backup before flashing.

## Methodology — how these facts were obtained (no device required)

All of the above came from the stock firmware package alone, before the console
shipped. The process:

1. **Identified the container format.** The stock `.awimg` carries magic `IMAGEWTY`
   (Allwinner LiveSuit image). binwalk's auto-carving produced mostly false positives
   (`data`, "FoxPro FPT", stray python `setuptools/` dirs), so it was discarded.
2. **Key realization: the rootfs is stored uncompressed.** A `strings` scan of the
   raw image returned live shell scripts, `/lib/modules/...` paths and init logic —
   i.e. the Linux rootfs sits in the image as plaintext. That means **no unpacking,
   loop-mount, or root was needed** — every userland fact is greppable directly.
3. **One-pass string index.** `strings -n 5 trimui_tg5050.awimg > /tmp/aw_strings.txt`
   (3.75M lines), then repeated targeted greps against that index for signature tokens:
   PMIC part numbers, WiFi/BT chip names, `modprobe`/`insmod` lines, init/`adbd`/getty,
   the input daemon, and battery parameters.
4. **Separated proof from noise.** WiFi looked ambiguous at first (hundreds of "BCM4/
   Broadcom" hits). The disambiguator was *driver-load evidence*: the rootfs actually
   runs `modprobe aic8800_fdrv.ko` and ships `aic8800d80/`+`aic8800dc/` firmware, while
   every "Broadcom" hit traced to the udev hardware database
   (`ID_MODEL_FROM_DATABASE=BCM4350…`). Conclusion: AIC8800, the BCM strings are inert hwdb.
5. **Cross-checked against the authoritative DTS.** Each hardware claim was confirmed
   against the decompiled vendor `vendor/trimui_smart_pro_source.dts` (the real DTB):
   battery `pmu_battery_cap=0x1388` → 5000 mAh, `lradc@2009800 keyboard_1350mv` 3 keys,
   the `wlan` SDIO node + power rails, and the PMIC on `s_twi0` (r_i2c0) at 0x34.
6. **Confirmed mainline gaps via web.** Basic A523 support is in kernel v6.15;
   display/DSI are WIP; AIC8800 has no in-tree driver (out-of-tree module, Radxa V5.0).
7. **Wrote `recon.sh`** to capture only what genuinely needs the powered device
   (i2c chip IDs, populated CPU regulator, AIC variant, gamepad kernel source, partition map).

Tooling present on the workstation for this: `binwalk`, `strings`, `dtc`/`fdtdump`,
`unsquashfs`, and `sunxi-fel` (the last doubles as the FEL brick-recovery net for day 1).

## Day-1 procedure
1. `adb push recon.sh /tmp/recon.sh && adb shell 'sh /tmp/recon.sh' | tee recon-out.txt`
2. `adb pull /tmp/live.dtb` (vendor ground-truth DTB for offline diffing)
3. **Back up the eMMC before flashing anything** (dd partitions over adb, or via FEL — `sunxi-fel` is installed here).
