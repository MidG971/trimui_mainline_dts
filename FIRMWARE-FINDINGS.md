<!-- SPDX-License-Identifier: (GPL-2.0-only OR MIT) -->
<!-- Copyright (C) 2026 Midgy BALON -->

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

# Firmware v1.0.2 re-mine (2026-07-04) — diff vs v1.0.1

Re-mined the newer stock build (`trimui_tg5050.awimg`, build `20260201_2150`; **beta**).
Method: parse the `IMAGEWTY` file table (1024-byte entries @0x400; name at entry+36,
`stored_len`/`orig_len`/`offset` at entry+0x124), then SHA-256 every partition and
diff the two firmwares. Only **two** partitions changed:

| Partition group | v1.0.1 → v1.0.2 | Note |
|---|---|---|
| boot0 / SPL / u-boot / boot_package / fes1 / toc\* / **`sunxi.fex` (vendor DTB)** / arisc / env | **byte-identical** | boot chain + device tree unchanged → U-Boot DRAM retarget and every DTB-derived fact still current (vendor DTB md5 `967f1c68…`) |
| kernel (`vmlinux.fex` / `boot.fex`) | changed | same source **Linux 5.15.147**, rebuilt `#42` (Dec 18 2025) → `#82` (Feb 2 2026); embedded DTB identical. Among `/lib/modules` only `mali_kbase.ko` changed (vendor GPU blob — irrelevant to our Panfrost path) |
| `rootfs.fex` (+18 MB) | changed | mostly UI/audio (bgm.mp3 +8 MB, 3 new mp3s, MainUI, lang files) + a new **"performance" power mode**. `/lib/firmware` byte-identical → AIC8800 blobs unchanged |

**New authoritative data (usable now):**

- **CPU DVFS ladders** (vendor `cluster{0,1}-opp-table`, production bin vf0100 — current
  since the DTB is byte-identical). Little cluster (cpu0–3, `reg_dcdc1`): 408 → **1416 MHz**,
  0.90 → 1.15 V. Big cluster (cpu4–7, `tcs4838`): 408 → **1800 MHz** nominal (0.90 → 1.15 V),
  turbo bins 1992 @1.22 V / 2088 @1.24 V / **2160 @1.28 V**. Two cpufreq domains confirmed on
  the stock OS (`cpufreq/policy0` = little, `policy4` = big). → filled into
  `dts/staging/trimui-cpu-opp.dtsi`.
- **GPU DVFS ladder** 150 / 200 / 300 / 400 / 600 / 648 / 696 / 744 / 840 / **888 MHz** (vendor
  `gpu-opp-table`; the draft previously stopped at 600). → filled into `dts/staging/trimui-gpu-opp.dtsi`.
- **Vendor DVFS knobs** (for reference): per-cluster `cpufreq/policy{0,4}/force_{min,max}_freq`
  and `/sys/class/gpu_ctl/force_max_freq` (mali_kbase), reset to 0 (unclamped) at boot.
- **Thermal:** the vendor kernel exposes **≥6 thermal zones** (the OSD reads `thermal_zone5`).
- **Bluetooth attach** = `hciattach -n ttyAS1 aic` (vendor `ttyAS1` = mainline `ttyS1` → our `&uart1`).

**Net:** v1.0.2 is a userspace/UI refresh + kernel rebuild; it introduces **no** device-tree,
bootloader, or (non-Mali) driver changes, so nothing in the port needs re-work. It is a **beta**
— re-mine once v1.0.2 ships officially, in case the release build differs.
