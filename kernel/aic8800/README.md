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
| [`build-aic8800.sh`](build-aic8800.sh) | Fetches the pinned source, applies the delta, builds the 3 modules (`make M=`) |
| [`aic8800-warpme-v7.2.patch`](aic8800-warpme-v7.2.patch) | Our tiny v7.1→v7.2 delta on top of the pinned source |

**Not here (by policy):** the RF firmware and the `.ko` binaries. Extract the
firmware from your device's own stock rootfs (`/lib/firmware/aic8800d80/`,
`/lib/firmware/aic8800dc/`).

## Source

Pinned to **warpme/minimyth2** `97b9429b` — its in-tree AIC8800 SDIO backport,
snapshot **v2025_0926**, driver `fdrv` **6.4.3.0** (the exact version shipped in
the Trimui stock firmware, so it stays compatible with the device's firmware
blobs). It carries all three modules — `aic8800_bsp`, `aic8800_fdrv` (WiFi) and
`aic8800_btlpm` (BT) — and, unlike older snapshots, already includes the
modern-kernel fixups (it targets linux-7.1). We build it **out-of-tree** (`make M=`)
so nothing is patched into your kernel tree.

> Earlier this used radxa-pkg/aic8800 (`fdrv` 6.4.3.0, snapshot 2025_0225) with a
> larger 5.15→7.x port + compat shim. warpme's snapshot is newer, on the same fdrv
> line, and already 7.1-fixed, so the port shrank to the three-line delta below.
> See `docs/`/the tracking notes for how to check warpme for newer snapshots.

## Build

```sh
# Kernel tree must be configured/built with CFG80211 + BT (build-trimui-kernel.sh does this).
./build-aic8800.sh /path/to/linux-v7.2 [work-dir]
```
Produces `aic8800_bsp.ko`, `aic8800_fdrv.ko`, `aic8800_btlpm.ko`. Verified building
**clean on v7.2-rc1** (0 errors, modpost clean), vermagic `7.2.0-rc1`.

## What the delta does

warpme's snapshot is already fixed for linux-7.1, so only the residual v7.1→v7.2
gap remains. `aic8800-warpme-v7.2.patch` (3 hunks):

- **`rwnx_platform.c`** — `strncpy()` was removed in v7.2; the call copies an exact
  computed length and NUL-terminates on the next line, so it becomes `memcpy()`.
- **`rwnx_msg_tx.c`** — `strncpy()` into a fixed-size buffer becomes `strscpy()`.
- **`rwnx_main.c`** — the v7.2 `cfg80211_ops.remain_on_channel` gained a trailing
  `const u8 *rx_addr`; add it to the callback (the internal helper ignores it).

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
   hciattach -n ttyS1 aic    # vendor hciattach "aic" type (vendor ttyAS1 = mainline ttyS1)
   ```
   Then `hciconfig hci0 up` / `bluetoothd`. Alternatively, test whether mainline's
   generic `hci_uart`/`btattach` can drive it once firmware is loaded (`btlpm` is
   mostly BT_WAKE/HOST_WAKE power management) — the cleaner, upstreamable route.

## Status / caveats

- Builds clean on v7.2-rc1; **not yet silicon-verified** (no device in hand).
- `.ko` are kernel-version locked — rebuild against whatever kernel you ship.
- Re-pin (`MM2_PIN` in the script) + re-test the delta if the source or kernel
  baseline moves. warpme also ships a USB backport (`3402-…`) — not needed here
  (the Trimui uses SDIO), but available if a USB AIC8800 ever matters.
