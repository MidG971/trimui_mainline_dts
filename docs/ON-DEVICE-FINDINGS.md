<!-- SPDX-License-Identifier: (GPL-2.0-only OR MIT) -->
<!-- Copyright (C) 2026 Midgy BALON -->

# On-device findings — day 1 (2026-07-22)

The device is in hand. These are facts read from **live silicon** (adb on the stock
v1.0.2 OS: `dmesg`, `/proc`, `/sys`, `i2cdetect`) — they **override** the earlier
values transcribed from the vendor DTB/firmware where they differ. Raw captures are
local in `bringup/` (not committed): `live.dts`, the fresh-boot dmesg, the i2c scan.

Device: adb `<device-serial-redacted>`, kernel `Linux Longan 5.15.147 #82` (= firmware
v1.0.2), `model = sun55iw3`, `compatible = allwinner,a523 arm,sun55iw3p1`. Vendor
U-Boot 2018.07. DRAM **1200 MHz / 1 GB** (confirms our U-Boot retarget frequency).

## Corrections to earlier assumptions (silicon wins)

### 1. Big-cluster CPU regulator = **axp1530**, not tcs4838
The PMIC bus (`i2c-6` = r_i2c0 / s_twi0) scan shows **0x34 and 0x36 driver-bound; 0x41
absent**. Kernel probe logs: `axp2101-6-0034: AXP20x variant AXP2202 found` and
`pmu_ext_core-6-0036: variant AXP1530 found`. The board has three CPU-buck footprints
(tcs@41 / sy@60 / axp1530@36); **only axp1530@36 is populated**.
→ **DROP the `tcs4838` driver (patch 0014) + its board node; wire cpu4-7 to
`axp1530 dcdc1`** (already mainline: `x-powers,axp1530` in axp20x).

### 2. Analog sticks come from the gamepad MCUs, **not GPADC**
`trimui_inputd` (the input daemon) has open only: `/dev/uinput`, `/dev/ttyAS5`,
`/dev/ttyAS7` — **no GPADC fd**. The `TRIMUI Player1` js0 device
(`/devices/virtual/input/input4` = uinput) carries the whole pad: buttons, **both
sticks + L2/R2 analog** (`ABS=0x3003f` → X/Y/Z/RX/RY/RZ), D-pad hat, and rumble (`FF`).
→ **DROP the GPADC-joystick work** (patch 0012's 2nd GPADC controller +
`dts/staging/trimui-gpadc-joystick.dtsi` + the adc-joystick nodes). The SoC GPADC
registers but is not the stick source.

## Resolved unknowns

- **Gamepad source (the oldest open question):** two serial MCUs on **uart5 @0x2501400**
  and **uart7 @0x2501c00**, read by the userspace `trimui_inputd` daemon and published
  as `js0 "TRIMUI Player1"` via **uinput**. Not gpio-keys, not USB-HID. The whole pad —
  D-pad, ABXY, L/R + L2/R2 (analog), L3/R3, **both sticks**, and **rumble** — flows
  through those two UARTs.
  **Mainline path:** enable uart5 + uart7 in DT, then either (a) run the vendor
  `trimui_inputd` on our rootfs as an interim (it needs only the 2 UARTs + uinput, both
  standard), or (b) write a proper **serdev input driver** (reverse-engineer the MCU
  serial protocol).
- **PMIC = AXP2202 @0x34** (main) — settles the long axp717-vs-axp2202 ambiguity.
- **WiFi/BT = AIC8800D80** (`aicbsp_sdio_probe: matched chip: aic8800d80`) — the "D80 or
  DC" is **D80**; our out-of-tree module already targets it.
- **DSI panel model = `er68576`** (`TRIMUI get lcd model:er68576`; cmdline `lcd=er68576`).
- **Partition map — eMMC = `mmcblk0`:** p1 boot-resource / p2 env / p3 boot / **p4 rootfs
  (root)** / p5 boot(backup) / p6 UDISK. **microSD = `mmcblk1`** (~50 GB).
- **UART map:** ttyAS0=uart0@2500000 (console), ttyAS1=uart1@2500400 (BT),
  ttyAS5=uart5@2501400, ttyAS7=uart7@2501c00 (gamepad MCUs). earlycon `uart8250,mmio32,0x02500000`.
- Other inputs: LRADC side-keys (`sunxi-keyboard`, lradc@2009800), vibrator, headphone
  jack-detect on the codec.

## Confirmations (assumptions that held)

**Regulator rails** (`/sys/class/regulator`, actual configured voltages):

| Rail | Voltage | Consumer (confirmed) |
| :--- | :--- | :--- |
| axp2202-dcdc1 | 0.92 V* | CPU **little** cluster (cpu0-3) |
| axp1530-dcdc1 | 0.92 V* | CPU **big** cluster (cpu4-7) |
| axp2202-dcdc2 | 0.92 V | **GPU / VE** (shared rail) |
| axp2202-dcdc3 | **1.10 V** | **DRAM** (confirms our 1.16→1.10 V fix) |
| axp2202-cldo4 | 3.3 V | panel power0 |
| axp2202-cldo1 | 1.8 V | panel power1 |
| axp2202-aldo4 | 1.8 V | codec avcc |
| axp2202-cpusldo | 0.9 V | CPU SoC LDO |
| usb1-vbus | 5 V (on) | USB3 host VBUS-out |

*idle; the CPU rails scale with DVFS.

- **Battery = 5000 mAh** (`charge_full_design=5000000`), live: Charging, 85 %, 4.03 V.
- **Thermal zones:** cpul / cpub / gpu / npu / ddr, all ~47 °C at idle. **npu temp ==
  gpu temp** — on-silicon proof of Chen-Yu's THS review point (the NPU "sensor" is just
  the GPU sensor value). Matches the upstream ths0/ths1 zone structure we rebased onto.
- **GPU ladder** 150–888 MHz (matches `trimui-gpu-opp.dtsi`); vendor uses the `mali_kbase`
  r42p0 blob — we use Panfrost.

## To reconcile / follow up

- **CPU OPP frequency points** differ slightly from our transcription — update
  `dts/staging/trimui-cpu-opp.dtsi` to the live vendor ladders:
  - little: 408 / 672 / 792 / 936 / **1032 / 1128** / 1224 / 1320 / 1416 MHz
  - big: 408 / 672 / 840 / 1008 / 1200 / 1344 / 1488 / 1584 / 1680 / 1800 / **1992 /
    2088 / 2160** MHz (the vendor **does** expose the turbo bins to 2160).
- **AXP2202 vs mainline:** mainline axp20x ships the **axp717** driver — confirm it
  covers axp2202 (the vendor calls it axp2202), or add the compatible.
- **eMMC backup** (Phase 2) before any writes.
- Panel `er68576` timings (we have the vendor DCS init blob).

## Series impact

**Applied (2026-07-22):**
- ✅ `tcs4838` → **axp1530**: board node replaced, cpu4-7 → `&reg_ext_dcdc1`, patch 0014
  + tcs4838 notes dropped. axp717 PMIC confirmed correct.
- ✅ **GPADC-joystick dropped**: board `&gpadc`/`&gpadc1` + adc-joystick nodes removed,
  patch 0012 dropped (sticks are on the gamepad MCUs, not GPADC).
- ✅ **CPU OPP reconciled** to the live vendor ladders (little 1008/1104 → 1032/1128).

**Pending (needs the datasheet mux + a build host — both unavailable this pass):**
- **uart5 + uart7** for the gamepad MCUs. Pins **confirmed on hardware**: uart5 = **PK17**,
  uart7 = **PK13** (single-pin / RX-only — the MCUs stream state to the SoC). Mainline has
  the `uart5`/`uart7` nodes (disabled) and is data-driven pinctrl, so the board just needs
  a `&uart5`/`&uart7` enable + a pinctrl group `pins = "PK17"/"PK13"; allwinner,pinmux = <N>`.
  **`<N>` (the Port K uart mux) is still unknown** — the pinctrl debugfs is absent on the
  vendor kernel and the CFG registers read ambiguous; get it from the a523 datasheet Port K
  mux table (or a correct devmem of the PK_CFG regs), then enable + build-verify. The daemon/
  serdev input path is the separate, larger task on top.
- **DTB build-verify** of all the above (axp1530 + GPADC drop + OPP) — the build host was
  offline this session; run `make CHECK_DTBS=y` + the board DTB build when it's back.
