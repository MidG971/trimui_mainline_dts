<!-- SPDX-License-Identifier: (GPL-2.0-only OR MIT) -->
<!-- Copyright (C) 2026 Midgy BALON -->

# Trimui Smart Pro S — board pin map (validated)

Cross-referenced from: vendor DTB pinctrl groups + gpio refs (`trimui_smart_pro_source.dts`),
A523 **Datasheet** mux tables (ports B,C,D,E,F,G,H,K,L,M), A523 **User Manual** §8.5 GPIO,
and the BSP pinctrl driver (ports **I, J** — absent from the datasheet).

GPIO decode in vendor DTB: `<phandle bank pin flags>`.
`phandle 0x59 = &pio` (banks A=0,B=1,…,K=10); `phandle 0x5a = &r_pio` (PL=0, PM=1).

## Buses & storage (matches current DTS ✅)

| Function | Pins | Mux | Notes |
|---|---|---|---|
| **UART0 console** | PB9 TX / PB10 RX | mux2 | `uart0_pb_pins`. Vendor can also route UART0→PF2/PF4 (debug-on-SD). |
| **r_i2c0 (PMIC bus, = S-TWI0)** | PL0 SCK / PL1 SDA | mux2 | `i2c@7081400`. axp2202@0x34, tcs4838@0x41. |
| S-TWI2 (USB-C PD bus) | PL12 SCK / PL13 SDA | mux2 | husb311 PD controller, intr=PL13. |
| **microSD (mmc0 = SDC0)** | PF0–PF5 + CD **PF6** (act-low) | mux2 / GPIO | D1,D0,CLK,CMD,D3,D2. |
| **WiFi SDIO (mmc1 = SDC1)** | PG0–PG5 | mux2 | CLK,CMD,D0–D3. |
| **eMMC (mmc2 = SDC2), 8-bit** | PC0 DS, PC1 RST, PC5 CLK, PC6 CMD, PC8 D3, PC9 D4, PC10 D0, PC11 D5, PC13 D1, PC14 D6, PC15 D2, PC16 D7 | mux3 | shares NAND pins. |

## WiFi / Bluetooth — AIC8800 (control on R_PIO **Port M**)

⚠️ **Correction:** earlier notes said PB0/PB1 — that was a bad decode (phandle `0x5a`
is `&r_pio`, bank 1 = **PM**, not PB). Real lines:

| Signal | Pin | Flag |
|---|---|---|
| `wlan_regon` (WL power/enable) | **PM1** | active-high |
| `wlan_hostwake` | **PM0** | |
| `chip_en` | **PM5** | |
| `bt_rst_n` | **PM2** | active-low |
| `bt_wake` | **PM3** | |
| `bt_hostwake` | **PM4** | |

Rails (axp2202): `aldo3`=3.3V, `bldo1`=1.8V, `bldo2`=1.8V (shared by WiFi+BT).
BT HCI UART = likely **UART1** (PG6 TX/PG7 RX/PG8 RTS/PG9 CTS — `uart1@0` group).

## Display — panel on **DSI1** (not DSI0)

⚠️ **Correction:** `dsi0@5506000` is `status=disabled`; **`dsi1@5508000` is `okay`**.
Panel routes through DSI1.

| Item | Value |
|---|---|
| Interface | MIPI-DSI, **4 lanes**, RGB888 (`dsi,format=0`) |
| DSI1 lane pins | **PD10–PD19** (D0±=PD10/11, D1±=PD12/13, CK±=PD14/15, D2±=PD16/17, D3±=PD18/19) |
| Panel reset | **PD22** (`reset-num=1`, `reset-delay-ms=120`) |
| Resolution | **720×1280 @ ~60 Hz** (portrait panel; landscape after rotate) |
| Pixel clock | 62 MHz (`hbp 18 / hfp 32 / hsync 6`, `vbp 34 / vfp 16 / vsync 2`) |
| Backlight | `pwm-backlight`, period 50000 ns, default level 0x32 |
| Panel rails | power0-supply, power1-supply (PMIC LDOs) |
| Panel init | full MIPI DCS `panel-init-sequence` present in vendor DTB → reusable. |

(`COMBOPHY_DSI1` @ 0x05508000, `TCON_LCD` + `DE` per `A523-DOCS-INDEX.md`.)

## Other discrete GPIOs decoded (verify "reference-cruft" ones on HW)

| Pin | Vendor property | Board-real? |
|---|---|---|
| PD22 | panel `reset-gpios` | ✅ yes |
| PF6 | SD `cd-gpios` | ✅ yes |
| PL13 | `husb311,intr_gpio` (USB-C PD) | ✅ likely |
| PH13 / PH14 | `pmu_vbus_det` / `pmu_acin_det` | ✅ likely |
| PH7 / PH8 | `usb_gma340_sel/oe` (USB switch) | ? verify |
| PH9 / PH10 | focaltech touch irq/reset | ✖ probably unpopulated (no touch) |
| PB11/PB12, PL2/PL3 | JTAG tck/tms, tdi/tdo | debug only |
| PB4 test, PL4 suspend | misc | verify |

## Inputs (no gpio-keys in vendor DTB)

- **Volume/side keys:** LRADC (`lradc@2009800`, `allwinner,keyboard_1350mv`, 3 keys). UM §8.7.
- **D-pad / ABXY / shoulders:** userspace `trimui_inputd` daemon (gamepad); kernel source TBD on HW.
- **Power:** axp2202 PEK.
- **Analog sticks:** GPADC (UM §8.4) — adc-joystick once driver exists.

## Ports I & J (from BSP driver — not in datasheet)

Mux0=GPIO-IN, mux1=GPIO-OUT, mux14=EINT on all pins.

**Port I (17 pins):** mux2/3 mostly UART4/5/6, I2C4/5, SPI1/2, PWM0-x, I2S2, DMIC, CIR, OWA.
**Port J (28 pins):** mux2 = LCD1-D0..23/CLK/DE/HSYNC/VSYNC; mux3 = LVDS2/LVDS3; mux5 = RGMII1;
PJ20–27 also UART2/3, SPI0. → Port J is the **second display/LVDS + 2nd Ethernet**.

**This board uses neither I nor J** (panel is DSI1 on Port D; no 2nd display, no Ethernet).
The `lvds2/lvds3/rgb1` groups that reference PJ are SoC-menu options, unused here.
