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

	gamepad_uart5: gamepad {
		compatible = "trimui,smart-pro-s-gamepad-uart5";
		current-speed = <19200>;
	};
};

&uart7 {
	pinctrl-names = "default";
	pinctrl-0 = <&uart7_pk13_pin>;
	status = "okay";

	gamepad_uart7: gamepad {
		compatible = "trimui,smart-pro-s-gamepad-uart7";
		current-speed = <19200>;
	};
};
```
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

## Single-pad merge (final driver, not the skeleton)
The skeleton registers one input device per MCU. For the shipping driver, expose ONE pad: share a
single `input_dev` between the two serdev instances (e.g. a phandle from one node to the other, or
a small parent binding), so userspace sees a single controller like the vendor's Xbox360 uinput pad.
Decide the exact mechanism once the per-UART control split is known from the capture.
