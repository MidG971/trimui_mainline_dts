<!-- SPDX-License-Identifier: (GPL-2.0-only OR MIT) -->
<!-- Copyright (C) 2026 Midgy BALON -->

# Trimui Smart Pro S gamepad — DEFINITIVE wire protocol

Reverse-engineered 2026-09-25 from the vendor daemon **`trimui_inputd`** (v1.0.2, aarch64,
`v1.0.2-mining/carved/rfs_12/usr/trimui/bin/trimui_inputd`) — the authoritative source, vs.
inferring from raw captures. Key functions: SetupSerial `0x1dbc`, recv `0x2290`, LEFT decode
`0x30d0`, RIGHT decode `0x3414`, stick decode `0x2de0`, turbo `0x2bac`.

## Transport
- **19200 baud, 8N1, RX-only, single wire each.** Two identical MCUs, one per side:
  - LEFT  = uart5 = `/dev/ttyAS5` (vendor) = `/dev/ttyS2` (mainline).
  - RIGHT = uart7 = `/dev/ttyAS7` (vendor) = `/dev/ttyS3` (mainline).
- Vendor loop: open → SetupSerial(19200,8N1) → { recv up to 99B → decode → usleep 16ms }.
- ⚠️ **Our earlier captures used 9600 = HALF baud** → each real 20-byte frame aliased into ~10
  garbled bytes, which is exactly the fake "10-byte / variable frame" we saw. **Recapture at 19200.**

## Frame (19 bytes, fixed; vendor memcpy over-reads 1)
| off | field | notes |
|---|---|---|
| 0 | **0xFF** header | validated; frame rejected if != 0xFF |
| 1 | ? | type/counter (TBD) |
| 2..5 | **32-bit button bitmask** (little-endian) | `mask = b2 | b3<<8 | b4<<16 | b5<<24`; each set bit = a button, mapped to a KEY code via a runtime table |
| 6..7 | **stick X** (u16 LE, 12-bit ADC) | calibrated (min/max/center/deadzone) |
| 8..9 | **stick Y** (u16 LE) | calibrated |
| 10..13 | ? | (TBD — maybe unused / second-stick component) |
| 14..15 | **analog trigger** (u16 LE) | L2 on LEFT MCU, R2 on RIGHT ("z-trigger") |
| 16..17 | ? | (TBD) |
| 18 | **0xFE** footer | validated; frame rejected if != 0xFE |
| 19 | ? | checksum/pad (TBD) |

Decode logic per side (`0x30d0`/`0x3414`): validate FF/FE → build 32-bit mask from b2..b5 →
turbo(L2/R2) → emit buttons from the mask via lookup tables (`0x16020` stride16 ×5, `0x16080`
stride12 ×3) → stick decode `0x2de0` (ldrh @6/@8/@14 + per-axis calibrate `0x1ab0/0x1ba0/0x1c90`)
→ save frame+mask as "previous" for edge detection.

## Calibration (from /mnt/UDISK/joypad.config, joypad_right.config)
left:  X 344-3655, Y 315-3231, center 1958/1713, deadzone 0.10
right: X 539-3790, Y 101-3774, center 2032/1836
(12-bit ADC range → scale to input ABS range.)

## COMPLETE button/stick map (HW-verified 2026-09-25 @19200)
Each MCU fills only its own half of a shared frame layout; the other half's bytes are 0.

**Button bitmask (bytes[2..5], u32 LE) — bit → control:**
| bit | control | MCU | | bit | control | MCU |
|---|---|---|---|---|---|---|
| 0 | B | right | | 8 | A | right |
| 1 | Y | right | | 9 | X | right |
| 2 | Select | right | | 10 | L1 | left |
| 3 | Start | right | | 11 | R1 | right |
| 4 | D-pad Up | left | | 12 | L2 | left |
| 5 | D-pad Down | left | | 13 | R2 | right |
| 6 | D-pad Left | left | | 14 | L3 | left |
| 7 | D-pad Right | left | | 15 | R3 | right |
| 16 | Menu | left | | | | |

(bits 17-31 unused. L1/L2/L3=10/12/14, R1/R2/R3=11/13/15. Face B/Y=0/1, A/X=8/9.)

**Analog sticks (u16 LE, 12-bit ADC):**
- LEFT stick:  X @ bytes 6:7,  Y @ bytes 8:9   (filled by the LEFT MCU; 0 on the right MCU's frame)
- RIGHT stick: X @ bytes 10:11, Y @ bytes 12:13 (filled by the RIGHT MCU)
- byte 14:15 = analog trigger slot (z-trigger) — reads **0 on this unit; L2/R2 are DIGITAL** (bits 12/13).

## NOT on the gamepad MCUs (separate inputs)
- **Home** = LRADC side-key (KEY_HOMEPAGE @410mV, sun4i-lradc-keys — already working).
- **Fn** = dip-switch on **GPIO PL11 (gpio363)** (vendor trimui_inputd reads it) → mainline `gpio-keys`.
- **Power** = AXP PEK (already working).

## Minor TBD
- byte[1] = 0x01 constant (type/player id?); bytes[16:17], byte[18]=0xFE ftr; byte after = next 0xFF.
  No functional impact — all controls are mapped.

## Driver
`kernel/drivers/gamepad-trimui-smart-pro-s.c` — update to: current-speed **19200**, frame_len 20,
sync byte 0xFF@0 + footer 0xFE@18, buttons = u32 LE @2, axes u16 LE @6/@8/@14. Then bind serdev
nodes (docs/GAMEPAD-SERDEV-PLAN.md) and fill the bit→KEY table from the 19200 recapture.
