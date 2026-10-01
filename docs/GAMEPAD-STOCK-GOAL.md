# Stock gamepad/input — the target contract (captured 2026-09-25)

Captured on stock firmware (Longan 5.15.147) via `adb` + `getevent`. This is the
**evdev behaviour our mainline port must reproduce** for drop-in game/emulator
compatibility. Raw logs: `docs/stock-input-capture/`.

## The pad: `TRIMUI Player1`

Stock's `trimui_inputd` merges both gamepad MCUs into **one** uinput device that
**spoofs a wired Xbox 360 controller**. This is deliberate: SDL2's
`gamecontrollerdb` keys off `045e:028e`, so every emulator/frontend maps it with
zero config.

```
Bus=0003 (BUS_USB)  Vendor=045e  Product=028e  Version=0114
Name="TRIMUI Player1"
Handlers=js0 event4
```

### Buttons (EV_KEY)
`BTN_A BTN_B BTN_X BTN_Y BTN_TL BTN_TR BTN_SELECT BTN_START BTN_MODE BTN_THUMBL BTN_THUMBR`
- No `BTN_DPAD_*` (d-pad is a hat, below)
- No `BTN_TL2/TR2` (triggers are axes, below)
- `BTN_MODE` = the Home/Guide button

### Axes (EV_ABS) — verified live
| axis        | range              | rest | source                    |
|-------------|--------------------|------|---------------------------|
| `ABS_X`     | −32767 .. 32767    | 0    | left stick X              |
| `ABS_Y`     | −32767 .. 32767    | 0    | left stick Y              |
| `ABS_RX`    | −32767 .. 32767    | 0    | right stick X             |
| `ABS_RY`    | −32767 .. 32767    | 0    | right stick Y             |
| `ABS_Z`     | 0 .. 255           | 0    | **L2** (digital: 0 / 255) |
| `ABS_RZ`    | 0 .. 255           | 0    | **R2** (digital: 0 / 255) |
| `ABS_HAT0X` | −1 .. 1            | 0    | d-pad L(−1)/R(+1)         |
| `ABS_HAT0Y` | −1 .. 1            | 0    | d-pad U(−1)/D(+1)         |

- Sticks: stock reports **clean signed-16-bit centred at 0** with `fuzz 0, flat 0`
  — the driver already applies per-unit calibration (center/range) and outputs
  centred values. Our MCUs give 12-bit Hall (0..4095, ~2048 center) → must scale
  to ±32767 and subtract the resting center.
- Triggers are **digital** on this hardware: only 0 or 255 ever emitted.

### Switch (EV_SW)
- `SW_TABLET_MODE` (0/1) = **the Fn / hall switch**, reported *on the pad device*.
  ⚠️ Divergence: our mainline impl currently exposes Fn as **gpio-keys `KEY_FN`
  on PL11** (committed 7ece8ff). Stock uses `SW_TABLET_MODE` on the pad. Decide:
  keep KEY_FN (cleaner mainline) vs. match stock (SW_TABLET_MODE) — or emit both.

### Force feedback (EV_FF)
- `FF_RUMBLE FF_PERIODIC FF_SQUARE FF_TRIANGLE FF_SINE FF_GAIN` — the pad accepts
  rumble and routes it to the `pwm-vibrator`. Not yet in our driver (follow-up).

## Other stock input devices (for parity)
| device            | codes                                                       |
|-------------------|-------------------------------------------------------------|
| `sunxi-keyboard`  | `KEY_VOLUMEDOWN KEY_VOLUMEUP KEY_HOMEPAGE` (LRADC side keys)|
| `axp2202-pek`     | `KEY_POWER`                                                  |
| `audiocodec Headphones` | `KEY_VOLUMEDOWN KEY_VOLUMEUP KEY_MEDIA 0246` + `SW_HEADPHONE_INSERT SW_MICROPHONE_INSERT` |

## Delta: current mainline driver → this goal
`kernel/drivers/gamepad-trimui-smart-pro-s.c` today creates **two** per-MCU
devices with d-pad-as-buttons + digital-trigger-buttons. To hit stock parity:
1. **Merge to one** input_dev; `id = BUS_USB / 0x045e / 0x028e / 0x0114`,
   name `"TRIMUI Player1"`.
2. D-pad bits → `ABS_HAT0X/Y` (−1/0/+1), drop `BTN_DPAD_*`.
3. L2/R2 bits → `ABS_Z/ABS_RZ` (0/255), drop `BTN_TL2/TR2`.
4. Sticks: scale 12-bit Hall → `±32767`, center-subtract (~2048); declare
   `ABS_X/Y/RX/RY` with `input_set_abs_params(... -32767, 32767, 0, 0)`.
5. Fn switch: emit `SW_TABLET_MODE` here (or resolve the KEY_FN divergence).
6. Later: wire `EV_FF`/`FF_RUMBLE` → pwm-vibrator.
