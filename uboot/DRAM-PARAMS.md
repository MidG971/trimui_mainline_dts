<!-- SPDX-License-Identifier: (GPL-2.0-only OR MIT) -->
<!-- Copyright (C) 2026 Midgy BALON -->

# Trimui Smart Pro S — DRAM parameters extracted from vendor boot0 (2026-06-04)

Source: stock firmware `trimui_tg5050.awimg`. The vendor **boot0** (eGON.BT0, 76 KB)
appears at byte offset **271360** (and a 2nd identical-DRAM copy at 349184). Carved to
`vendor-boot0/boot0_a.bin` / `boot0_b.bin`. `sunxi-bootinfo` confirms the eGON header
(Length 77824, HSize 48, platform "4.0").

## How it was decoded
The vendor boot0 stores a `dram_para` struct. Its layout was pinned down by **anchoring
to the known Avaota-A1 values** (same SoC family): the fields `dx_odt/dx_dri/ca_dri/odt_en`
and `tpr0/tpr1/tpr6/tpr10/tpr11/tpr12` line up byte-for-byte in order, which fixes every
slot. Both boot0 copies carry identical DRAM values.

Raw `dram_para` region (u32 LE), boot0 offset 0x38:
```
0x38: 000004b0 00000008  07070707 0d0d0d0d  00000e0e 84848484   <- clk,type,dx_odt,dx_dri,ca_dri,odt_en
0x50: 0000310a 10001000  00000000 00000034  ...                 <- para0,para1,para2,mr0,... (vendor-only)
0x90: 80808080 06060606  1f090503 00000000  3a000000 862f3333   <- tpr0,tpr1,tpr2,tpr3,tpr6,tpr10
0xa8: c0c0bbbf 35352f31  00000c64 48484848                      <- tpr11,tpr12,tpr13,tpr14
```

> **★ RESOLVED ON HARDWARE — the vendor-boot0 overrides below did NOT train.**
> The table is an accurate decode of what the *vendor* boot0 carries, but those
> board-specific values (`tpr2/tpr6/tpr10/tpr11/tpr12`) **failed DRAM training on the
> mainline A523 driver**. The config that actually boots uses the **A523 / Avaota-A1
> reference values** (the right-hand column) — i.e. we *drop* the vendor overrides rather
> than apply them. See **"Applied to U-Boot — what actually boots"** below. The vendor
> `dram_para` is kept here for provenance only; it is not the config we ship.

## Decoded values (the 12 fields mainline U-Boot's `struct dram_para` uses)

| field | Trimui value | Avaota-A1 | note |
|---|---|---|---|
| clk | **1200** | 1200 (A523 default) | LPDDR4 1200 MHz |
| type | **8 = LPDDR4** | LPDDR4 (A523 default `SUNXI_DRAM_A523_LPDDR4`) | |
| dx_odt | `0x07070707` | `0x07070707` | same |
| dx_dri | `0x0d0d0d0d` | `0x0d0d0d0d` | same |
| ca_dri | `0x00000e0e` | `0x0e0e` | same |
| odt_en | `0x84848484` | `0x84848484` | same |
| tpr0 | `0x80808080` | `0x80808080` | same |
| **tpr2** | **`0x1f090503`** | (default 0x0) | **Trimui-specific** |
| **tpr6** | **`0x3a000000`** | `0x38000000` | **Trimui-specific** |
| **tpr10** | **`0x862f3333`** | `0x802f3333` | **Trimui-specific** |
| **tpr11** | **`0xc0c0bbbf`** | `0xc7c5c4c2` | **Trimui-specific** |
| **tpr12** | **`0x35352f31`** | `0x3533302f` | **Trimui-specific** |

The 5 bold fields are the board-specific values **in the vendor boot0**. On the mainline
A523 DRAM driver they **failed to train**, so the booting config uses the **Avaota-A1
column** for all of them (`tpr2` left at its default). The vendor delta is recorded for
provenance only — it is not what we ship.

## Applied to U-Boot — what actually boots
The booting `trimui-tg5050_defconfig` (this dir + `configs/` on the build server) uses the
**A523 / Avaota-A1 reference values, NOT the vendor overrides above.** The DRAM block is:
`DX_ODT=0x07070707`, `DX_DRI=0x0d0d0d0d`, `CA_DRI=0x0e0e`, `ODT_EN=0x84848484`,
`TPR0=0x80808080`, `TPR1=0x06060606`, `TPR6=0x38000000`, `TPR10=0x802f3333`,
`TPR11=0xc7c5c4c2`, `TPR12=0x3533302f` (no `TPR2`), at `CONFIG_DRAM_CLK=1200` (LPDDR4,
1 GiB). Plus the two FIT boot fixes (`RAW_MODE_U_BOOT_SECTOR=0x200`, `DATA_PART_OFFSET=0x0`).
The hand-decoded vendor overrides were tried first and **failed to train** — that training
failure (invisible without a serial console) is why the boot was blocked for months.

## Confidence / caveats
- DRAM clk/type/odt/dri and tpr mapping are **high confidence** (every anchor field
  matched Avaota in order; two boot0 copies agree). This is the best possible config
  short of booting the device.
- **RESOLVED on HW:** the mainline A523 DRAM driver does **not** train with the vendor boot0
  timings (vendor boot0 ≠ mainline driver internals — MR/timing are recomputed by the driver,
  not taken from boot0). It trains cleanly with the A523 / Avaota-A1 reference values, which is
  what we ship. Confirmed by a UART boot: `DRAM: 1024 MiB` (SPL) → `DRAM: 1 GiB` (U-Boot).
- **PMIC confirmed on HW = AXP2202 @ 0x34** (X-Powers OEM name for AXP717 silicon; mainline
  `axp717` driver binds). VCC-DRAM = **1.10 V** (DCDC3); the DCDC2/CPU rails are mapped in the
  live-capture notes. The old `CONFIG_AXP_I2C_ADDRESS=0x34` is dropped — 0x34 is the driver
  default (see `trimui-tg5050_defconfig`).
