<!-- SPDX-License-Identifier: (GPL-2.0-only OR MIT) -->
<!-- Copyright (C) 2026 Midgy BALON -->

# Trimui Smart Pro S — hardware bring-up runbook

Everything in the port is written and builds clean against Linux **v7.1**
(no-hardware phase complete). This is the step-by-step plan to run when the
device arrives, with the exact tools for each step.

**Golden rules**
- Do all of **Phase 1–2 (recon + backup) before writing anything to the device.**
- Boot our kernel from **microSD first**, never eMMC, until it's proven.
- First U-Boot boot is via **FEL (RAM-only)** — it touches no storage, so it can't brick.
- Keep the stock firmware (`trimui_tg5050_20251218_v1.0.1`) — it's the recovery path.

---

## 0. Tools & materials checklist

### Physical
| Item | Why | Notes |
|---|---|---|
| USB-C **data** cable | adb + FEL + fastboot over USB | not a charge-only cable |
| USB-to-TTL **3.3 V** serial adapter | kernel/U-Boot console (ttyS0) | **3.3 V, not 5 V** — UART0 = PB9 TX / PB10 RX, 115200 8N1 |
| microSD card (≥8 GB) | boot our kernel without touching eMMC | |
| Small tools / tweezers | open the case to reach UART pads; FEL button | find pads from a teardown photo |

### Host tools (this PC — already installed unless noted)
| Tool | Use | Status |
|---|---|---|
| `sunxi-fel` (sunxi-tools) | FEL recovery / RAM-boot U-Boot | ✅ installed, A523-capable |
| `adb`, `fastboot`, `lsusb` | shell on stock OS, fastboot, USB IDs | ✅ |
| `dtc`, `dd` | DT (de)compile, eMMC imaging | ✅ |
| serial terminal: `picocom`/`minicom`/`screen` | console | install: `sudo apt install picocom` |
| our U-Boot FEL image | `uboot-a523/u-boot-sunxi-with-spl-trimui.bin` | ✅ built |
| `recon.sh` | read-only day-1 collector | ✅ in repo root |
| build host `compiler-rock3b` | kernel build (`kernel/build-trimui-kernel.sh`) | ✅ |

### On-device (stock OS, pushed via adb if missing)
- `i2c-tools` (`i2cdetect`/`i2cget`) — to read the PMIC + regulator voltages. If the
  stock OS lacks it, push a static aarch64 `i2cget`/`i2cdetect` (or read via
  `/sys/class/regulator/*/`).
- `evtest` — to identify input devices (gamepad MCU, LRADC keys).

---

## Phase 1 — Recon on the stock OS (READ-ONLY, zero risk)

Plug in USB-C, power on stock OS (adbd autostarts).

```bash
adb devices                       # confirm the device shows up
adb push recon.sh /tmp/ && adb shell sh /tmp/recon.sh | tee recon-$(date +%F).log
adb pull /sys/firmware/fdt live.dtb 2>/dev/null || \
  adb shell 'cat /sys/firmware/fdt' > live.dtb     # the running DTB
dtc -I dtb -O dts live.dtb -o live.dts             # decompile for reference
```

Then capture the HW-gated unknowns (commands run inside `adb shell`):
```bash
# PMIC: is it axp717 or axp2202? r_i2c0 = bus on PL0/PL1 (find its i2c-N number)
i2cdetect -l                                   # locate the r_i2c0 controller
i2cget -y <N> 0x34 0x03                         # chip-ID reg @0x34 (axp2202 bus)
# Regulator voltages we VERIFY (panel cldo1/cldo4, DRAM dcdc3, wifi aldo3/bldo1/2)
for r in /sys/class/regulator/*; do \
  echo "$(cat $r/name 2>/dev/null) $(cat $r/microvolts 2>/dev/null)"; done | sort -u
# Populated external CPU regulator (tcs4838@0x41 vs alts @0x36/0x60)
i2cdetect -y -r <N>
# WiFi/BT chip variant (AIC8800 D80 vs DC)
lsmod | grep -i aic ; ls /lib/firmware/ | grep -i aic
# Input: gamepad MCU + LRADC keys
cat /proc/bus/input/devices ; lsusb
# Storage partition map (so we don't clobber the wrong thing)
cat /proc/partitions ; ls -l /dev/block/by-name/ 2>/dev/null
dmesg | grep -iE 'dsi|tcon|disp|panel|axp|aic|mmc|pwm'   # vendor driver hints
```
Save all output. These map directly to files to fix — see the table at the end.

---

## Phase 2 — Back up the eMMC (BEFORE writing anything)

Brick insurance. Over adb (root) or from FEL/U-Boot later.
```bash
# whole eMMC (size from /proc/partitions; mmcblk0 is usually eMMC, mmcblk1 the SD)
adb shell 'dd if=/dev/block/mmcblk0 bs=8M' > emmc-full-backup.img
# (or per-partition: boot/env/rootfs from /dev/block/by-name/)
sha256sum emmc-full-backup.img > emmc-full-backup.img.sha256
```
Keep this image and the stock firmware archive safe.

---

## Phase 3 — Brick-safe first boot via FEL (RAM-only, touches nothing)

This validates the U-Boot + the **DRAM retarget** without writing storage.

1. Connect the serial adapter (PB9/PB10, GND), open the console:
   ```bash
   picocom -b 115200 /dev/ttyUSB0
   ```
2. Enter FEL mode (try, in order): hold a button combo at power-on (A523 FEL combo —
   check XDA A523 FEL howto), **or** from stock: `adb reboot efex` / `adb reboot fel`.
3. Confirm + RAM-boot our U-Boot:
   ```bash
   sunxi-fel version                                  # must report A523 / sun55iw3
   sunxi-fel -v uboot uboot-a523/u-boot-sunxi-with-spl-trimui.bin
   ```
4. Watch the serial console:
   - **DRAM trains + U-Boot prompt** → success, go to Phase 4.
   - **DRAM init hangs/errors** → tweak the 5 board params (`tpr2/6/10/11/12`) in
     `uboot/trimui-tg5050_defconfig`, rebuild U-Boot, retry. (See `uboot/DRAM-PARAMS.md`.)

---

## Phase 4 — Boot the mainline kernel from microSD (not eMMC)

1. **Build** (on `compiler-rock3b`):
   ```bash
   git clone --depth 1 -b v7.1 \
     https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git linux-trimui
   ./kernel/build-trimui-kernel.sh /root/trimui-display/linux-trimui /path/to/repo
   # → arch/arm64/boot/Image  +  …/allwinner/sun55i-a523-trimui-smart-pro-s.dtb  + modules
   ```
2. **Make a boot microSD**: partition (FAT boot + ext4 rootfs), put `Image` + the
   `.dtb` + a small rootfs (a distro arm64 rootfs or buildroot/initramfs). Use a
   U-Boot `boot.scr`/extlinux that loads `Image` + the dtb and sets
   `console=ttyS0,115200 root=/dev/mmcblk0p2 rw`.
3. Boot from SD (in U-Boot: `load mmc 0:1 ...; booti ...`), watch the console.
4. **Verify, in order** (each is already wired in our DTS/drivers):
   - Console to a shell over ttyS0.
   - Storage: `lsblk` / `dmesg | grep mmc` (SD = mmc0, eMMC = mmc2).
   - PMIC: `dmesg | grep axp`, `ls /sys/class/regulator/`.
   - USB: `lsusb` on the OTG/host ports.
   - WiFi: needs the **AIC8800 out-of-tree module** (build separately) + the PM
     control GPIOs already in the DTS.

---

## Phase 5 — Resolve the HW-gated unknowns → update the tree

From Phase 1 output, fix and rebuild:
| Unknown (from recon) | File to update |
|---|---|
| PMIC = axp717 **or** axp2202 @0x34 | `dts/…-smart-pro-s.dts` `pmic@34` compatible |
| Real panel rails cldo1 / cldo4 voltages | panel `power0/1-supply` constraints + check vs MMC vqmmc share |
| `vdd-dram` exact (we set 1.10 V) | `reg_dcdc3` + `uboot/…_defconfig` `CONFIG_AXP_DCDC3_VOLT` |
| Populated CPU regulator (0x41 tcs4838?) | `cpu-supply` map (cluster1) |
| AIC8800 variant (D80/DC) | WiFi firmware dir + module build |
| Gamepad input source | add the right input node/driver (not gpio-keys) |
| eMMC/SD partition map | boot config + any flashing |

Rebuild and re-test after each change.

---

## Phase 6 — Display bring-up (the last DT + a lit pixel)

Everything below the DE is written + building; remaining = the **DE3.5 DT** and
HW tuning. See `kernel/DE35-NOTES.md`.

1. **Finish the DE DT** (the pioneering bit): add the `display-engine` aggregator +
   `bus@5000000` + `display_clocks` (reuse `allwinner,sun50i-h616-de33-clk`) + the
   `mixer@100000` node (3 regs `layers`/`top`/`display` = `0x05100000`/`0x05000000`/
   `0x05280000`), give `de@5000000` its compatible, and add
   `allwinner,sun55i-a523-display-engine` to `sun4i_drv.c`. The live DTB from Phase 1
   resolves the open unknowns (the DE-clk reg offset / SRAM, the bus compatible).
2. Boot; check the pipeline binds as one DRM device:
   ```bash
   dmesg | grep -iE 'sun4i-drm|sun8i-mixer|sun6i-mipi-dsi|combo|tcon|panel'
   ls /sys/class/drm/                      # expect card0 + connector DSI-1
   ```
3. Push a test pattern through the TCON/DSI:
   ```bash
   modetest -M sun4i-drm                    # list connectors/modes (libdrm-tests)
   modetest -M sun4i-drm -s <conn>@<crtc>:720x1280 -P <plane>@<crtc>:...   # solid colour
   ```
4. Panel + backlight: confirm `panel-trimui-smart-pro-s` probes, the init blob runs,
   `/sys/class/backlight/*` controls brightness. Tune as needed:
   - PHY: lane-rate band / analog trim (`phy-sun55i-dsi-combo.c`).
   - Mixer: blender `map[]`, `mod_rate` (`sun8i_mixer.c` cfg).
   - Panel: reset polarity (DT flag), init-sequence timing.

---

## Phase 7 — Input, audio, the rest

- **Input**: `evtest` → LRADC keys (vol/side, `lradc@2009800`), the gamepad
  (D-pad/ABXY — kernel source TBD from recon), AXP2202 power key.
  - **LRADC keys — confirm/calibrate (no multimeter):** the board keymap voltages
    (410 / 646 / 900 mV → Home / Vol+ / Vol−) are transcribed from the vendor DTB
    `key0/1/2`, so they should be right. **Confirm:** `evtest`, pick the LRADC device,
    press each of the 3 keys, check the right code fires (`KEY_HOMEPAGE` / `KEY_VOLUMEUP`
    / `KEY_VOLUMEDOWN`). If all fire → drop the `VERIFY` tags, done.
    **If a key doesn't fire / maps wrong** (mainline's r329-LRADC scale ≠ vendor 1350 mV
    ref): read the *measured* voltage in software — add a temporary
    `dev_info(dev, "lradc voltage=%u\n", voltage);` in the IRQ handler of
    `drivers/input/keyboard/sun4i-lradc-keys.c`, rebuild that module, press each button,
    read `dmesg`, and put those µV in the DT. If all three are off by the same ratio, fix
    `vref-supply` (currently `&reg_bldo2` / 1.8 V, tagged VERIFY) instead. (Alternatively
    `devmem2` the LRADC data register at `0x2009800`+offset while holding a key.)
- **Analog sticks**: GPADC (`adc-joystick`) — UM §8.4.
- **RGB LEDs**: already wired — `&ledc` enabled in the board DTS with 17 RGB LEDs
  (the LEDC node + driver landed in mainline v7.1; `allwinner,sun55i-a523-ledc`). Check
  `/sys/class/leds/` and test a channel; if colours are swapped, set
  `allwinner,pixel-format` (default grb). Confirm the count (vendor said 17).
- **Thermal + fan**: adopt M. Kalashnikov's A523 THS series (linux-sunxi, msgid
  `20250411003827…` / respin `20260504050245…`; not yet mainline) — it adds the THS0/1
  driver (`sun8i_thermal.c`, needs a "gpadc" clock + shared reset), a `sid@` node, and
  `ths@`/`thermal-zones` to `sun55i-a523.dtsi` + the binding. It's all SoC-level — apply
  his series (via `b4 am`), don't re-author. **Our board add-on** = the Trimui fan: a
  `pwm-fan` on **pwm0 ch10 / pin PB6** (vendor: 40000 ns period, **inverted**, 32 levels)
  plus a `cooling-map` in his `cpu` thermal-zone, so the THS sensors auto-throttle cpufreq
  and drive the fan. (Fan params from the vendor DTB; wire once his zones land + a build
  host is up.)
- **Audio**: the A523 codec (UM §4.1) — separate driver effort.
- **cpufreq/OPP, GPU** — Mali-G57 (Valhall JM) via **Panfrost** (NOT Panthor), already
  fully upstream (node+binding+driver). Board work = `&gpu { mali-supply = <…>; status =
  "okay"; }` once the GPU rail is confirmed on HW (vendor `mali-supply` phandle 0x20). See
  `GPU-NOTES.md`.

---

## Quick reference — what's already done (so you don't redo it)
DSI host, combo-D-PHY, TCON-LCD, PWM, panel driver, SoC display DT nodes, the board
DTS, U-Boot (DRAM-retargeted, FEL-bootable), and the DE3.5 mixer cfg are all written
and build clean on v7.1. Patches: `kernel/patches/0001–0008`; drivers:
`kernel/drivers/`. The single remaining code/DT task is the DE DT assembly (Phase 6.1),
best finished against the device's live DTB.
