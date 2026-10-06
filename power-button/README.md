# TrimUI Smart Pro S — power-button behavior (mainline)

Display-free power **gestures** (2026-10-06). Debian rootfs + systemd. HW-verified.

## Behavior
- **Tap** (< 2.5 s)   -> **sleep** (s2idle suspend); tap again to wake.
- **Hold** (>= 2.5 s) -> **power off** (clean `systemctl poweroff`).
- **Hold ~4 s+**      -> AXP PEK **hardware off** (PMIC, `shutdown`=4000ms; supersedes the above).

The 2.5 s threshold is deliberately high so a normal ~1-2 s press always sleeps and never
powers off by accident; a clean poweroff takes a deliberate long hold.

### No on-screen menu, no restart gesture — why
- The original plan was a navigable **on-screen power menu**. Abandoned: the A523 DSI panel
  won't reliably render/establish a custom frame (commit/flip_done deadlock; see
  `docs/DISPLAY-*`). Gestures give the same power actions with zero display dependency.
- **Restart is intentionally omitted**: `systemctl reboot` uses the warm-reboot path, which is
  broken on this board (it tears down then hangs, needing a manual power-cycle). Re-add a
  double-tap -> reboot once warm reboot works.

## Files (deployed paths)
- `/usr/local/bin/power-btn-daemon.py` — reads the AXP PEK (found BY NAME, not a fixed event
  number), measures press duration: `< 2.5 s` -> `systemctl suspend`, `>= 2.5 s` ->
  `systemctl poweroff`. Ignores the next release after suspend (the wake press) so it never loops.
- `/etc/systemd/system/power-btn.service` — enabled on boot (`After=local-fs.target` so it
  starts early, not after multi-user.target, closing the boot-time dead-button window).
- `/usr/lib/systemd/system-sleep/50-display-blank` — on suspend blanks fb0 (screen off + DSI
  PHY gated = cool) AND turns the RGB indicator ring off (saves + restores brightness on resume).

## Notes / gotchas
- s2idle only (`mem`->[s2idle]; deep suspend not available on A523 mainline yet).
- PEK is a wake source (`power/wakeup=enabled`); wake needs a firm-ish press.
- logind already ignores the power key (`/etc/systemd/logind.conf.d/10-no-powerkey.conf`).
- Input event numbers shuffle between boots — daemon finds axp20x-pek by name.
- `systemctl suspend` hands back around resume -> wake guard is release-based (ignore next
  release), not timed.
- LED ring restores ~10 s after wake (resume path settling) — cosmetic.
