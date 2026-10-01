<!-- SPDX-License-Identifier: (GPL-2.0-only OR MIT) -->
<!-- Copyright (C) 2026 Midgy BALON -->

# Gamepad serdev driver — skeleton + DT nodes + fill-in workflow

Drafted 2026-09-25 (offline). Driver skeleton:
`kernel/drivers/gamepad-trimui-smart-pro-s.c`. The pad is two 19200-baud RX-only MCUs on
**uart5 (ttyAS5, PK17)** and **uart7 (ttyAS7, PK13)**; together they carry the whole pad.

## DT nodes (serdev children under the gamepad UARTs)
The board DTS already enables `&uart5`/`&uart7` (`dts/sun55i-a523-trimui-smart-pro-s.dts:200`).
Add a serdev child to each so the driver binds:

```dts
&uart5 {
	pinctrl-names = "default";
	pinctrl-0 = <&uart5_pk17_pin>;
	status = "okay";

	gamepad_left: gamepad {
		compatible = "trimui,smart-pro-s-gamepad-left";
		current-speed = <19200>;
	};
};

&uart7 {
	pinctrl-names = "default";
	pinctrl-0 = <&uart7_pk13_pin>;
	status = "okay";

	gamepad_right: gamepad {
		compatible = "trimui,smart-pro-s-gamepad-right";
		current-speed = <19200>;
	};
};
```
(These nodes are now in the canonical board DTS. The compatibles were changed from
`-uart5`/`-uart7` to `-left`/`-right` so they describe the MCU's function, not the
wiring — the node already lives under the right UART controller.)
Notes:
- A serdev child has **no `reg`** (single device on the wire) and makes the port a serdev bus —
  **`/dev/ttyAS5` and `/dev/ttyAS7` disappear** once the driver binds. That's expected.
- **CAPTURE vs serdev are mutually exclusive** on a UART: to reverse-engineer the protocol you
  read the raw **tty** (`/dev/ttyAS5`), which needs NO serdev child. So do the capture FIRST with
  the current tty DT, then add these nodes to switch to the driver. Keep the two states separate
  (e.g. a `-capture`/`-serdev` overlay) while iterating.

## Kconfig / Makefile
`drivers/input/joystick/Kconfig` (or wherever it lands upstream):
```
config JOYSTICK_TRIMUI_SMART_PRO_S
	tristate "Trimui Smart Pro S gamepad (serdev)"
	depends on SERIAL_DEV_BUS
	select INPUT
	help
	  Serdev input driver for the two 19200-baud gamepad MCUs on the
	  Trimui Smart Pro S (uart5/uart7).
```
Makefile:
```
obj-$(CONFIG_JOYSTICK_TRIMUI_SMART_PRO_S) += gamepad-trimui-smart-pro-s.o
```
Also ensure `CONFIG_SERIAL_DEV_BUS=y` and `CONFIG_SERIAL_DEV_CTRL_TTYPORT=y` in the kernel config.

## Fill-in workflow (what makes the skeleton real)
Everything marked `TODO(protocol-map)` in the driver comes from the capture analysis:
1. **Capture (device up, NO serdev child):** on mainline the vendor daemon is absent, so read the
   raw stream straight off the tty — one file per held control. Minimal, no app needed:
   ```sh
   stty -F /dev/ttyS2 19200 raw -echo
   timeout 3 cat /dev/ttyS2 > gp5-neutral.bin      # nothing held
   timeout 3 cat /dev/ttyS2 > gp5-A.bin            # hold A ... etc per control
   # repeat for /dev/ttyAS7
   ```
   (Or use the existing `gamepad-logger/` app on stock; but the mainline tty path is simpler now
   that PK15 side-board +5V is enabled and the MCUs stream — see [[input-output-subsystem-bringup]].)
2. **Analyze (offline):** `python3 gamepad-logger/analyze-gamepad.py <dir>` → `protocol-map.json`
   (frame length, sync byte+offset, per-button byte+bit, per-axis byte+range, per UART).
3. **Fill the driver:** transcribe the map into `trimui_gp_uart5`/`trimui_gp_uart7`
   (`frame_len`, `sync_byte`, `sync_off`, `active_low`) and the `trimui_gp_btns_*` /
   `trimui_gp_axes_*` tables (currently empty stubs). Pick BTN_/ABS_ codes to match an Xbox-style
   pad (BTN_A/B/X/Y, BTN_TL/TR, ABS_X/Y/RX/RY, ABS_Z/RZ for L2/R2, BTN_THUMBL/R, HAT for D-pad).
4. **Build (needs compile server or laptop kit):** add the Kconfig/Makefile, build the module,
   deploy. Until filled, the driver frame-syncs and logs only (reports no events) — safe to load.

## Single-pad merge — DONE (2026-10-01)
The driver is no longer a skeleton. The two serdev instances share ONE `input_dev`
("TRIMUI Player1", BUS_USB 045e:028e) via a kref'd module-level singleton: the first
to probe builds + registers the pad, the second reuses it, and it is torn down when
the last instance unbinds. (A DT phandle linking the two nodes was the alternative,
but serdev nodes must be children of their UART controllers so they cannot share a
DT parent; the singleton keeps the binding trivial since there is exactly one pad.)

Mapping matches the verified stock/daemon contract (docs/GAMEPAD-STOCK-GOAL.md):
D-pad → `ABS_HAT0X/Y` (−1/0/+1); L2/R2 → `ABS_Z/RZ` (digital 0/255); sticks →
`±32767` with per-axis calibration, a 10% deadzone (integer-scaled, no kernel FP),
and Y inverted (Xbox up = negative). FF/rumble is NOT yet wired (follow-up — stock
routes `FF_RUMBLE` to the pwm-vibrator; in-kernel this needs a cross-device forward).

Build (out-of-tree against the 7.2-rc3 tree, vermagic `7.2.0-rc3-dirty`):
`make -C <kernel> M=<dir> ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules`.
Built `.ko` staged at `kernel/modules/gamepad-trimui-smart-pro-s.ko`.
