# Trimui Smart Pro S gamepad protocol (WIP — 2026-08-18)

Two serial MCUs (`02N210542`, house-marked), one per side board, each reads its
side's Hall joystick + buttons and streams **RX-only at 9600 baud, 8N1**:
- **uart5 = /dev/ttyS2 = LEFT**  (D-pad, Menu, left stick, L1/L2/L3)
- **uart7 = /dev/ttyS3 = RIGHT** (A/B/X/Y, Select, Start, right stick, R1/R2/R3)

Power: the side-board +5V is gated by **PK15** (reg_5v_en) — without it both MCUs
are dead (see the dts). Both MCUs stream continuously (no host TX needed).

## Frame = 10 bytes. The two sides use DIFFERENT layouts (mirror firmware):

### LEFT (ttyS2) — terminator 0x80 at byte[9] (constant):
`[b0] 00 00 [X] [Y] [b5] 00 00 00 80`
- b0: `0x0f`/`0x1f` — bit4 toggles every frame = **frame parity** (NOT a button).
- **byte[3] = stick X** (center ~0x70, full-left ~0x48; range ~0x40..0xbc). CONFIRMED.
- **byte[4] = stick Y** (center ~0x6a).
- byte[1] bit3 (0x08) = a button (seen pressed). byte[5] bit6 (0x40) also toggles — TBD.

### RIGHT (ttyS3) — sync on 0x0f header at byte[0]; NO 0x80 terminator
(the 0x80 seen at idle was the centered right-Y axis value):
`0f 00 00 00 [b4] [b5] [b6] 02 00 00`
- byte[4]=0x00/0x08, byte[5]~0x20, byte[6]~0x41 vary with the stick — axis bytes
  not yet cleanly mapped (small observed range; needs held-extreme captures).
- byte[7]=0x02 constant.

## Status
- `trimui-gamepad.py` (python3-evdev): reads both ttys @9600, 0x80-frames the LEFT,
  emits ABS_X/ABS_Y to a uinput pad — **LEFT STICK WORKS (full range)**. Also prints
  changing button-candidate bytes to help finish the map.
- TODO: right-frame sync (0x0f header) + right axis bytes; full button map (dpad+menu
  left / abxy+select+start right / L,R / L3,R3); calibration (/mnt/UDISK/joypad.config:
  left x344-3655 y315-3231 center1958/1713; right x539-3790 y101-3774 center2032/1836).
- Upstream target: a mainline serdev driver (drivers/input/joystick) per side once the
  protocol is fully mapped; this python parser is the interim + the RE reference.
