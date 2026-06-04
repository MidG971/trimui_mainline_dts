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

The 5 bold fields are the board-specific training/timing values; everything else matches
the A523 family defaults. These 5 are the entire DRAM delta vs the Avaota config.

## Applied to U-Boot
`trimui-tg5050_defconfig` (in this dir, and `configs/` on the build server) = Avaota-A1
config + the 5 DRAM overrides + `CONFIG_AXP_I2C_ADDRESS=0x34`. Built →
`u-boot-sunxi-with-spl-trimui.bin` (valid eGON.BT0; differs from the Avaota reference
image precisely in the SPL DRAM region).

## Confidence / caveats
- DRAM clk/type/odt/dri and tpr mapping are **high confidence** (every anchor field
  matched Avaota in order; two boot0 copies agree). This is the best possible config
  short of booting the device.
- Still unverified until HW: that the mainline reverse-engineered A523 DRAM driver trains
  successfully with these exact params (vendor boot0 ≠ mainline driver internals; MR/timing
  are recomputed by the driver, not taken from boot0). First FEL boot will tell.
- PMIC set to 0x34 (known from vendor DTS) but chip ID (axp717 vs axp2202) and DRAM/CPU
  voltages still need the on-device check (`../recon.sh` §3).
