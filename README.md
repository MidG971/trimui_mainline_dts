# Allwinner T527 / Trimui Smart Pro - Mainline Device Tree (21 KB Unification)

This repository contains a fully unified, cleaned, and production-ready Mainline Device Tree (`.dts`) for the **Trimui Smart Pro** retro-gaming handheld, based on the Allwinner T527 SoC. 

The tree successfully compiles with `dtc` against modern mainline inclusion targets without syntax errors or broken phandles.

## 🛠️ Hardware Support Matrix (DTS Status)

| Component | Status | Mainline Driver / Node |
| :--- | :---: | :--- |
| **8x Cortex-A55 Cores** | 🟢 OK | DVFS Dynamic Scaling tied to AXP2202 `dcdc1` |
| **AXP2202 PMIC** | 🟢 OK | Regulators (`cldo`, `aldo`, `bldo`) un-hardcoded |
| **5000 mAh Battery** | 🟢 OK | Custom fuel-gauge voltage/capacity discharge curves |
| **720x1280 MIPI-DSI Display**| 🟢 OK | TCON1 layout + Native panel-init-sequence injected |
| **PWM Backlight** | 🟢 OK | `pwm-backlight` operational |
| **Audio Codec** | 🟢 OK | Core ALSA `simple-audio-card` routing (SPK & HP Out) |
| **Gamepad Buttons** | 🟢 OK | Linux `gpio-keys` EVDEV mapping |
| **Analog Joysticks** | 🟢 OK | GPADC 12-bit channel calibration |
| **Wi-Fi / Bluetooth** | 🟢 OK | BRCM SDIO Wi-Fi + Mainline BCM Bluetooth on `uart1` (LPM) |
| **Vibrator Motor** | 🟢 OK | `pwm-vibrator` on PWM Channel 7 (Inverted) |
| **Internal Cooling Fan** | 🟢 OK | 32-step `pwm-fan` on PWM Channel 10 (Inverted) |
| **USB Host / OTG** | 🟢 OK | Dual VBUS controlled via GPIO (`PD8` / `PE6`) |

## 🚀 How to Compile

Simply run the included compilation script to generate the production binary:

```bash
./compile.sh
