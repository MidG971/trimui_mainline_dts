# Warm-reboot investigation (Trimui Smart Pro S / A523)

**Symptom:** from running mainline Linux, `reboot` / `systemctl reboot` leaves the SoC frozen — the
device does not come back. Cold power-cycle is the only reliable restart. (Cold boot works fully.)

## Confirmed offline (2026-08-30)

- **A523 has no working PSCI reset** — BL31 (jernejsk a523-v4) `SYSTEM_RESET` hangs (known T527/A523
  issue; ut-slayer confirms). So PSCI is not the reboot path.
- **Our reset method = the SoC watchdog, and it's configured correctly:**
  - `drivers/watchdog/sunxi_wdt.c` `sun55i_wdt_reg`: `wdt_reset_mask=0x03`, `wdt_reset_val=0x01`,
    **`restart_priority=200`** (beats PSCI's default 128). Registered via `watchdog_set_restart_priority`.
  - `CONFIG_SUNXI_WATCHDOG=y`; DT `watchdog@2050000` `allwinner,sun55i-a523-wdt` status okay.
  - The driver's own comment notes the **vendor 5.15 kernel also resets via this watchdog** — so the
    method matches stock, which reboots fine.
- **Boot chain = mainline SPL** (`sd-bringup` `sunxi-spl.bin` @128K → U-Boot FIT @256K → kernel).
  DRAM init resolved 2026-08-05 (Cubie A5E params, clamped to the Trimui's 1 GiB). That DRAM training
  has only ever been exercised on a **cold** boot.

## Leading theory

The watchdog fires and resets the SoC, but the **mainline SPL's DRAM re-init hangs on a warm reset**:
after a watchdog reset the external DRAM chip is not in a clean power-on state (it was cold on first
boot), and the SPL's training sequence assumes cold DRAM. The vendor reboots because the *vendor boot0*
handles the warm-reset DRAM case; our mainline SPL does not. Matches the old diagnostic rule
("resets then hangs in SPL = DRAM re-init").
Secondary candidate: the A523 watchdog reset doesn't actually fire (handler `while(1)` spins → frozen)
— less likely since the config matches the working vendor path.

## Code analysis of the SPL DRAM driver (device-independent, 2026-09-17)

Read `u-boot/arch/arm/mach-sunxi/dram_sun55i_a523.c` (the SPL DRAM init) end to end:
- `sunxi_dram_init()` **always full-retrains** — auto-detect rank/width + size, then a full
  `mctl_core_init` — with **no warm/cold detection, no saved-training fast path, no warm-reset
  flag** (no RTC/PRCM scratch read).
- `mctl_sys_init()` resets the **SoC-side** DRAM path (MBUS, DRAM gate/reset via CCU, PLL5, DRAM
  clocks) — controller reset is present.
- There is **no explicit LPDDR4 chip reset (`RESET_n`), self-refresh exit, or JEDEC MRW/PIR**
  anywhere — the driver relies on the chip being in power-on-reset state.

★ **BUT the H616 driver (`dram_sun50i_h616.c`) — same lineage, warm-reboots fine — is
structurally identical** in reset handling (same CCU gate/reset, no chip `RESET_n`, no warm
detection). So there is **no obvious A523-specific driver gap** — this WEAKENS "the DRAM driver
is missing a warm-reset step" as the sole cause.

⇒ Refined direction (needs UART or a power-cycle to resolve):
- (a) the **watchdog reset scope** on A523 may not fully reset the DRAM controller (leaving it
  mid-operation, so `mctl_sys_init`'s re-reset can't recover); or
- (b) an **LPDDR4-specific warm-state** issue (H616's "works" may be DDR3-specific; LPDDR4 has a
  strict `RESET_n` power-up the driver never actively drives).
- Note for A1: the **mainline `axp20x` driver registers only a power-OFF handler, no restart** —
  so the PMIC power-cycle path needs the AXP2202 auto-restart / RTC-wake capability confirmed
  before it can be wired.

## The no-UART discriminator (device online + USB-C cable)

No UART adapter on hand; use the USB-C/FEL observation. Connect USB-C, `reboot` over SSH, watch the port:
- **FEL enumerates (`1f3a:efe8`)** → SoC reset fired, SPL failed → **DRAM warm-reset (Theory A)**.
- **Device returns on WiFi/Tailscale** → warm reboot works now (re-test; may already be fixed).
- **Nothing (stays dead)** → reset never fired → **watchdog reset not firing (Theory B)**.
Also capture, from a live shell: `dmesg | grep -i watchdog` (did `sun55i_wdt` probe + register the
restart handler at prio 200?), `ls /sys/class/watchdog/`, and `cat /sys/class/watchdog/*/{state,identity}`.

## Fix directions

- **Theory A (DRAM warm-reset) — most likely:**
  1. **PMIC power-cycle reboot (best no-UART fix) — CONFIRMED FEASIBLE (2026-09-17).** The AXP717C
     datasheet §6.5.4 / §6.14.2.23 documents a hardware restart: **`REG 0x27` bit [1] = restart**
     ("the system will power off and then power on; VRTC stays up"). That is the *same* register as
     the SOFT_PWROFF poweroff (bit [0], patch 0024) — bit 0 = power off, **bit 1 = power-cycle**.
     A true off→on cycle unpowers DRAM → clean cold training → sidesteps the warm-reset hang, no
     UART needed. **Implement:** add an `axp20x` **restart handler** for the AXP717 that does
     `regmap_set_bits(AXP717_SOFT_PWROFF, BIT(1))`, registered via `devm_register_sys_off_handler`
     (`SYS_OFF_MODE_RESTART`) at a **priority above the watchdog's 200**. The mainline `axp20x`
     driver currently registers only a power-off handler, so this is a new handler (companion to the
     0024 poweroff patch; upstream-worthy on its own). HW-test when the board is back.
  2. **Mainline SPL warm-reset DRAM handling:** reset the DRAM PHY/controller (or issue a DRAM chip
     reset) before re-training in the A523 U-Boot DRAM driver — compare against the vendor boot0's
     warm-reset path. Needs UART on PB9/PB10 (wired on-board; adapter not on hand right now).
- **Theory B (watchdog not firing):** verify the A523 `wdt_cfg` reset encoding (mask 0x03/val 0x01)
  actually triggers a whole-SoC reset on this silicon; check the watchdog probes and the restart
  handler is the one selected.

## Status
**★★★ A1 FIX PROVEN ON HARDWARE (2026-09-22).** On the replacement board, wrote **REG 0x27 bit 1
= 0x06** to the AXP717 (`i2cset -f -y 0 0x34 0x27 0x06`) → the PMIC power-cycled the SoC and the
device **rebooted and came back on its own** (fresh boot, uptime ~28s, back on WiFi/Tailscale, no
manual cold-boot). Confirms the whole theory: watchdog/PSCI restart hangs the SPL on warm DRAM, but
a true PMIC power-off/on unpowers DRAM → clean cold training → device returns. REG 0x27 self-cleared
to 0x04 after (bit1 is one-shot). The DRAM-warm-reset (Theory A) is the cause; the PMIC power-cycle
is the fix. NEXT: make `reboot` use it automatically — the driver handler is written/staged
(`outgoing-7.3-rc3/0001-mfd-axp20x-restart-…`), but axp20x is **built-in** (`CONFIG_MFD_AXP20X=y`)
on the device kernel, so activating it needs a kernel rebuild (batch with next Image build) — or a
userspace stopgap (systemd reboot hook writing 0x27 bit1) for immediate use.

**A1 fix IMPLEMENTED + staged (2026-09-17), HW-test pending — now PROVEN above.** The AXP717 restart handler
(`regmap_set_bits(AXP717_SOFT_PWROFF, BIT(1))` at `SYS_OFF_PRIO_FIRMWARE` — above the watchdog
and PSCI restart handlers) is written, build-tested (`axp20x.o` clean under `W=1`), checkpatch
0/0/0, and staged as
`outgoing-7.3-rc3/0001-mfd-axp20x-restart-the-AXP717-via-SOFT_PWROFF-1.patch`.

**When the replacement board is up:** deploy it and `reboot` over SSH — if the device power-cycles
and comes back on its own, warm-reboot is fixed (and the SPL warm-DRAM question is moot). If it
does NOT, fall back to the USB-C/FEL discriminator above (Theory A vs B) + the DRAM/watchdog angles.

## ★★★ SOLVED 2026-10-07 (userspace, no reflash) — `systemctl reboot` now works
Re-confirmed the PMIC fix on the current kernel (AXP717 @ i2c bus 0, 0x34): setting REG 0x27 bit 1
(read-modify-write `0x0c -> 0x0e`, i.e. `i2cset -f -y 0 0x34 0x27 0x0e`) power-cycles the SoC →
device returned on its own in ~10 s (fresh boot). Then wired it into userspace so the normal reboot
path uses it:
- **`/usr/lib/systemd/system-shutdown/warm-reboot.shutdown`** — systemd runs this at the very end of
  shutdown (after unmount); for `$1 = reboot` it does the `i2cset` power-cycle. **`systemctl reboot`
  TESTED → clean shutdown + PMIC power-cycle + device back in ~20 s on its own.** ✓
- **`/usr/local/bin/warm-reboot`** — direct `sync` + `i2cset` for an immediate power-cycle.
Both live on the SD ROOTFS (persist across reboot). Everything survives the warm-reboot (CPU-CCU DVFS,
the fixed mic codec, WiFi). ⚠ the **display** often comes up in its usual flaky/fractal state after a
reboot — that's the known display boot-to-boot fragility, orthogonal to the reboot (device is fully
functional over SSH regardless); blank it with `echo 4 > /sys/class/graphics/fb0/blank`.
DONE 2026-10-07 (proper/upstream fix): the **axp20x restart handler** (`kernel/patches/0036-mfd-axp20x-restart-the-AXP717-via-SOFT_PWROFF.patch`, `regmap_set_bits(AXP717_SOFT_PWROFF, BIT(1))` at SYS_OFF_PRIO_FIRMWARE) is now **built into the device kernel Image and HW-verified** — with the userspace hook disabled, native `systemctl reboot`/`reboot` power-cycles the PMIC via the handler and the device returns on its own (~20 s). The userspace systemd shutdown hook above was a transitional stopgap and has been removed; the kernel handler is the mechanism now (upstream-worthy, companion to the 0024 SOFT_PWROFF poweroff).
