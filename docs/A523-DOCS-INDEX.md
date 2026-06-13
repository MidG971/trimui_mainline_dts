<!-- SPDX-License-Identifier: (GPL-2.0-only OR MIT) -->
<!-- Copyright (C) 2026 Midgy BALON -->

# A523 Documentation Index (local reference)

Two Allwinner A523 documents are in `docs/`. **Both are Allwinner "Confidential"**
(footer on every page; the datasheet is additionally RC4-encrypted). They are kept
**local only — do NOT commit them or verbatim copies to the public repo.** Derived
functional facts (pin functions, base addresses, timings) are fine to use in our
own DTS/notes; those values are also visible in public sunxi/T527 sources.

| File | What it is | Pages | Use |
|---|---|---|---|
| `a523_trm.pdf` | **A523 Datasheet** (mislabeled "trm"). Overview, pinout, **GPIO mux tables**, **electrical characteristics**, pin assignment, thermal. No registers. | 137 | Pin mux (ports B,C,D,E,F,G,H,K,L,M), electrical (DRAM/GPADC/LRADC/codec/power). |
| `A523_User_Manual_V1.1_merged_cleaned.pdf` | **Full register-level User Manual** (the real TRM). Per-peripheral register maps. Merged from module manuals → each module has its own sub-TOC. | 1909 | Driver bring-up: CCU, DSI/TCON/DE, GPADC, LRADC, codec, SMHC, TWI, PWM, DRAMC. |

Datasheet omits GPIO **Port I** and **Port J** — those came from the BSP pinctrl
driver (see `BOARD-PINMAP.md`).

## User Manual chapter → page map

Page numbers are the **printed** page (TOC). **PDF page = printed + 1.**

| § | Chapter | Printed p. | PDF p. |
|---|---|---|---|
| 2.1 | Memory Mapping (base addresses) | 40 | 41 |
| 2.2 | CPUX (Cortex-A55) | 45 | 46 |
| 2.5 | **CCU** (clocks) | 101 | 102 |
| 2.6 | DMAC | 210 | 211 |
| 2.7 | GIC | 254 | 255 |
| 2.11 | PRCM | 350 | 351 |
| 2.12 | RTC | 406 | 407 |
| 2.14 | THS (thermal) | 437 | 438 |
| 2.16 | Watchdog (WDT) | 462 | 463 |
| 3.1 | NAND Flash (NDFC) | 472 | 473 |
| 3.2 | **SDRAM controller (DRAMC)** | 473 | 474 |
| 3.3 | **SD/MMC (SMHC)** | 474 | 475 |
| 4.1 | **Audio Codec** | 545 | 546 |
| 4.2 | I2S/PCM | 625 | 626 |
| 4.3 | DMIC | 676 | 677 |
| 4.4 | OWA (SPDIF) | 690 | 691 |
| 5.1 | Display Engine (DE) | 722 | 723 |
| 5.3 | G2D | 725 | 726 |
| 6.2 | **MIPI DSI** | 757 | 758 |
| 6.3 | **TCON LCD** | 758 | 759 |
| 6.4 | TCON TV | 800 | 801 |
| 7.1 | CSIC (camera) | 816 | 817 |
| 8.1 | CIR RX | 1025 | 1026 |
| 8.4 | **GPADC** (analog sticks) | 1074 | 1075 |
| 8.5 | **GPIO / Port Controller** | 1092 | 1093 |
| 8.6 | LEDC | 1284 | 1285 |
| 8.7 | **LRADC** (vol keys) | 1304 | 1305 |
| 8.8 | USB2.0 DRD | 1314 | 1315 |
| 8.9 | USB2.0 HOST | 1390 | 1391 |
| 8.11 | USB3.1 DRD | 1441 | 1442 |
| 8.12 | PCIe2.1 | 1570 | 1571 |
| 8.13 | **Two Wire Interface (TWI/I2C)** | 1611 | 1612 |
| 8.14 | **PWM** (backlight/fan/vibrator) | 1637 | 1638 |
| 8.15 | SPI | 1696 | 1697 |
| 8.18 | UART | 1829 | 1830 |
| 9.1 | Crypto Engine | 1864 | 1865 |

## Peripheral memory map (§2.1, base addresses)

| Block | Base | Notes |
|---|---|---|
| GPIO (PIO) | `0x02000000` | main pinctrl |
| PWMCTRL0 | `0x02000C00` | fan/vibrator/backlight |
| CCU | `0x02001000` | |
| LEDC | `0x02008000` | |
| GPADC | `0x02009000` | analog sticks |
| LRADC | `0x02009800` | volume/side keys |
| THS0 / THS1 | `0x0200A000` / `0x02009400` | thermal |
| UART0..7 | `0x02500000` + 0x400·n | console = UART0 |
| TWI0..5 | `0x02502000` + 0x400·n | |
| DMAC | `0x03002000` | |
| **SMHC0 / SMHC1 / SMHC2** | `0x04020000` / `0x04021000` / `0x04022000` | SD / SDIO-wifi / eMMC |
| SPI0/1/2 | `0x04025000`/`6000`/`7000` | |
| USB0 / USB1 | `0x04100000` / `0x04200000` | otg / host |
| USB3.1+PCIe top | `0x04F00000` | |
| **DE (Display Engine)** | `0x05000000` (4 MB) | |
| **DISPLAY0_TOP** | `0x05500000` | |
| **TCON_LCD0 / TCON_LCD1** | `0x05501000` / `0x05502000` | |
| TCON_TV1 | `0x05504000` | |
| **COMBOPHY_DSI0 / DSI1** | `0x05506000` / `0x05508000` | panel is on **DSI1** |
| RTC | `0x07090000` | |
| **S_TWI0 (= r_i2c0, PMIC bus)** | `0x07081400` | axp2202 @ 0x34, tcs4838 @ 0x41 |
| S_TWI1 / S_TWI2 | `0x07081800` / `0x07081C00` | husb311 PD on S_TWI2 |
| S_GPIO (R_PIO) | `0x07022000` | PL/PM banks |
| AUDIO CODEC | `0x07110000` | |
| I2S0..3 | `0x07112000` + 0x1000·n | |
| DRAM space | `0x40000000`– | |
