<!-- SPDX-License-Identifier: (GPL-2.0-only OR MIT) -->
<!-- Copyright (C) 2026 Midgy BALON -->

# AIC8800 WiFi + Bluetooth (out-of-tree)

The Trimui Smart Pro S uses an **AICSemi AIC8800** (SDIO WiFi on `mmc1`, UART
Bluetooth on `uart1`). It has **no mainline driver**, so WiFi/BT come from the
vendor BSP built as out-of-tree modules. The popular community repos are
WiFi-only; the piece they drop — **Bluetooth** — is included here.

## What's here

| File | |
|---|---|
| [`build-aic8800.sh`](build-aic8800.sh) | Fetches the pinned BSP, applies the port, builds the 3 modules |
| [`aic8800-7.2.patch`](aic8800-7.2.patch) | Our port of the vendor driver to mainline v6.15+/v7.x |
| [`aic_compat72.h`](aic_compat72.h) | Small force-included compat shim (version-guarded) |

**Not here (by policy):** the RF firmware and the `.ko` binaries. Extract the
firmware from your device's own stock rootfs (`/lib/firmware/aic8800d80/`,
`/lib/firmware/aic8800dc/`).

## Source

Pinned to **radxa-pkg/aic8800** `bd11969` — SDK **V5.0** (2026-01-23), driver
`fdrv` **6.4.3.0**, which is the exact version shipped in the Trimui stock
firmware. Built from its `src/SDIO/driver_fw/driver/aic8800/` tree, which uniquely
carries all three modules — `aic8800_bsp`, `aic8800_fdrv` (WiFi) and
`aic8800_btlpm` (BT) — plus the `libbt-vendor` userspace with the `aic` HCI type.

## Build

```sh
# Kernel tree must be configured/built with CFG80211 + BT (build-trimui-kernel.sh does this).
./build-aic8800.sh /path/to/linux-v7.2 [work-dir]
```
Produces `aic8800_bsp.ko`, `aic8800_fdrv.ko`, `aic8800_btlpm.ko`. Verified building
**clean on v7.2-rc1** (0 errors), vermagic `7.2.0-rc1`.

## What the port does

The vendor driver targets ~5.15 and predates several kernel changes:
- **`aic8800-7.2.patch`** — `of_gpio.h`→`gpio/consumer.h`; add `<vmalloc.h>`;
  `MODULE_IMPORT_NS()`→string literal (6.13+); the v7.2 **`cfg80211_ops`
  migration** (keys/stations `net_device`→`wireless_dev`; `radio_idx`/`link_id`
  added to tx-power etc.; internal callers pass `->ieee80211_ptr`); the TDLS
  `ieee80211_mgmt` action-union change.
- **`aic_compat72.h`** (force-included) — mechanical renames: `del_timer`→
  `timer_delete`, `from_timer`→`timer_container_of`, `in_irq`→`in_hardirq`,
  `wakeup_source_*`→`register/unregister`, `strncpy`→`strscpy`.

## Runtime (on the device)

1. Firmware → `/lib/firmware/aic8800d80/` and `/aic8800dc/` (both variants ship;
   the driver picks the one that probes — D80 vs DC is HW-gated).
2. Load order:
   ```sh
   modprobe aic8800_bsp      # base/bus (auto-loaded as a dependency)
   modprobe aic8800_fdrv     # WiFi (SDIO on &mmc1; board DTS has wifi_pwrseq)
   modprobe aic8800_btlpm    # BT power/sleep helper
   ```
3. Bluetooth HCI over UART (`&uart1`, PG6-9 + RTS/CTS):
   ```sh
   hciattach -n ttyS1 aic    # vendor hciattach fork with the "aic" type
   ```
   Then `hciconfig hci0 up` / `bluetoothd`. Alternatively, test whether mainline's
   generic `hci_uart`/`btattach` can drive it once firmware is loaded (`btlpm` is
   mostly BT_WAKE/HOST_WAKE power management) — the cleaner, upstreamable route.

## Status / caveats

- Builds clean on v7.2-rc1; **not yet silicon-verified** (no device in hand).
- `.ko` are kernel-version locked — rebuild against whatever kernel you ship.
- Re-pin + re-test `aic8800-7.2.patch` if the BSP or kernel baseline moves.
