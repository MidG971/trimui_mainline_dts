# Trimui Smart Pro S — Mainline Porting Notes

Status board: **A523-PRO2-AXP717C** · SoC **Allwinner A523 (sun55iw3p1)** · model TG5050.
Authoritative hardware reference: decompiled vendor DTB → `trimui_smart_pro_source.dts`
(model `sun55iw3`, board `A523\0A523-PRO2-AXP717C`, compatible `allwinner,a523 / arm,sun55iw3p1`).

---

## 0. Reality check — mainline A523 maturity (READ FIRST)

Mainline `sun55i-a523.dtsi` (Arm Ltd, 6.13+) currently exposes **only**:

| Present in mainline | Missing in mainline (no driver/node yet) |
|---|---|
| CPU cores (A55), PSCI, GIC | Display engine (DE3.5), TCON, **MIPI-DSI** |
| CCU / R-CCU clocks | **PWM** (backlight) |
| MMC0/1/2 | **GPADC** (analog sticks) |
| UART0-7 | **LRADC** |
| I2C0-5, **R-I2C0** | **Audio codec / I2S / DAI** |
| USB (otg, ehci0/1, ohci0/1, phy) | NPU / VE (video decode) / G2D |
| RTC, watchdog, NMI, r_pio | Mali GPU |

**Consequence:** screen, backlight, audio, analog sticks, video decode and GPU
cannot work on a current mainline tree yet — the SoC sub-drivers don't exist.
A realistic port is **phased**: get a serial console + storage + USB + PMIC + WiFi
first; display/audio/sticks/GPU come later as those SoC drivers are mainlined
(or written). Any DTS that references `&dsi0`, `&pwm`, `&gpadc`, `&lradc`,
`&codec`, `&de`, `&tcon0` will fail `dtc`/kbuild against mainline today.

---

## 1. Hardware truth table (from vendor DTB)

| Subsystem | Vendor fact | Mainline target |
|---|---|---|
| **Main PMIC** | `pmu@34` = `x-powers,axp2202`, reg `0x34`, on **s_twi0** (`r_i2c0`), drive-vbus, IRQ via NMI. Board silk says **AXP717C**. | `r_i2c0` + `x-powers,axp717` *(verify chip — see §3)*. NOT on `i2c0`. |
| **CPU supply (dual cluster)** | cluster0 (cpu@0) `cpu-supply` → **axp2202-dcdc1**; cluster1 (cpu@400) `cpu-supply` → **tcs4838-dcdc0** (`tcs@41` on r_i2c0). So the populated external CPU regulator is **tcs4838@0x41** (not axp1530/sy8827g). | Two separate `cpu-supply` per cluster: little=PMIC dcdc1, big=tcs4838 dcdc0. |
| **Fan** | `pwm-fan`, pwms = ch **10**, 40000 ns, **inverted**. 32 cooling levels. | `pwm-fan` once A523 PWM driver lands. |
| **Vibrator** | `pwm-vibrator`, pwms = ch **7**, 50000 ns, normal polarity. | `pwm-vibrator` once PWM driver lands. |
| **Ethernet** | gmac0 + gmac1 present in vendor DTB (SoC MACs); handheld likely has no RJ45. | Ignore unless a port exists. |
| **LEDs / BT(brcm) / gpio-keys** | **0 hits in vendor DTB** — not present. | Do not add; fabricated if seen elsewhere. |
| **Battery/charger** | axp2202 bat + usb power supply, type-C, vbus detect GPIO. | axp717 power-supply + `simple-battery` (need real mAh/voltages). |
| **eMMC** | mmc2, 8-bit, non-removable, HS200/HS400 1.8V. PC pins. | `&mmc2` bus-width 8, mmc-hs200/hs400-1_8v, non-removable. |
| **microSD** | mmc0, 4-bit, CD = **PF6** active-low, UHS (sdr104/ddr50). | `&mmc0` cd-gpios `<&pio 5 6 ...>`, vmmc/vqmmc from PMIC. |
| **WiFi** | `allwinner,sunxi-wlan`, **SDIO on mmc1 (bus 1)**. Power = axp2202 aldo3(3.3V)+bldo1(1.8V)+bldo2(1.8V). `wlan_regon`=**PB1**, `wlan_hostwake`=**PB0**. Chip not named (generic). | `&mmc1` non-removable + SDIO child. **Identify chip** (lsmod/firmware on device) before picking compatible — likely AIC8800 or similar, NOT bcm/rtl as guessed. |
| **Display** | MIPI-**DSI0**, **4-lane**, `allwinner,dsi0` + combo-phy. Panel reset = **PD22**. Backlight via PWM. | Blocked: no mainline DSI/DE. Will need panel driver + DSI bridge once SoC DE lands. |
| **Backlight** | sunxi-pwm-v201. | Blocked until A523 PWM driver in mainline. |
| **Analog sticks** | **gpadc0** (2ch) + **gpadc1** (2ch) = 2 sticks × X/Y. | `adc-joystick` binding once GPADC driver exists. |
| **Buttons (D-pad/ABXY/L/R/Start/Select)** | **No gpio-keys / no key matrix in vendor DTS.** Almost certainly an **internal USB gamepad MCU** (or I2C MCU). | Investigate on HW (`lsusb`/`evtest`). Do **not** fabricate gpio-keys. |
| **Vol/Power/Home keys** | Check axp2202 PEK (power) + possibly LRADC/MCU for vol. | axp717 power button child; vol = TBD on HW. |
| **Audio** | analog + digital codec (sunxi). Speaker amp enable likely a GPIO. | Blocked until A523 codec driver in mainline. |
| **USB** | otg (type-C), plus internal host ports (ehci/ohci) — one hosts the gamepad MCU. | `&usb_otg`, `&ehci0/1`, `&ohci0/1`, `&usbphy`. |

GPIO encoding in vendor DTS: `<phandle port pin func>` where port 0=PA,1=PB…5=PF,6=PG,7=PH.
e.g. `<&pio 5 6 ...>` = PF6, `<&pio 3 22 ...>` = PD22, `<&pio 1 1 ...>` = PB1.

---

## 2. Review of existing DTS attempts

### `sun55i-t527-trimui-smart-pro_v0.1.1.dts`
- ❌ `#include "sun55i-t527.dtsi"` — no such file in mainline (base is `sun55i-a523.dtsi`).
- ❌ PMIC `axp2202@3a` on `&i2c0` — wrong addr (0x34) and wrong bus (it's on `r_i2c0`/s_twi0). compatible direction OK-ish.
- ❌ References `&dsi0`, `&pwm0`, `&lradc`, `&gpadc`, `&codec_analog/digital` — none exist in mainline → won't build.
- ⚠️ WiFi `brcm,bcm4329-fmac` is a guess; vendor uses generic sunxi-wlan, real chip unknown.
- ✅ DSI panel + 4 lanes + reset PD22 — *correct interface and reset pin* (matches vendor). Good instinct.
- ✅ mmc0 CD = PF6, eMMC HS200/HS400 — correct.
- ⚠️ `simple-audio-card` widgets/routing are plausible scaffolding but unverifiable now.

### `sun55i-t527-trimui-smart-pro_v0.1.2.dts`
- ✅ `#include "sun55i-a523.dtsi"` + compatible `allwinner,sun55i-a523` — correct base.
- ❌ PMIC `axp717c@0x34` on `&i2c0` — right address, **wrong bus** (must be `&r_i2c0`).
- ❌ Display reworked to **parallel RGB666** (`tcon0`, `lcd0_rgb666_pins`, `innolux,gc9503`) — **regression**: the panel is MIPI-DSI 4-lane (v0.1.1 was right).
- ❌ Big fabricated `gpio-keys` block (PG9–PG20) — vendor has no such GPIO buttons; buttons are via USB MCU. Invented.
- ❌ `realtek,rtl8723ds` on SPI — WiFi is SDIO (mmc1), not SPI, and chip is unidentified.
- ❌ References `&de`, `&tcon0`, `&mixer0`, `&pwm`, `&codec`, `&dai` — absent in mainline → won't build.
- ⚠️ Two files diverge on SoC name (t527 vs a523). Pick one: mainline base = **a523**; T527 is the same die, automotive bin — use `allwinner,sun55i-a523` as fallback compatible.

**Net:** v0.1.2 fixed the include but regressed the display and invented inputs/wifi.
The correct baseline merges: v0.1.2's a523 include + v0.1.1's DSI/reset, minus everything mainline can't build yet. See `sun55i-a523-trimui-smart-pro-s.dts`.

### Repo `dts/sun55i-a523-trimui-smart-pro-s.dts` (the "production-ready" one)

**It compiles, but a clean `dtc` build proves nothing about correctness.** `dtc`
only checks syntax + phandle resolution; it does NOT verify register addresses,
`compatible` strings, or that any driver exists. This file declares all the
missing peripherals as new nodes with invented addresses/compatibles, so a DTB
is produced — but most of it cannot bind or work on hardware.

| Item (line) | Problem | Severity |
|---|---|---|
| `&i2c0` PMIC `pmic@34` (327) | PMIC is on **r_i2c0** (`i2c@7081400`), not i2c0 (`i2c@2502000`). Won't probe. | 🔴 |
| `x-powers,axp2202` (332) + `axp2202-battery-power` (383) | No mainline driver/binding for axp2202; mainline has axp717. Won't bind. | 🔴 |
| `usb@2000000/2001000/2002000` (273-324) | **Fabricated addresses** — real USB is 0x4100000/0x4101000/0x4101400/0x4200000/0x4200400, and these **duplicate** `usb_otg`/`ehci0/1`/`ohci0/1` already in the dtsi. | 🔴 |
| `gpadc@2009000` single, 4 ch (265) | Real HW = **two** blocks gpadc0@2009000 + gpadc1@2009c00 (2 ch each). `sun50i-a100-gpadc` ≠ mainline compatible; no A523 GPADC driver yet anyway. | 🔴 |
| `pwm@2000c00` `sun50i-a100-pwm` (257) | No mainline A523 PWM; compatible won't match the v201/v202 IP. (fan ch10 / vibrator ch7 channels ARE correct, though.) | 🔴 |
| `codec`/`codec_plat` `sun50i-a100-codec` (244-254) | Not the A523 codec; no mainline A523 audio. Won't bind. | 🔴 |
| `gpio-keys` PD12-21/PE10-14 (26-119) | **Invented** — vendor DTB has no gpio buttons (USB MCU). PD14-22 region overlaps display/LCD pins (PD22 = panel reset). | 🔴 |
| `gpio-leds` PG14/PG15 (156) | **Invented** — 0 hits in vendor DTB. | 🟠 |
| `brcm,bcm43438-bt` on uart1 + PB2/3/4 (395) | WiFi/BT chip **unidentified**; vendor uses generic sunxi-wlan over SDIO. BT chip + pins are guesses. | 🟠 |
| `cpu-supply = <&dcdc1>` ×8 (415-422) | Half-right: cluster0 → dcdc1, but cluster1 (cpu@400+) → **tcs4838-dcdc0**. | 🟠 |
| battery 5000 mAh / 4.35 V / OCV table (209) | Plausible but unverified; confirm on HW. | 🟡 |
| fan ch10/40000/inv, vibrator ch7/50000 (174-187) | **Correct** vs vendor (good) — but inert until PWM driver exists. | 🟢 |
| `&{/}` re-declaring SoC nodes | Wrong layering: SoC peripherals belong in an upstreamed `.dtsi`, not redeclared in the board file with made-up addresses. | 🟠 |

**Verdict:** treat the repo DTS as a *wishlist/scratchpad*, not a port. The honest,
buildable state is `sun55i-a523-trimui-smart-pro-s.dts` here (phase 1-2 only).

---

## 3. The one thing to verify first on real hardware

`i2cdetect` the r_i2c0 bus and dump PMIC ID register:
- Confirm PMIC at 0x34 and whether it answers as **AXP717** or **AXP2202**
  (board silk = AXP717C; vendor driver = axp2202; mainline only has axp717).
- Confirm which CPU/GPU regulator is populated (0x36 axp1530 / 0x41 tcs4838 / 0x60 sy8827g).
- `lsusb` / `dmesg` to identify the gamepad MCU and WiFi chip.
- `cat /sys/.../power_supply/*/` for real battery design capacity/voltages.

---

## 4. Phased bring-up plan

**Phase 0 — toolchain & boot path**
- Mainline U-Boot for A523 (or chainload from vendor U-Boot). UART0 console (PB pins).
- Build kernel `defconfig` + arm64, our DTS. Boot from microSD first.

**Phase 1 — console + storage** *(buildable today)*
- chosen/serial0, `&mmc0` (SD), `&mmc2` (eMMC), `&r_i2c0` + PMIC stub.
- Goal: kernel boots to shell over UART, sees SD + eMMC.

**Phase 2 — power + USB + WiFi** *(buildable today)*
- Full axp717 regulators + power button + battery; `cpu-supply` → CPU dcdc; OPP/cpufreq.
- `&usb_otg`/`&ehci*`/`&ohci*`/`&usbphy`. Identify + enable gamepad MCU over USB (evtest).
- `&mmc1` SDIO + WiFi node once chip identified.

**Phase 3 — needs new/forthcoming SoC drivers**
- Backlight: A523 PWM driver → `pwm-backlight`.
- Analog sticks: A523 GPADC driver → `adc-joystick`.
- Volume keys: depending on HW (LRADC or MCU).

**Phase 4 — display (hardest)**
- A523 Display Engine + MIPI-DSI driver (likely upstream-in-progress / port from vendor).
- Panel: 4-lane DSI, reset PD22 — write panel driver / use panel-simple-dsi once timings known.

**Phase 5 — audio, GPU (Mali), VPU**
- A523 audio codec driver; Mali (Panthor/Bifrost) + VPU when mainline support matures.

---

## 5. Repo structure suggestion

```
dts/sun55i-a523-trimui-smart-pro-s.dts   # our board (kernel naming convention)
PORTING-NOTES.md                          # this file (truth table + roadmap)
vendor/trimui_smart_pro_source.dts        # decompiled vendor DTB (reference only)
vendor/aliases_dts.txt battery.txt codec.txt   # extracted vendor sub-dumps
```
Mainline kernel path would be `arch/arm64/boot/dts/allwinner/sun55i-a523-trimui-smart-pro-s.dts`.
</content>
</invoke>
