#!/bin/sh
# SPDX-License-Identifier: (GPL-2.0-only OR MIT)
# Copyright (C) 2026 Midgy BALON
# ============================================================================
# Trimui Smart Pro S (TG5050 / Allwinner A523) — interactive HW verification
# ----------------------------------------------------------------------------
# The INTERACTIVE companion to recon.sh. recon.sh is a read-only passive dump;
# this script *guides* you through actively exercising each subsystem (press a
# button, move a stick, plug USB, listen for a tone...), captures the results,
# and writes a Markdown report suitable as "tested-on-hardware" evidence for the
# mainline submission (per feature: what was tested, the exact commands, the raw
# output, the measured value, the DT property it feeds, and a PASS/FAIL/SKIP
# verdict), ending with a summary table + a device-tree calibration block.
#
# Run it on the device (mainline rootfs preferred; degrades on the stock OS):
#     sh hw-verify.sh            # interactive menu (pick tests, re-run, skip)
#     sh hw-verify.sh --all      # run every on-device subsystem test in order
#     sh hw-verify.sh pmic lradc # run just those tests
#     sh hw-verify.sh --bringup  # the pre-rootfs phases (vendorboot/recon/backup/fel/sdboot)
#     sh hw-verify.sh --demo     # no-device skeleton (everything PENDING)
#     sh hw-verify.sh --list     # list test ids
#     sh hw-verify.sh --help
#
# Two groups of steps share one report format:
#   * BRING-UP phases (vendorboot, recon, backup, fel, sdboot) cover the pre-rootfs runbook
#     steps from docs/HARDWARE-BRINGUP.md (run from the HOST / stock OS). The
#     risky ones (eMMC backup dd, FEL, SD write) are GUIDED ONLY: the script
#     prints the exact command to run by hand, loudly labels the risk, requires
#     a typed acknowledgement, and records the result — it NEVER executes a dd,
#     enters FEL, or writes/partitions any storage itself.
#   * SUBSYSTEM tests (identity..cpufreq) assume an already-booted mainline
#     rootfs and actively exercise each block (press a button, move a stick...).
#
# POSIX sh / busybox-ash safe, read-mostly. The only writes are optional and
# explicit (backlight/LED/fan/vibrator sweeps, and only after confirming the
# sysfs node is writable); each restores the original value. It never flashes or
# partitions. If a tool is missing it prints how to get it and SKIPs; if stdin is
# not a TTY it auto-skips every prompt so piping/CI can never wedge it.
#
# Cross-references: recon.sh, dts/sun55i-a523-trimui-smart-pro-s.dts,
# docs/HARDWARE-BRINGUP.md (Phases 1-7), docs/UPSTREAMING.md, FIRMWARE-FINDINGS.md.
# ============================================================================

SELF=hw-verify.sh
MAXLINES=40

# ---- mode / tty detection --------------------------------------------------
DEMO=0
INTERACTIVE=1
[ -t 0 ] || INTERACTIVE=0

# On-device subsystem tests (assume a booted mainline rootfs).
TEST_IDS="identity storage pmic lradc gamepad sticks display audio leds vibrator wifi bluetooth usb battery rtc thermal cpufreq"
# Pre-rootfs bring-up phases (run from the host / stock OS; guided, mostly RO).
BRINGUP_IDS="vendorboot recon backup fel sdboot"
# Full ordered set (bring-up first, chronologically) for the report + menus.
ALL_IDS="$BRINGUP_IDS $TEST_IDS"

# ---- primitives (echo recon.sh's helper style) -----------------------------
sec()  { printf '\n\n========== %s ==========\n' "$1"; }
say()  { printf '%s\n' "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }

need() {
	# need TOOL "install hint" -> 0 if present, else print hint + return 1.
	# In demo mode always succeed so the no-device skeleton shows the full test
	# body (commands + calibration) even on a host that lacks the on-device tool.
	have "$1" && return 0
	[ "$DEMO" = 1 ] && return 0
	say "  [tool missing] '$1' not found — $2"
	return 1
}

danger() {
	# danger ACKWORD "one-line risk description" -> gate a risky bring-up step.
	# The script NEVER runs the dangerous command itself; this only decides
	# whether to prompt for the step's result. Prints a loud, labelled warning
	# (also into the report), then:
	#   demo / non-TTY / non-interactive -> auto-skip (return 1), never armed;
	#   interactive -> require typing ACKWORD exactly to arm (return 0).
	printf '\n  %s\n' "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
	printf '  !! RISKY STEP: %s\n' "$2"
	printf '  !! This script will NOT run it — it only shows the exact command to\n'
	printf '  !! run BY HAND and records the outcome. It never dds, enters FEL, or\n'
	printf '  !! writes/partitions any storage. Read the command before you run it.\n'
	printf '  %s\n' "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
	{ printf '\n> RISKY STEP (guided, NOT executed by this script): %s\n' "$2"; } >> "$TESTBUF"
	if [ "$DEMO" = 1 ] || [ "$INTERACTIVE" != 1 ]; then
		say "  (auto-skip: demo / no TTY — step documented above, not armed)"
		return 1
	fi
	printf '  Type %s to acknowledge the risk and record a result (else skip): ' "$1"
	read -r _ack || return 1
	[ "$_ack" = "$1" ] && return 0
	say "  (skipped — acknowledgement not given)"
	return 1
}

pause() {
	# Wait for Enter, but only when interactively driving real hardware.
	[ "$INTERACTIVE" = 1 ] && [ "$DEMO" != 1 ] || return 0
	printf '%s' "${1:-  [Press Enter to continue] }"
	read -r _ || true
}

verdict() {
	# verdict DEFAULT -> resolve the PASS/FAIL/SKIP for the current test.
	# Demo => always PENDING. Non-interactive => the default the caller computed.
	if [ "$DEMO" = 1 ]; then echo PENDING; return; fi
	if [ "$INTERACTIVE" != 1 ]; then echo "${1:-SKIP}"; return; fi
	# Prompt to stderr: this runs inside $(...), so only the resolved verdict may
	# reach stdout (else the prompt text pollutes the captured value).
	printf '  Verdict [P]ass/[F]ail/[S]kip (default %s): ' "${1:-SKIP}" >&2
	read -r _v || { echo "${1:-SKIP}"; return; }
	case "$_v" in
		p|P|pass|PASS) echo PASS ;;
		f|F|fail|FAIL) echo FAIL ;;
		s|S|skip|SKIP) echo SKIP ;;
		*)             echo "${1:-SKIP}" ;;
	esac
}

askval() {
	# askval "prompt" "default" -> echo the user's typed value (or the default)
	if [ "$INTERACTIVE" != 1 ] || [ "$DEMO" = 1 ]; then echo "$2"; return; fi
	# Prompt to stderr: askval runs inside $(...), so stdout must carry only the
	# typed value (a stdout prompt would be captured into the result).
	printf '  %s [%s]: ' "$1" "$2" >&2
	read -r _x || { echo "$2"; return; }
	if [ -n "$_x" ]; then echo "$_x"; else echo "$2"; fi
}

# ---- per-test report buffering ---------------------------------------------
begin_test() {
	# begin_test ID "what is tested (one line)"
	CUR_ID=$1
	WHATIS=$2
	: > "$TESTBUF"
	sec "$(test_title "$1")"
	say "$2"
}

cap() {
	# cap "shell command" — run it (unless demo), echo output, append a fenced
	# code block (command + trimmed raw output) to the current test buffer.
	printf '\n$ %s\n' "$1"
	{ printf '```\n$ %s\n' "$1"; } >> "$TESTBUF"
	if [ "$DEMO" = 1 ]; then
		printf '  (demo mode — command not executed)\n'
		printf '(demo mode — command not executed)\n' >> "$TESTBUF"
	else
		_out=$(eval "$1" 2>&1)
		printf '%s\n' "$_out"
		_n=$(printf '%s\n' "$_out" | wc -l | tr -d ' ')
		if [ "${_n:-0}" -gt "$MAXLINES" ]; then
			printf '%s\n' "$_out" | head -n "$MAXLINES" >> "$TESTBUF"
			printf '... (%s lines total; trimmed to %s in report)\n' "$_n" "$MAXLINES" >> "$TESTBUF"
		else
			printf '%s\n' "$_out" >> "$TESTBUF"
		fi
	fi
	printf '```\n' >> "$TESTBUF"
}

manual() {
	# manual "label" "command to run by hand" — record but do not execute.
	say "  >> run by hand: $2"
	{ printf '%s (run by hand — not executed here):\n```\n%s\n```\n' "$1" "$2"; } >> "$TESTBUF"
}

calib() { printf '%s\n' "$1" >> "$CALIB"; }

finish() {
	# finish VERDICT "measured value" "DT artifact it feeds" "notes"
	{
		printf '\n### %s — %s\n\n' "$CUR_ID" "$(test_title "$CUR_ID")"
		printf '**What was tested:** %s\n\n' "$WHATIS"
		printf '**Verdict:** %s  \n' "$1"
		printf '**Measured / verified value:** %s  \n' "${2:-—}"
		printf '**Feeds (DT property / mainline artifact):** %s\n\n' "$3"
		printf '**Commands run and raw output:**\n\n'
		cat "$TESTBUF"
		printf '\n**Notes:** %s\n' "${4:-—}"
	} >> "$REPORT"
	# summary line, deduped by test id (a re-run replaces the earlier verdict)
	if [ -f "$SUMMARY" ]; then
		grep -v "^$CUR_ID	" "$SUMMARY" > "$SUMMARY.tmp" 2>/dev/null || true
		mv "$SUMMARY.tmp" "$SUMMARY" 2>/dev/null || true
	fi
	printf '%s\t%s\t%s\t%s\n' "$CUR_ID" "$1" "$(test_title "$CUR_ID")" "${2:-}" >> "$SUMMARY"
	say ""
	say "  -> $CUR_ID: $1  (${2:-no value})"
}

# ---- small shared helpers --------------------------------------------------
i2c_buses() { i2cdetect -l 2>/dev/null | sed -n 's/^i2c-\([0-9][0-9]*\).*/\1/p'; }

find_event() {
	# find_event "name-regex" -> print the eventN handler of the first matching
	# /proc/bus/input/devices block (case-insensitive on the N: Name= line).
	awk -v pat="$1" '
		/^N: Name=/    { name=tolower($0) }
		/^H: Handlers=/{
			if (name ~ tolower(pat)) {
				m=split($0,a,/[ =\t]/)
				for (i=1;i<=m;i++) if (a[i] ~ /^event[0-9]+$/) { print a[i]; exit }
			}
		}' /proc/bus/input/devices 2>/dev/null
}

# ============================================================================
# TESTS
# ============================================================================

test_title() {
	case "$1" in
		vendorboot) echo "Phase 0 — fresh vendor boot log (golden reference)" ;;
		recon)     echo "Phase 1 — stock-OS recon (recon.sh + live DTB)" ;;
		backup)    echo "Phase 2 — eMMC backup (brick-insurance)" ;;
		fel)       echo "Phase 3 — FEL RAM-boot U-Boot (DRAM retarget)" ;;
		sdboot)    echo "Phase 4 — first mainline boot from microSD" ;;
		identity)  echo "Identity, kernel & live DTB" ;;
		storage)   echo "Storage (microSD mmc0 / eMMC mmc2 + partitions)" ;;
		pmic)      echo "PMIC identity (AXP717C vs AXP2202) + CPU regulator" ;;
		lradc)     echo "LRADC side keys (Home / Vol+ / Vol-)" ;;
		gamepad)   echo "Gamepad / buttons (D-pad, ABXY, L/R) source" ;;
		sticks)    echo "Analog sticks (GPADC) calibration" ;;
		display)   echo "Display (DRM connector) + backlight sweep" ;;
		audio)     echo "Audio codec (speaker / mic / headphone jack)" ;;
		leds)      echo "LEDC RGB array (17 LEDs, colour order)" ;;
		vibrator)  echo "Vibrator (pwm-vibrator, pwm0 ch7)" ;;
		wifi)      echo "WiFi (AIC8800 SDIO on mmc1)" ;;
		bluetooth) echo "Bluetooth (AIC8800 UART / hci0)" ;;
		usb)       echo "USB host / gadget / USB-C PD + DP alt-mode" ;;
		battery)   echo "Battery / charger" ;;
		rtc)       echo "RTC (sun55i-a523 rtc@7090000)" ;;
		thermal)   echo "Thermal zones + PWM fan" ;;
		cpufreq)   echo "CPU frequency / OPP (little + big cluster)" ;;
		*)         echo "$1" ;;
	esac
}

# ============================================================================
# PRE-ROOTFS BRING-UP PHASES (host / stock OS; guided, mostly read-only).
# These mirror docs/HARDWARE-BRINGUP.md Phases 1-4. The destructive-looking
# ones are GUIDED ONLY (see danger()): the script prints the command to run by
# hand and records the outcome; it never dds, enters FEL, or writes storage.
# ============================================================================

# ----------------------------------------------------------------------------
t_vendorboot() {
	begin_test vendorboot "Phase 0: capture a FULL kernel log from a COLD boot of the STOCK firmware — the golden reference the mainline boot is diffed against (driver probe order, regulator/clock/PMIC init, input-device registration, thermal/cpufreq bring-up). READ-ONLY."
	say "  Do this on a FRESH power-on: the earliest boot lines are the most useful and"
	say "  the first to be lost once the kernel ring buffer wraps. Two capture paths —"
	say "  serial gets vendor U-Boot + the whole kernel boot; adb gets the kernel log"
	say "  without opening the case. Grab both if you can."
	if have adb; then cap "adb devices"; else say "  (adb absent — install android-tools-adb / platform-tools for the adb path)"; fi
	say ""
	say "  [A] Serial (best — also captures vendor U-Boot). 3.3V UART on ttyS0"
	say "      (PB9 TX / PB10 RX, 115200 8N1). Start logging BEFORE you power on:"
	manual "serial cold-boot capture" "picocom -b 115200 /dev/ttyUSB0 --logfile vendor-serial-boot-\$(date +%F).log   # then cold-power the device"
	say "  [B] adb (no disassembly). Cold-boot the stock OS, then grab dmesg promptly"
	say "      (on a fresh boot the ring buffer still holds the whole kernel log):"
	manual "adb fresh-boot dmesg" "adb wait-for-device && adb shell dmesg > vendor-dmesg-boot-\$(date +%F).log"
	say "  [C] Recover a PREVIOUS boot's log from persistent store, if the vendor kept one:"
	manual "pstore / last_kmsg" "adb shell 'ls -l /sys/fs/pstore/ 2>/dev/null; cat /proc/last_kmsg 2>/dev/null' | tee vendor-lastkmsg-\$(date +%F).log"
	say ""
	say "  Then mine the log for the facts the port needs (probe order + bind points):"
	manual "extract highlights" "grep -niE 'axp|regulator|dsi|tcon|disp|panel|aic|mmc|sdio|pwm|gpadc|lradc|input:|thermal|cpufreq|[Rr]egistered' vendor-*-boot-*.log"
	_ok=$(askval "Captured a full fresh-boot log? (yes/serial/adb/no)" "yes")
	calib "Phase-0 vendor boot: keep vendor-serial-boot-<date>.log + vendor-dmesg-boot-<date>.log as the GOLDEN reference; diff the mainline boot against it (vendor driver probe order / bind points)"
	_dflt=SKIP; [ "$INTERACTIVE" = 1 ] && [ "$_ok" != no ] && _dflt=PASS
	finish "$(verdict "$_dflt")" "fresh vendor boot log = ${_ok}" \
		"golden vendor boot reference (diff target for the mainline boot; confirms driver bind order + which subsystems the vendor kernel brings up)" \
		"Capture on a COLD boot before the ring buffer wraps. Serial adds vendor U-Boot; adb dmesg gets the kernel log without opening the case. Pairs with Phase-1 recon (state) + live.dtb (topology)."
}

# ----------------------------------------------------------------------------
t_recon() {
	begin_test recon "Phase 1: guided READ-ONLY recon on the STOCK OS — run recon.sh over adb and pull the live DTB. Resolves the HW-gated unknowns (PMIC id, AIC8800 variant, gamepad source, partition map)."
	say "  Runs on the HOST over adb (stock OS; adbd autostarts). Fully read-only —"
	say "  recon.sh flashes/writes nothing. This step just guides + records it."
	if have adb; then cap "adb devices"; else say "  (adb absent on this host — install android-tools-adb / platform-tools)"; fi
	if [ -f recon.sh ]; then say "  recon.sh found in $(pwd) — push and run it:"; else say "  recon.sh not in CWD; run these from the repo root."; fi
	manual "run the passive recon dump" "adb push recon.sh /tmp/ && adb shell sh /tmp/recon.sh | tee recon-\$(date +%F).log"
	manual "pull the live DTB (for offline DE work + diffing)" "adb pull /sys/firmware/fdt live.dtb && dtc -I dtb -O dts live.dtb -o live.dts"
	_ok=$(askval "recon.sh completed and live.dtb pulled OK? (yes/no)" "yes")
	calib "Phase-1 recon: keep recon-<date>.log + live.dtb — they feed the pmic/wifi/gamepad/storage verdicts and the Phase-6 DE DT assembly"
	_dflt=SKIP; [ "$INTERACTIVE" = 1 ] && [ "$_ok" = yes ] && _dflt=PASS
	finish "$(verdict "$_dflt")" "recon + live.dtb = ${_ok}" \
		"Phase-1 recon log + live.dtb (input to pmic/wifi/gamepad/storage + Phase-6 DE assembly)" \
		"recon.sh is the read-only collector; see docs/HARDWARE-BRINGUP.md Phase 1. Do the backup (Phase 2) before writing anything."
}

# ----------------------------------------------------------------------------
t_backup() {
	begin_test backup "Phase 2: full eMMC image backup (brick-insurance) BEFORE anything is written. READS the eMMC to a host file; it does not modify the device."
	say "  Runs on the HOST over adb (stock OS, root). On the STOCK OS the eMMC is"
	say "  usually /dev/block/mmcblk0 and the microSD is mmcblk1 — CONFIRM before dd"
	say "  (under mainline the eMMC is mmc2; the index differs by kernel)."
	manual "confirm which device is the eMMC first" "adb shell cat /proc/partitions   # eMMC = the large internal one (~mmcblk0 on stock)"
	manual "image the whole eMMC to a host file" "adb shell 'dd if=/dev/block/mmcblk0 bs=8M' > emmc-full-backup.img"
	manual "checksum the image" "sha256sum emmc-full-backup.img | tee emmc-full-backup.img.sha256"
	if danger BACKUP "images the whole eMMC to emmc-full-backup.img (a host-side READ; the device is not modified)"; then
		_sz=$(askval "Backup image size (e.g. 8G / bytes), blank if not done" "")
		_ok=$(askval "Image created AND sha256 saved? (yes/no)" "yes")
	else
		_sz=""; _ok="skipped"
	fi
	calib "recovery: emmc-full-backup.img (+ .sha256) + stock firmware trimui_tg5050_20251218_v1.0.1 kept safe; /proc/partitions map -> storage test + boot config"
	_dflt=SKIP; [ "$_ok" = yes ] && _dflt=PASS
	finish "$(verdict "$_dflt")" "backup = ${_ok}; size = ${_sz:-?}" \
		"brick-insurance eMMC image (recovery path); partition map feeds the storage test + boot config" \
		"Keep the image and the stock firmware archive. This is the recovery path if a later write goes wrong."
}

# ----------------------------------------------------------------------------
t_fel() {
	begin_test fel "Phase 3: brick-safe FEL RAM-boot of our U-Boot to validate the DRAM retarget. FEL is RAM-only — it touches NO storage, so it cannot brick."
	say "  Runs on the HOST. Serial console on ttyS0 (UART0 = PB9 TX / PB10 RX,"
	say "  3.3 V, 115200 8N1). Watch it for the DRAM training + U-Boot prompt."
	if have sunxi-fel; then cap "sunxi-fel version"; else say "  (sunxi-fel absent — install sunxi-tools; it must be A523/sun55iw3-capable)"; fi
	if ls uboot/*.bin >/dev/null 2>&1; then cap "ls -l uboot/*.bin"; else say "  (no FEL image yet — build uboot/u-boot-sunxi-with-spl-trimui.bin; see uboot/README.md)"; fi
	manual "open the serial console" "picocom -b 115200 /dev/ttyUSB0"
	manual "enter FEL (RAM loader)" "adb reboot efex     # or hold the A523 FEL button combo at power-on"
	manual "confirm the SoC over FEL" "sunxi-fel version   # must report A523 / sun55iw3"
	manual "RAM-boot our U-Boot (no storage write)" "sunxi-fel -v uboot uboot/u-boot-sunxi-with-spl-trimui.bin"
	if danger FEL "RAM-boots our U-Boot over USB to validate the DRAM params (RAM-only; no storage write)"; then
		_ver=$(askval "sunxi-fel version string (expect sun55iw3 / A523)" "")
		_dram=$(askval "DRAM trained + U-Boot prompt reached on serial? (yes/no)" "yes")
	else
		_ver=""; _dram="skipped"
	fi
	calib "U-Boot DRAM retarget: if DRAM init hangs, tweak tpr2/tpr6/tpr10/tpr11/tpr12 in uboot/trimui-tg5050_defconfig; DRAM rail = reg_dcdc3 (vdd-dram 1.10 V) = CONFIG_AXP_DCDC3_VOLT=1100"
	_dflt=SKIP; [ "$_dram" = yes ] && _dflt=PASS
	finish "$(verdict "$_dflt")" "sunxi-fel = ${_ver:-?}; DRAM+U-Boot = ${_dram}" \
		"U-Boot DRAM retarget (uboot/trimui-tg5050_defconfig tpr2/6/10/11/12); reg_dcdc3 vdd-dram 1.10 V" \
		"FEL is RAM-only and cannot brick. If DRAM fails: uboot/DRAM-PARAMS.md + uboot/DRAM-VALIDATION.md, rebuild, retry."
}

# ----------------------------------------------------------------------------
t_sdboot() {
	begin_test sdboot "Phase 4: first mainline boot from microSD (never eMMC). The microSD is written on the HOST — the device eMMC is never touched."
	say "  Build on compiler-rock3b (kernel/build-trimui-kernel.sh) -> Image +"
	say "  sun55i-a523-trimui-smart-pro-s.dtb + modules, then write a boot microSD."
	say "  WRITE THE SD, NOT YOUR HOST DISK: check lsblk and pick the right /dev/sdX."
	manual "identify the SD on the host (before writing)" "lsblk   # confirm the microSD node; NOT your system disk"
	manual "kernel cmdline for extlinux/boot.scr" "console=ttyS0,115200 root=/dev/mmcblk0p2 rw   # SD enumerates as mmc0"
	manual "boot from SD in U-Boot" "load mmc 0:1 <addr> Image; load mmc 0:1 <addr> <dtb>; booti ..."
	if danger SDCARD "you will PARTITION + WRITE a microSD on the host (choose the SD node carefully; never the device eMMC)"; then
		_con=$(askval "Console to a shell over ttyS0? (yes/no)" "yes")
		_map=$(askval "SD came up as mmc0 and eMMC as mmc2 (dmesg | grep mmc)? (yes/no)" "yes")
	else
		_con="skipped"; _map="skipped"
	fi
	calib "boot: console=ttyS0,115200 root=/dev/mmcblk0p2 (SD=&mmc0); dtb=sun55i-a523-trimui-smart-pro-s.dtb; eMMC=&mmc2 (leave untouched until proven)"
	_dflt=SKIP; [ "$_con" = yes ] && _dflt=PASS
	finish "$(verdict "$_dflt")" "console = ${_con}; mmc-map(SD=mmc0,eMMC=mmc2) = ${_map}" \
		"first microSD boot; console + root= cmdline; &mmc0/&mmc2 index mapping (feeds the storage test)" \
		"Boot the SD until proven; keep eMMC pristine. DTS aliases: mmc0=microSD, mmc1=WiFi SDIO, mmc2=eMMC."
}

# ----------------------------------------------------------------------------
t_identity() {
	begin_test identity "Snapshot kernel/model/compatible and pull the live DTB (feeds the board compatible + the DE3.5 DT assembly in Phase 6)."
	cap "uname -a"
	cap "cat /proc/version 2>/dev/null"
	cap "cat /proc/device-tree/model 2>/dev/null | tr -d '\\000'; echo"
	cap "cat /proc/device-tree/compatible 2>/dev/null | tr '\\000' ' '; echo"
	cap "cat /proc/cmdline 2>/dev/null"
	cap "cp /sys/firmware/fdt /tmp/live.dtb 2>/dev/null && echo 'saved /tmp/live.dtb (pull it for offline DE DT work)' || echo 'no /sys/firmware/fdt'"
	_model=""
	[ "$DEMO" != 1 ] && _model=$(cat /proc/device-tree/model 2>/dev/null | tr -d '\000')
	_v=$(verdict PASS)
	finish "$_v" "${_model:-see raw output}" \
		"model/compatible in dts/sun55i-a523-trimui-smart-pro-s.dts; /tmp/live.dtb for Phase 6 DE assembly" \
		"Expected model 'Trimui Smart Pro S', compatible 'trimui,smart-pro-s'."
}

# ----------------------------------------------------------------------------
t_storage() {
	begin_test storage "Enumerate the MMC devices and confirm the runbook mapping (microSD = mmc0, WiFi SDIO = mmc1, eMMC = mmc2) plus the partition map. READ-ONLY."
	cap "for h in /sys/class/mmc_host/mmc*; do [ -e \"\$h\" ] && printf '%s -> %s\\n' \"\$h\" \"\$(readlink -f \$h 2>/dev/null)\"; done"
	cap "for b in /sys/block/mmcblk*; do [ -e \"\$b\" ] && printf '%s size=%s type=%s\\n' \"\$b\" \"\$(cat \$b/size 2>/dev/null)\" \"\$(cat \$b/device/type 2>/dev/null)\"; done"
	cap "cat /proc/partitions 2>/dev/null"
	have lsblk && cap "lsblk -o NAME,SIZE,TYPE,MOUNTPOINT 2>/dev/null"
	cap "ls -l /dev/disk/by-name 2>/dev/null || ls -l /dev/mmcblk*p* 2>/dev/null"
	cap "dmesg 2>/dev/null | grep -iE 'mmc[0-9]|mmcblk|mmc_host' | tail -20"
	_sd=""; _emmc=""
	if [ "$DEMO" != 1 ]; then
		# SD is removable (mmcblk*/removable=1); eMMC is non-removable + biggest.
		for _b in /sys/block/mmcblk*; do
			[ -e "$_b/removable" ] || continue
			if [ "$(cat "$_b/removable" 2>/dev/null)" = 1 ]; then _sd=$(basename "$_b"); else _emmc=$(basename "$_b"); fi
		done
	fi
	_map=$(askval "Does dmesg show SD=mmc0, WiFi=mmc1, eMMC=mmc2? (yes/no/partial)" "yes")
	calib "storage: microSD=&mmc0 (cd-gpios PF6, cap-sd-highspeed), WiFi SDIO=&mmc1 (non-removable, pwrseq), eMMC=&mmc2 (bus-width 8, hs400-1_8v). vmmc=&reg_cldo3 / vqmmc=&reg_cldo1 VERIFY"
	_dflt=SKIP; [ "$INTERACTIVE" = 1 ] && [ "$_map" = yes ] && _dflt=PASS
	finish "$(verdict "$_dflt")" "SD=${_sd:-?} eMMC=${_emmc:-?}; mmc index map=${_map}" \
		"&mmc0/&mmc1/&mmc2 index + vmmc/vqmmc supplies; root= boot device (mmc0p2 on SD)" \
		"Confirms the SD=mmc0 / eMMC=mmc2 mapping the runbook + boot cmdline rely on. eMMC vmmc/vqmmc rails are VERIFY in the DTS."
}

# ----------------------------------------------------------------------------
t_pmic() {
	begin_test pmic "THE key unknown: probe 0x34 on r_i2c0, decode AXP717C regs, decide axp717 vs axp2202; probe the external CPU regulator (expect tcs4838@0x41)."
	say "Board silk = AXP717C (register map = mainline x-powers,axp717). It has no"
	say "chip-ID register, so we identify by BEHAVIOUR: does the AXP717 map decode"
	say "sanely (esp. DCDC3 ~= 1.10 V for LPDDR4)? If not, it's a genuinely different"
	say "AXP2202 that mainline lacks a driver for. Capture enough to tell either way."
	if ! need i2cget "install i2c-tools (apt/opkg install i2c-tools) or push a static aarch64 i2cget"; then
		finish "$(verdict SKIP)" "n/a" "pmic@34 'compatible' (axp717 vs axp2202)" "i2c-tools absent — cannot read the PMIC."
		return
	fi
	# Locate the bus that ACKs at 0x34 (the r_i2c0 controller).
	_pbus=""
	if [ "$DEMO" != 1 ]; then
		for _b in $(i2c_buses); do
			if i2cget -y "$_b" 0x34 0x00 >/dev/null 2>&1; then _pbus=$_b; break; fi
		done
	fi
	if [ -n "$_pbus" ]; then
		say "  PMIC ACKed at 0x34 on i2c-$_pbus"
	else
		say "  (no 0x34 ACK found yet — using placeholder bus 'N' in the recorded commands)"
		_pbus=N
	fi
	cap "i2cdetect -l"
	cap "i2cdetect -y -r $_pbus"
	cap "i2cdump -y $_pbus 0x34 b"
	cap "for r in 0x00 0x01 0x03 0x83 0x84 0x85 0x90 0x91; do printf 'reg %s = ' \$r; i2cget -y $_pbus 0x34 \$r 2>/dev/null; done"
	cap "cat /sys/kernel/debug/regulator/regulator_summary 2>/dev/null | grep -iE 'dcdc|vdd|axp|dram' || echo '(no regulator_summary — axp driver not bound / debugfs off)'"
	# External CPU (big-cluster) regulator: which of 0x36/0x41/0x60 ACKs?
	_cpu=""
	if [ "$DEMO" != 1 ] && [ "$_pbus" != N ]; then
		for _a in 0x36 0x41 0x60; do
			i2cget -y "$_pbus" "$_a" 0x00 >/dev/null 2>&1 && _cpu="$_cpu $_a"
		done
	fi
	cap "for a in 0x36 0x41 0x60; do i2cget -y $_pbus \$a 0x00 >/dev/null 2>&1 && echo \"addr \$a ACK\"; done  # 0x36=axp1530 0x41=tcs4838 0x60=sy8827g"
	_dflt=SKIP; [ -n "$_pbus" ] && [ "$_pbus" != N ] && _dflt=PASS
	_v=$(verdict "$_dflt")
	_dec=$(askval "Did the dump decode as AXP717 (DCDC3 ~1.10V)? type axp717 or axp2202" "axp717")
	calib "pmic@34: compatible = \"x-powers,$_dec\";  (0x34 ACK on i2c-${_pbus}; DCDC3 target 1.10 V)"
	[ -n "$_cpu" ] && calib "CPU big-cluster regulator ACK at:$_cpu  (0x41 => tcs4838; cluster1 cpu-supply)"
	finish "$_v" "0x34 on i2c-${_pbus}; decode=${_dec}; CPU-reg ACK:${_cpu:- none}" \
		"pmic@34 'compatible'; reg_dcdc3 (vdd-dram 1.10 V); cluster1 cpu-supply (tcs4838@0x41)" \
		"axp717 vs axp2202 decides the mainline PMIC driver. tcs4838 has no mainline driver yet (fan53555 lacks it) — boot at bootloader voltage."
}

# ----------------------------------------------------------------------------
t_lradc() {
	begin_test lradc "Confirm the 3 LRADC side keys fire the right codes. Voltages 410/646/900 mV are transcribed from the vendor DTB, so this is confirmation, not measurement."
	if ! need evtest "install evtest (apt/opkg install evtest)"; then
		finish "$(verdict SKIP)" "n/a" "&lradc button-410/646/900 keymap" "evtest absent."
		return
	fi
	cap "cat /proc/bus/input/devices 2>/dev/null | grep -iA4 lradc || cat /proc/bus/input/devices 2>/dev/null"
	_ev=""; [ "$DEMO" != 1 ] && _ev=$(find_event "lradc")
	if [ -n "$_ev" ]; then
		say "  LRADC input device = /dev/input/$_ev"
	else
		_ev="eventX"
		say "  Could not auto-find the LRADC device; pick it from the list above."
	fi
	say "  Now run evtest and press each side key once:"
	say "    Home  -> expect  code 172 (KEY_HOMEPAGE)"
	say "    Vol+  -> expect  code 115 (KEY_VOLUMEUP)"
	say "    Vol-  -> expect  code 114 (KEY_VOLUMEDOWN)"
	manual "evtest LRADC keys" "evtest /dev/input/$_ev    # press Home, Vol+, Vol-; Ctrl-C to stop"
	pause "  Press Enter here AFTER you have watched the codes in evtest..."
	_r=$(askval "Which keys fired correctly? (e.g. all / home,volup / none)" "all")
	say "  If a key does NOT fire (mainline r329 LRADC scale != vendor 1350 mV ref):"
	say "  add a temp dev_info to sun4i-lradc-keys.c to print the measured uV, or"
	say "  devmem2 0x2009800; if the whole scale is off, fix vref-supply (&reg_bldo2)."
	_mv=$(askval "Measured voltages if you read them (uV, blank if not)" "410000 646000 900000")
	calib "&lradc: button-410=KEY_HOMEPAGE, button-646=KEY_VOLUMEUP, button-900=KEY_VOLUMEDOWN; voltages(uV)=${_mv}"
	_dflt=SKIP; [ "$INTERACTIVE" = 1 ] && [ "$_r" = all ] && _dflt=PASS
	finish "$(verdict "$_dflt")" "keys fired: ${_r}; voltages ${_mv}" \
		"&lradc button-410/646/900 (linux,code + voltage); vref-supply" \
		"If all three fire, drop the VERIFY tags on the keymap."
}

# ----------------------------------------------------------------------------
t_gamepad() {
	begin_test gamepad "Identify the D-pad/ABXY/shoulder/stick-click(L3,R3) source (open question: gpio-keys? USB/i2c MCU? hidraw?) and confirm every button registers."
	cap "cat /proc/bus/input/devices 2>/dev/null"
	cap "ls -l /dev/input/ 2>/dev/null"
	cap "ls -l /dev/hidraw* /dev/ttyS* 2>/dev/null"
	have lsusb && cap "lsusb" || say "  (lsusb absent — install usbutils to see an internal USB gamepad MCU)"
	cap "dmesg 2>/dev/null | grep -iE 'input:|gamepad|joystick|hid|gpio-key' | tail -20"
	_ev=""; [ "$DEMO" != 1 ] && _ev=$(find_event "gamepad|joystick|controller|pad|trimui|hid")
	if [ -n "$_ev" ]; then
		say "  Candidate gamepad device = /dev/input/$_ev"
		have evtest && manual "evtest gamepad" "evtest /dev/input/$_ev   # press D-pad, A/B/X/Y, L1/R1, L2/R2, Select/Start, and CLICK both sticks (L3/R3)"
	else
		say "  No obvious gamepad input node — inspect /proc/bus/input/devices above."
	fi
	say "  Don't forget the stick CLICKS: press each analog stick straight down until"
	say "  it clicks. L3 = BTN_THUMBL (code 317), R3 = BTN_THUMBR (code 318). They are"
	say "  DIGITAL buttons on THIS gamepad node — not the GPADC axes (that's the 'sticks' test)."
	pause "  Press Enter after exercising every button (incl. L3/R3) in evtest..."
	_src=$(askval "What is the kernel source of the pad? (usb-hid / i2c-mcu / gpio-keys / platform / unknown)" "unknown")
	_ok=$(askval "Did every button register? (yes/some/no)" "yes")
	_l3r3=$(askval "Did L3 and R3 (clicking the two sticks) both register? (yes/one/no)" "yes")
	calib "gamepad source = ${_src}; node = /dev/input/${_ev:-?}  (informs the DT/driver decision — do NOT fabricate gpio-keys)"
	calib "stick-click buttons: L3=BTN_THUMBL(317) R3=BTN_THUMBR(318) on the gamepad node; registered=${_l3r3}"
	_dflt=SKIP; [ "$INTERACTIVE" = 1 ] && [ "$_ok" = yes ] && _dflt=PASS
	finish "$(verdict "$_dflt")" "source=${_src}; node=/dev/input/${_ev:-?}; buttons=${_ok}; L3/R3=${_l3r3}" \
		"gamepad input node/driver (open question in PORTING-NOTES §1); NOT gpio-keys; L3/R3=BTN_THUMBL/BTN_THUMBR" \
		"Vendor uses userspace trimui_inputd over an internal MCU; capture what the kernel exposes, INCLUDING the two stick-click (L3/R3) buttons on the same node."
}

# ----------------------------------------------------------------------------
t_sticks() {
	begin_test sticks "Calibrate the two analog sticks: read raw ADC per axis at both extremes + centre. Feeds adc-joystick abs-range / abs-flat."
	cap "for d in /sys/bus/iio/devices/iio:device*; do [ -e \"\$d\" ] && printf '%s name=%s\\n' \"\$d\" \"\$(cat \$d/name 2>/dev/null)\"; done"
	_chs=""
	[ "$DEMO" != 1 ] && _chs=$(ls /sys/bus/iio/devices/iio:device*/in_voltage*_raw 2>/dev/null)
	if [ -z "$_chs" ]; then
		if [ "$DEMO" = 1 ]; then
			say "  (demo) would sweep each in_voltageN_raw channel of gpadc0@2009000 + gpadc1@2009c00."
		else
			say "  No GPADC iio channels found (need the A523 GPADC driver: gpadc0 upstream v7.2,"
			say "  gpadc1 via kernel/patches). Cannot calibrate."
		fi
		calib "joystick-left/right: abs-range = <MIN MAX> per axis — PENDING (no iio channels found)"
		finish "$(verdict SKIP)" "no iio channels" \
			"joystick-left/right axis abs-range + abs-flat (from gpadc0/gpadc1)" \
			"Requires GPADC driver bound; expected 4 channels = LX LY RX RY, 12-bit 0..4095."
		return
	fi
	say "  Found channels — sweep each one. Map: gpadc0 ch0/ch1 = LEFT X/Y, gpadc1 ch0/ch1 = RIGHT X/Y."
	say "  (These are the analog AXES only. The stick-CLICK buttons L3/R3 are digital —"
	say "   test them in the 'gamepad' test, they land on the gamepad input node.)"
	_i=0
	for _f in $_chs; do
		_i=$((_i + 1))
		_name=$(cat "$(dirname "$_f")/name" 2>/dev/null)
		say ""
		say "  Channel $_i: $_f  (device: ${_name:-?})"
		pause "    Move the matching stick to ONE extreme and hold, then press Enter..."
		_a=$(cat "$_f" 2>/dev/null)
		pause "    Move it to the OTHER extreme and hold, then press Enter..."
		_b=$(cat "$_f" 2>/dev/null)
		pause "    Release the stick to CENTRE, then press Enter..."
		_c=$(cat "$_f" 2>/dev/null)
		# order min/max
		_min=$_a; _max=$_b
		if [ "${_a:-0}" -gt "${_b:-0}" ] 2>/dev/null; then _min=$_b; _max=$_a; fi
		_flat=0
		if [ -n "$_min" ] && [ -n "$_max" ] && [ "$_max" -gt "$_min" ] 2>/dev/null; then
			_flat=$(( (_max - _min) / 32 ))
		fi
		{
			printf 'channel %s (%s) %s: min=%s max=%s centre=%s -> abs-flat~%s\n' \
				"$_i" "${_name:-?}" "$_f" "${_min:-?}" "${_max:-?}" "${_c:-?}" "$_flat"
		} >> "$TESTBUF"
		say "    -> min=${_min:-?} max=${_max:-?} centre=${_c:-?}  (suggest abs-flat=$_flat)"
		calib "axis $_i ($_f): abs-range = <${_min:-0} ${_max:-4095}>; abs-flat = <${_flat}>; centre~${_c:-?}"
	done
	_dflt=SKIP; [ "$INTERACTIVE" = 1 ] && [ "$DEMO" != 1 ] && _dflt=PASS
	finish "$(verdict "$_dflt")" "swept $_i channel(s) — see abs-range lines" \
		"joystick-left/right axis@0/1 abs-range + abs-flat (io-channels &gpadc / &gpadc1)" \
		"12-bit expected (0..4095). Set abs-range to the measured min/max and abs-flat to a deadzone."
}

# ----------------------------------------------------------------------------
t_display() {
	begin_test display "Confirm a DRM connector is up, optionally light a test pattern, and sweep the backlight (feeds the DSI panel + pwm-backlight)."
	cap "for s in /sys/class/drm/*/status; do [ -e \"\$s\" ] && printf '%s: %s\\n' \"\$s\" \"\$(cat \$s)\"; done"
	cap "ls /sys/class/drm/ 2>/dev/null"
	if have modetest; then
		cap "modetest -M sun4i-drm 2>&1 | head -n 30"
		say "  To light a solid test pattern (visual PASS check), run e.g.:"
		manual "modetest pattern" "modetest -M sun4i-drm -s <connector_id>@<crtc_id>:720x1280"
	else
		say "  modetest absent (install libdrm-tests / libdrm2 tools) — skipping test pattern."
	fi
	# backlight sweep
	_bl=""
	[ "$DEMO" != 1 ] && for _d in /sys/class/backlight/*; do [ -e "$_d/brightness" ] && { _bl=$_d; break; }; done
	if [ -n "$_bl" ] && [ -w "$_bl/brightness" ]; then
		_orig=$(cat "$_bl/brightness" 2>/dev/null)
		_max=$(cat "$_bl/max_brightness" 2>/dev/null)
		say "  Sweeping backlight $_bl (0 .. $_max). Watch the panel brightness change."
		for _lvl in 0 $((_max / 4)) $((_max / 2)) "$_max"; do
			printf '%s' "$_lvl" > "$_bl/brightness" 2>/dev/null
			{ printf 'set %s/brightness = %s\n' "$_bl" "$_lvl"; } >> "$TESTBUF"
			say "    brightness=$_lvl"
			pause "    (Enter for next level)"
		done
		printf '%s' "$_orig" > "$_bl/brightness" 2>/dev/null
		say "  restored brightness=$_orig"
	elif [ "$DEMO" = 1 ]; then
		say "  (demo) would sweep /sys/class/backlight/*/brightness 0..max and restore."
		manual "backlight sweep" "for l in 0 64 128 255; do echo \$l > /sys/class/backlight/*/brightness; done"
	else
		say "  No writable /sys/class/backlight/*/brightness (need root + PWM backlight bound)."
	fi
	_status=""; [ "$DEMO" != 1 ] && _status=$(cat /sys/class/drm/*/status 2>/dev/null | sort -u | tr '\n' ',')
	_ok=$(askval "Panel lit and backlight visibly changed? (yes/no)" "yes")
	_dflt=SKIP; [ "$INTERACTIVE" = 1 ] && [ "$_ok" = yes ] && _dflt=PASS
	finish "$(verdict "$_dflt")" "connector status: ${_status:-?}; backlight sweep=${_ok}" \
		"MIPI-DSI panel (panel-trimui-smart-pro-s) + pwm-backlight; DRM connector name" \
		"Display needs the DE3.5 + DSI stack (Phase 6). Use /tmp/live.dtb to finish the DE DT."
}

# ----------------------------------------------------------------------------
t_audio() {
	begin_test audio "Exercise speaker (amp on PH6), mic, and headphone-jack detect. Feeds the codec routing + pa-gpios + jack thresholds."
	cap "cat /proc/asound/cards 2>/dev/null"
	have aplay   && cap "aplay -l 2>/dev/null"
	have arecord && cap "arecord -l 2>/dev/null"
	# speaker
	if have speaker-test; then
		say "  Playing a 440 Hz tone on the SPEAKER (the driver should raise the PH6 amp)."
		pause "  Ready to listen? Press Enter..."
		cap "speaker-test -t sine -f 440 -l 1 2>&1 | head -n 15"
	elif have aplay; then
		say "  speaker-test absent; play any wav with: aplay /path/to/tone.wav"
		manual "speaker" "aplay /usr/share/sounds/alsa/Front_Center.wav"
	else
		need aplay "install alsa-utils (aplay/arecord/speaker-test)" || true
	fi
	_spk=$(askval "Did you HEAR the speaker tone? (yes/no)" "yes")
	# mic
	if have arecord && have aplay; then
		say "  Recording 3 s from the MIC, then playing it back."
		pause "  Speak after pressing Enter..."
		cap "arecord -d 3 -f cd /tmp/hwv-mic.wav 2>&1 | tail -n 5 && aplay /tmp/hwv-mic.wav 2>&1 | tail -n 3"
	fi
	_mic=$(askval "Did the mic play-back contain your voice? (yes/no/skip)" "yes")
	# jack detect
	cap "cat /proc/bus/input/devices 2>/dev/null | grep -iB2 -A3 'jack\\|headphone\\|hmic' || echo '(no jack input device listed)'"
	cap "for e in /sys/class/extcon/*/state; do [ -e \"\$e\" ] && printf '%s: %s\\n' \"\$e\" \"\$(cat \$e)\"; done"
	say "  Insert then remove the headphones; a jack switch (SW_HEADPHONE_INSERT) or"
	say "  extcon state should toggle."
	_jack=$(askval "Did headphone insert/remove get detected? (yes/no/skip)" "skip")
	calib "&codec: pa-gpios PH6 amp = speaker ${_spk}; mic ${_mic}; jack-detect ${_jack}"
	_dflt=SKIP; [ "$INTERACTIVE" = 1 ] && [ "$_spk" = yes ] && _dflt=PASS
	finish "$(verdict "$_dflt")" "speaker=${_spk}; mic=${_mic}; jack=${_jack}" \
		"&codec allwinner,audio-routing + allwinner,pa-gpios (PH6); jack/HMIC detect thresholds" \
		"Speaker is fed from the LINEOUT pins via the PH6 amp; confirm routing + jack detect for the codec patch."
}

# ----------------------------------------------------------------------------
t_leds() {
	begin_test leds "Confirm the 17-LED RGB array and its colour order (grb vs rgb) by driving one LED red/green/blue."
	cap "ls /sys/class/leds/ 2>/dev/null"
	cap "ls -d /sys/class/leds/*multi* 2>/dev/null | wc -l | tr -d ' '; echo ' multi-led entries'"
	_led=""
	[ "$DEMO" != 1 ] && for _d in /sys/class/leds/*; do
		[ -e "$_d/multi_intensity" ] && [ -w "$_d/brightness" ] && { _led=$_d; break; }
	done
	if [ -n "$_led" ]; then
		_mx=$(cat "$_led/max_brightness" 2>/dev/null); _mx=${_mx:-255}
		say "  Driving $_led. Watch which physical colour lights up."
		for _c in "R:255 0 0" "G:0 255 0" "B:0 0 255"; do
			_lbl=${_c%%:*}; _val=${_c#*:}
			printf '%s' "$_val" > "$_led/multi_intensity" 2>/dev/null
			printf '%s' "$_mx" > "$_led/brightness" 2>/dev/null
			{ printf 'set %s multi_intensity=%s (asked %s)\n' "$_led" "$_val" "$_lbl"; } >> "$TESTBUF"
			say "    asked for $_lbl -> multi_intensity='$_val'"
			pause "    What colour is actually lit? (Enter for next)"
		done
		printf '0' > "$_led/brightness" 2>/dev/null
	elif [ "$DEMO" = 1 ]; then
		manual "LED colour order" "echo '255 0 0' > /sys/class/leds/<multi-led>/multi_intensity; echo 255 > .../brightness"
	else
		say "  No writable multi-LED under /sys/class/leds (need root + &ledc bound)."
	fi
	_cnt=$(askval "How many LEDs are physically present/addressable?" "17")
	_order=$(askval "Colour order? (grb = correct default / rgb = need to flip)" "grb")
	calib "&ledc: LED count = ${_cnt} (expect 17); pixel-format = ${_order} (default grb; set allwinner,pixel-format if rgb)"
	_dflt=SKIP; [ "$INTERACTIVE" = 1 ] && _dflt=PASS
	finish "$(verdict "$_dflt")" "count=${_cnt}; order=${_order}" \
		"&ledc multi-led@0..16 count; allwinner,pixel-format (grb vs rgb)" \
		"Vendor DTB says 17 LEDs, output RGB. WS2812 default wire order is grb — flip only if colours swap."
}

# ----------------------------------------------------------------------------
t_vibrator() {
	begin_test vibrator "Confirm the haptic motor (pwm-vibrator on pwm0 ch7, 50000 ns, normal polarity). The driver registers an input force-feedback (FF_RUMBLE) device, so drive it via a rumble effect."
	cap "cat /proc/bus/input/devices 2>/dev/null | grep -iB2 -A3 'vibr\\|rumble\\|haptic' || echo '(no vibrator input device listed)'"
	cap "ls /sys/class/pwm/ 2>/dev/null; for c in /sys/class/pwm/pwmchip*/pwm*; do [ -e \"\$c/period\" ] && printf '%s period=%s duty=%s enable=%s\\n' \"\$c\" \"\$(cat \$c/period 2>/dev/null)\" \"\$(cat \$c/duty_cycle 2>/dev/null)\" \"\$(cat \$c/enable 2>/dev/null)\"; done"
	cap "dmesg 2>/dev/null | grep -iE 'vibrat|pwm-vibrator|rumble|input:.*[Vv]ibr' | tail -10"
	_ev=""; [ "$DEMO" != 1 ] && _ev=$(find_event "vibr|rumble|haptic")
	if [ -n "$_ev" ]; then
		say "  Vibrator FF device = /dev/input/$_ev"
		have fftest && manual "buzz the motor" "fftest /dev/input/$_ev   # select a RUMBLE/periodic effect and feel it"
	else
		say "  No vibrator FF input node found (need the pwm-vibrator driver bound + pwm0)."
		manual "buzz the motor" "fftest /dev/input/eventN   # pick the vibrator node from the list above"
	fi
	pause "  Press Enter after trying to buzz the motor..."
	_ok=$(askval "Did the motor buzz? (yes/no/skip)" "yes")
	calib "vibrator: pwms = <&pwm0 7 50000 0>; pwm-names=\"enable\"; compatible=\"pwm-vibrator\" (vendor: pwm0 ch7, 50 us period, normal polarity)"
	_dflt=SKIP; [ "$INTERACTIVE" = 1 ] && [ "$_ok" = yes ] && _dflt=PASS
	finish "$(verdict "$_dflt")" "buzz=${_ok}; node=/dev/input/${_ev:-?}" \
		"vibrator{} compatible pwm-vibrator; pwms=<&pwm0 7 50000 0> (PWM0 channel 7)" \
		"Vendor DTB: haptic on PWM0 ch7, 50000 ns period, normal polarity. Shares &pwm0 with backlight (ch0) + fan (ch10)."
}

# ----------------------------------------------------------------------------
t_wifi() {
	begin_test wifi "Confirm the AIC8800 SDIO WiFi (mmc1) loads and scans; capture the D80 vs DC variant."
	cap "lsmod 2>/dev/null | grep -iE 'aic|cfg80211|mac80211'"
	cap "ls -1 /lib/firmware/ 2>/dev/null | grep -i aic; ls -d /lib/firmware/aic8800* 2>/dev/null"
	cap "ls -l /sys/bus/sdio/devices 2>/dev/null; for d in /sys/bus/sdio/devices/*; do cat \"\$d/uevent\" 2>/dev/null; done"
	cap "ls /sys/class/net/ 2>/dev/null"
	cap "dmesg 2>/dev/null | grep -iE 'aic|sdio|wlan|mmc1' | tail -20"
	_wdev=""
	[ "$DEMO" != 1 ] && for _n in /sys/class/net/wlan*; do [ -e "$_n" ] && { _wdev=$(basename "$_n"); break; }; done
	if have iw && [ -n "$_wdev" ]; then
		say "  Bringing $_wdev up and scanning (needs the out-of-tree aic8800 module loaded)."
		cap "ip link set $_wdev up 2>&1; iw dev $_wdev scan 2>&1 | grep -E 'SSID|freq' | head -n 20"
	elif ! have iw; then
		need iw "install iw (apt/opkg install iw) or use wpa_cli" || true
	else
		say "  No wlanN interface yet — build/load the AIC8800 module (kernel/aic8800/)."
	fi
	_var=$(askval "AIC8800 variant that actually loaded? (d80 / dc)" "d80")
	_scan=$(askval "Did the scan see networks? (yes/no)" "yes")
	calib "AIC8800 variant = aic8800${_var}; mmc1 SDIO + wifi_pwrseq (reset PM1). Firmware dir /lib/firmware/aic8800${_var}"
	_dflt=SKIP; [ "$INTERACTIVE" = 1 ] && [ "$_scan" = yes ] && _dflt=PASS
	finish "$(verdict "$_dflt")" "iface=${_wdev:-none}; variant=${_var}; scan=${_scan}" \
		"&mmc1 SDIO + wifi_pwrseq (PM1); AIC8800 out-of-tree module + firmware dir" \
		"AIC8800 stays out-of-tree; only the DT power-sequencing hooks upstream with the board."
}

# ----------------------------------------------------------------------------
t_bluetooth() {
	begin_test bluetooth "Attach and bring up the AIC8800 Bluetooth over UART (ttyS1), then scan. Feeds the &uart1 BT hooks (PM2/3/4)."
	cap "ls -l /dev/ttyS1 2>/dev/null; dmesg 2>/dev/null | grep -iE 'hci|bluetooth|uart1' | tail -15"
	say "  Attach the controller (vendor uses ttyAS1 = mainline ttyS1):"
	if have hciattach; then
		manual "BT attach" "hciattach -n /dev/ttyS1 aic 1500000    # backgrounded; needs the aic8800_btlpm helper"
	elif have btattach; then
		manual "BT attach" "btattach -B /dev/ttyS1 -P aic"
	else
		need hciattach "install bluez (hciattach/hciconfig/hcitool) or bluez-tools (btattach)" || true
	fi
	pause "  Attach BT in another shell, then press Enter to check hci0..."
	have hciconfig && cap "hciconfig -a 2>&1 | head -n 20"
	have hciconfig && cap "hciconfig hci0 up 2>&1"
	if have bluetoothctl; then
		manual "BT scan" "bluetoothctl -- scan on   # then: devices"
	elif have hcitool; then
		manual "BT scan" "hcitool scan"
	fi
	_up=$(askval "Did hci0 come up and scan find a device? (yes/no)" "yes")
	calib "Bluetooth: hciattach -n /dev/ttyS1 aic; control on R_PIO Port M (bt_rst=PM2, bt_wake=PM3, bt_hostwake=PM4)"
	_dflt=SKIP; [ "$INTERACTIVE" = 1 ] && [ "$_up" = yes ] && _dflt=PASS
	finish "$(verdict "$_dflt")" "hci0 up + scan = ${_up}" \
		"&uart1 (uart-has-rtscts) BT HCI; R_PIO PM2/PM3/PM4 control lines" \
		"BT rides the AIC8800 out-of-tree stack; the DT contributes only the &uart1 wiring."
}

# ----------------------------------------------------------------------------
t_usb() {
	begin_test usb "Check USB host enumeration, the OTG/gadget mode, and USB-C DisplayPort alt-mode (SVID 0xff01)."
	if have lsusb; then
		say "  Plug a USB device into the (host) port, then confirm it enumerates."
		pause "  Press Enter after plugging something in..."
		cap "lsusb"
	else
		need lsusb "install usbutils (lsusb)" || true
	fi
	cap "cat /sys/kernel/debug/usb/devices 2>/dev/null | head -n 30 || echo '(no usbfs debug)'"
	cap "ls /sys/kernel/config/usb_gadget/ 2>/dev/null; echo '(otg dr_mode = otg on the external USB-C)'"
	# Type-C / DP alt-mode
	cap "for t in /sys/class/typec/*; do [ -e \"\$t/uevent\" ] && cat \"\$t/uevent\"; done"
	cap "for a in /sys/class/typec/*/*/svid /sys/class/typec/*/*/*/svid; do [ -e \"\$a\" ] && printf '%s = %s\\n' \"\$a\" \"\$(cat \$a)\"; done"
	cap "ls -l /sys/bus/i2c/devices 2>/dev/null | grep -iE 'husb|ps87|tcpc'"
	# USB-PD: the husb311/RT1711H TCPC + tcpm expose the role + negotiated contract.
	cap "for r in /sys/class/typec/*/power_role /sys/class/typec/*/data_role; do [ -e \"\$r\" ] && printf '%s = %s\\n' \"\$r\" \"\$(cat \$r)\"; done"
	cap "for p in /sys/class/power_supply/tcpm* /sys/class/power_supply/*usb*pd*; do [ -e \"\$p/uevent\" ] && { echo \"--- \$p ---\"; cat \"\$p/uevent\"; }; done"
	cap "ls /sys/class/usb_power_delivery/ 2>/dev/null"
	say "  For DP-out: plug a USB-C DisplayPort sink; expect an alt-mode with SVID 0xff01."
	_host=$(askval "Did the USB host port enumerate a device? (yes/no)" "yes")
	_pd=$(askval "USB-PD contract negotiated (tcpm power_supply ONLINE / PDOs)? (yes/no/skip)" "skip")
	_dp=$(askval "USB-C DisplayPort alt-mode (svid ff01) present? (yes/no/skip)" "skip")
	calib "USB: otg dr_mode=otg; PD via husb311 (compatible \"hynetek,husb311\",\"richtek,rt1711h\"); DP alt-mode svid 0xff01 via ps8743 mux (DP-out is Phase-6+)"
	_dflt=SKIP; [ "$INTERACTIVE" = 1 ] && [ "$_host" = yes ] && _dflt=PASS
	finish "$(verdict "$_dflt")" "host-enum=${_host}; pd-contract=${_pd}; dp-altmode=${_dp}" \
		"&usb_otg / &ehci0-1 / &ohci0-1 / &usbphy; husb311 TCPC (PD) + typec DP alt-mode (ps8743 mux)" \
		"USB2 data + PD charging work now (husb311 = RT1711H fallback, upstream); DP-out depends on the ps8743 mux driver + display stack (Phase 6)."
}

# ----------------------------------------------------------------------------
t_battery() {
	begin_test battery "Read the battery/charger and compare to the expected 5000 mAh / 4.2 V CV / 1 A. Feeds simple-battery + axp717 charger limits."
	cap "for ps in /sys/class/power_supply/*; do [ -e \"\$ps/uevent\" ] && { echo \"--- \$ps ---\"; cat \"\$ps/uevent\"; }; done"
	_cap=""; _volt=""; _stat=""
	if [ "$DEMO" != 1 ]; then
		for ps in /sys/class/power_supply/*; do
			[ -e "$ps/capacity" ]     && _cap=$(cat "$ps/capacity" 2>/dev/null)
			[ -e "$ps/voltage_now" ]  && _volt=$(cat "$ps/voltage_now" 2>/dev/null)
			[ -e "$ps/status" ]       && _stat=$(cat "$ps/status" 2>/dev/null)
		done
	fi
	say "  Plug and unplug the charger; the 'status' should flip Charging <-> Discharging."
	pause "  Press Enter after toggling the charger..."
	_ok=$(askval "Did capacity/voltage read sanely and status flip with the charger? (yes/no)" "yes")
	calib "battery: charge-full-design ~5000 mAh; voltage-max-design 4.2 V (CV); constant-charge-current-max 1 A"
	_dflt=SKIP; [ "$INTERACTIVE" = 1 ] && [ "$_ok" = yes ] && _dflt=PASS
	finish "$(verdict "$_dflt")" "capacity=${_cap:-?}% voltage=${_volt:-?}uV status=${_stat:-?}" \
		"battery{} simple-battery values; axp717 constant-charge-current-max / voltage-max-design" \
		"Expected 5000 mAh, 4.2 V CV, 1 A runtime. AXP717 has a HW fuel gauge (no OCV table in DT)."
}

# ----------------------------------------------------------------------------
t_rtc() {
	begin_test rtc "Confirm the on-SoC RTC (rtc@7090000) is bound, reads a time, and ticks. READ-ONLY (no clock set). Feeds &rtc enable + wake/alarm."
	cap "ls -l /dev/rtc* 2>/dev/null; ls /sys/class/rtc/ 2>/dev/null"
	cap "for r in /sys/class/rtc/rtc*; do [ -e \"\$r\" ] && printf '%s name=%s date=%s time=%s\\n' \"\$r\" \"\$(cat \$r/name 2>/dev/null)\" \"\$(cat \$r/date 2>/dev/null)\" \"\$(cat \$r/time 2>/dev/null)\"; done"
	have hwclock && cap "hwclock -r 2>&1"
	cap "dmesg 2>/dev/null | grep -iE 'rtc|sun6i-rtc' | tail -10"
	# Tick check: read the RTC seconds twice, ~2 s apart, and confirm it advanced.
	# Interactive only (needs the two Enter presses to space the reads apart).
	_ticks="skip"
	if [ "$INTERACTIVE" = 1 ] && [ "$DEMO" != 1 ] && [ -e /sys/class/rtc/rtc0/since_epoch ]; then
		_t1=$(cat /sys/class/rtc/rtc0/since_epoch 2>/dev/null)
		pause "  Waiting to re-read the RTC (press Enter, wait ~2 s, press Enter again)..."
		pause ""
		_t2=$(cat /sys/class/rtc/rtc0/since_epoch 2>/dev/null)
		if [ -n "$_t1" ] && [ -n "$_t2" ] && [ "$_t2" -gt "$_t1" ] 2>/dev/null; then _ticks="yes"; else _ticks="no"; fi
		{ printf 'since_epoch: t1=%s t2=%s -> ticking=%s\n' "${_t1:-?}" "${_t2:-?}" "$_ticks"; } >> "$TESTBUF"
		say "  since_epoch $_t1 -> $_t2 (ticking=$_ticks)"
	fi
	_ok=$(askval "RTC present and reading a sane time? (yes/no)" "yes")
	calib "&rtc: status = \"okay\" (sun55i-a523 rtc@7090000); reads time + ticks (${_ticks}). Confirm battery-backed retention across a power cycle if an RTC cell is fitted."
	_dflt=SKIP; [ "$INTERACTIVE" = 1 ] && [ "$_ok" = yes ] && _dflt=PASS
	finish "$(verdict "$_dflt")" "read=${_ok}; ticking=${_ticks}" \
		"&rtc (rtc@7090000) enable; RTC time source / alarm-wake" \
		"On-SoC RTC via sun55i-a523.dtsi. Retention across power-off depends on a fitted RTC battery/supercap (VERIFY on HW)."
}

# ----------------------------------------------------------------------------
t_thermal() {
	begin_test thermal "Read the thermal zones and spin the PWM fan (pwm0 ch10 / PB6, inverted). Feeds pwm-fan + the THS cooling-map."
	cap "for z in /sys/class/thermal/thermal_zone*; do [ -e \"\$z/temp\" ] && printf '%s type=%s temp=%s\\n' \"\$z\" \"\$(cat \$z/type 2>/dev/null)\" \"\$(cat \$z/temp 2>/dev/null)\"; done"
	cap "for c in /sys/class/thermal/cooling_device*; do [ -e \"\$c/type\" ] && printf '%s type=%s max=%s\\n' \"\$c\" \"\$(cat \$c/type 2>/dev/null)\" \"\$(cat \$c/max_state 2>/dev/null)\"; done"
	_fan=""
	[ "$DEMO" != 1 ] && for _c in /sys/class/thermal/cooling_device*; do
		case "$(cat "$_c/type" 2>/dev/null)" in *fan*|*pwm*) [ -w "$_c/cur_state" ] && { _fan=$_c; break; };; esac
	done
	if [ -n "$_fan" ]; then
		_orig=$(cat "$_fan/cur_state" 2>/dev/null)
		_max=$(cat "$_fan/max_state" 2>/dev/null); _max=${_max:-1}
		say "  Spinning fan $_fan to max_state=$_max — listen/feel for airflow."
		printf '%s' "$_max" > "$_fan/cur_state" 2>/dev/null
		pause "  Fan should be running. Press Enter to stop..."
		printf '%s' "$_orig" > "$_fan/cur_state" 2>/dev/null
		say "  restored cur_state=$_orig"
	elif [ "$DEMO" = 1 ]; then
		manual "fan spin" "echo <max_state> > /sys/class/thermal/cooling_device<N>/cur_state"
	else
		say "  No writable pwm-fan cooling_device (need root + PWM driver bound)."
	fi
	_ok=$(askval "Did the fan spin when driven? (yes/no/skip)" "yes")
	calib "pwm_fan: pwms = <&pwm0 10 40000 PWM_POLARITY_INVERTED>; 32 cooling-levels; bind to THS cpu thermal-zone"
	_dflt=SKIP; [ "$INTERACTIVE" = 1 ] && [ "$_ok" = yes ] && _dflt=PASS
	finish "$(verdict "$_dflt")" "fan spin=${_ok}; zones read from sysfs" \
		"pwm_fan (pwm0 ch10 PB6 inverted) + #cooling-cells; THS thermal-zones cooling-map" \
		"Vendor exposes >=6 thermal zones; adopt the A523 THS series and add our pwm-fan cooling-map on top."
}

# ----------------------------------------------------------------------------
t_cpufreq() {
	begin_test cpufreq "Confirm the two cpufreq domains and their OPP ladders vs the vendor bins (little <=1416, big <=1800 MHz)."
	cap "for p in /sys/devices/system/cpu/cpufreq/policy0 /sys/devices/system/cpu/cpufreq/policy4; do [ -d \"\$p\" ] && { echo \"--- \$p ---\"; cat \"\$p/scaling_cur_freq\" 2>/dev/null; cat \"\$p/scaling_available_frequencies\" 2>/dev/null; }; done"
	cap "cat /sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq /sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq 2>/dev/null"
	cap "cat /sys/devices/system/cpu/cpufreq/policy4/scaling_min_freq /sys/devices/system/cpu/cpufreq/policy4/scaling_max_freq 2>/dev/null"
	_p0=""; _p4=""
	if [ "$DEMO" != 1 ]; then
		_p0=$(cat /sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq 2>/dev/null)
		_p4=$(cat /sys/devices/system/cpu/cpufreq/policy4/scaling_max_freq 2>/dev/null)
	fi
	_ok=$(askval "Two policies (0=little, 4=big) present with sane freqs? (yes/no)" "yes")
	calib "cpufreq: policy0 little <=1416 MHz (reg_dcdc1), policy4 big <=1800 MHz nominal (tcs4838); see trimui-cpu-opp.dtsi"
	_dflt=SKIP; [ -n "$_p0" ] && [ -n "$_p4" ] && _dflt=PASS
	finish "$(verdict "$_dflt")" "policy0 max=${_p0:-?}kHz; policy4 max=${_p4:-?}kHz" \
		"cluster0/1 OPP tables (dts/staging/trimui-cpu-opp.dtsi); two cpufreq domains" \
		"Vendor bin vf0100: little 408..1416 MHz, big 408..1800 (turbo bins to 2160). Confirm both domains bind."
}

# ---- dispatcher ------------------------------------------------------------
run_test() {
	case "$1" in
		vendorboot) t_vendorboot ;;
		recon)     t_recon ;;
		backup)    t_backup ;;
		fel)       t_fel ;;
		sdboot)    t_sdboot ;;
		identity)  t_identity ;;
		storage)   t_storage ;;
		pmic)      t_pmic ;;
		lradc)     t_lradc ;;
		gamepad)   t_gamepad ;;
		sticks)    t_sticks ;;
		display)   t_display ;;
		audio)     t_audio ;;
		leds)      t_leds ;;
		vibrator)  t_vibrator ;;
		wifi)      t_wifi ;;
		bluetooth) t_bluetooth ;;
		usb)       t_usb ;;
		battery)   t_battery ;;
		rtc)       t_rtc ;;
		thermal)   t_thermal ;;
		cpufreq)   t_cpufreq ;;
		*)         say "unknown test: $1  (see --list)" ;;
	esac
}

# ---- report init / finalize ------------------------------------------------
report_init() {
	_model=""; _compat=""
	if [ "$DEMO" != 1 ]; then
		_model=$(cat /proc/device-tree/model 2>/dev/null | tr -d '\000')
		_compat=$(cat /proc/device-tree/compatible 2>/dev/null | tr '\000' ' ')
	fi
	{
		printf '# Trimui Smart Pro S — on-hardware verification report\n\n'
		printf -- '- **Generated:** %s\n' "$(date 2>/dev/null)"
		printf -- '- **Mode:** %s\n' "$MODE"
		printf -- '- **Model (DT):** %s\n' "${_model:-unknown / not read}"
		printf -- '- **Compatible (DT):** %s\n' "${_compat:-unknown / not read}"
		printf -- '- **Kernel:** %s\n' "$(uname -a 2>/dev/null || echo unknown)"
		printf -- '- **Generated by:** %s (companion to recon.sh)\n' "$SELF"
		printf '\n'
		printf 'Each section records what was tested, the exact commands run, the raw output,\n'
		printf 'the measured value, the device-tree property / mainline artifact it feeds, and a\n'
		printf 'verdict (PASS / FAIL / SKIP / PENDING). This is reproducible "tested-on-hardware"\n'
		printf 'evidence for the mainline submission — paste the relevant section into a patch'"'"'s\n'
		printf 'test notes or a tracking issue (see docs/UPSTREAMING.md).\n'
		printf '\n---\n'
	} > "$REPORT"
	REPORT_STARTED=1
}

report_finalize() {
	[ "${REPORT_STARTED:-0}" = 1 ] || return 0
	{
		printf '\n\n---\n\n## Summary\n\n'
		printf '| Test | Feature | Verdict | Measured value |\n'
		printf '|---|---|---|---|\n'
		for id in $ALL_IDS; do
			_line=$(grep "^$id	" "$SUMMARY" 2>/dev/null | tail -n1)
			[ -n "$_line" ] || continue
			_vd=$(printf '%s' "$_line" | cut -f2)
			_tt=$(printf '%s' "$_line" | cut -f3)
			_vl=$(printf '%s' "$_line" | cut -f4)
			printf '| %s | %s | %s | %s |\n' "$id" "$_tt" "$_vd" "${_vl:-—}"
		done
		_np=$(cut -f2 "$SUMMARY" 2>/dev/null | grep -c '^PASS$')
		_nf=$(cut -f2 "$SUMMARY" 2>/dev/null | grep -c '^FAIL$')
		_ns=$(cut -f2 "$SUMMARY" 2>/dev/null | grep -c '^SKIP$')
		_nd=$(cut -f2 "$SUMMARY" 2>/dev/null | grep -c '^PENDING$')
		printf '\n**Totals:** %s PASS · %s FAIL · %s SKIP · %s PENDING\n' \
			"${_np:-0}" "${_nf:-0}" "${_ns:-0}" "${_nd:-0}"
		printf '\n## Calibration values ready for the device tree\n\n'
		printf 'Confirmed numbers to paste into `dts/sun55i-a523-trimui-smart-pro-s.dts`\n'
		printf '(and the U-Boot defconfig for the DRAM rail). Drop the matching `VERIFY` tags\n'
		printf 'once each is confirmed on hardware:\n\n'
		printf '```\n'
		if [ -s "$CALIB" ]; then cat "$CALIB"; else printf '(no calibration values captured in this run)\n'; fi
		printf '```\n'
	} >> "$REPORT"
}

# ---- menu ------------------------------------------------------------------
id_for_num() {
	case "$1" in ''|*[!0-9]*) return ;; esac
	_i=0
	for _id in $TEST_IDS; do
		_i=$((_i + 1))
		[ "$_i" = "$1" ] && { echo "$_id"; return; }
	done
}

menu() {
	while :; do
		printf '\n===== Trimui Smart Pro S — HW verification =====\n'
		printf ' Pre-rootfs bring-up phases (host / stock OS; type the id to run):\n'
		for _id in $BRINGUP_IDS; do
			printf '      %-9s %s\n' "$_id" "$(test_title "$_id")"
		done
		printf ' On-device subsystem tests (booted mainline rootfs):\n'
		_i=0
		for _id in $TEST_IDS; do
			_i=$((_i + 1))
			printf '  %2d) %-9s %s\n' "$_i" "$_id" "$(test_title "$_id")"
		done
		printf '   a) run subsystem tests   b) run bring-up phases   r) report path   q) quit\n'
		printf 'select> '
		read -r _sel || break
		case "$_sel" in
			q|Q|'') break ;;
			a|A) for _id in $TEST_IDS; do run_test "$_id"; done ;;
			b|B) for _id in $BRINGUP_IDS; do run_test "$_id"; done ;;
			r|R) printf 'report: %s\n' "$REPORT" ;;
			*)
				_id=$(id_for_num "$_sel")
				if [ -n "$_id" ]; then
					run_test "$_id"
				else
					case " $ALL_IDS " in
						*" $_sel "*) run_test "$_sel" ;;
						*) printf 'invalid selection: %s\n' "$_sel" ;;
					esac
				fi ;;
		esac
	done
}

# ---- help ------------------------------------------------------------------
usage() {
	cat <<EOF
$SELF — interactive hardware verification for the Trimui Smart Pro S
(Allwinner A523). Guides you through the pre-rootfs bring-up phases and the
on-device subsystem tests, and writes a mainlining-ready Markdown report.
Companion to the read-only recon.sh.

Usage:
  sh $SELF [options] [test ...]

Options:
  -h, --help        this help
  -l, --list        list the test ids and titles (both groups)
  -a, --all         run every on-device subsystem test in order
  -b, --bringup     run the pre-rootfs bring-up phases (vendorboot/recon/backup/fel/sdboot)
  -d, --demo        no-device self-test: emit a full PENDING report skeleton
  -y, --yes         non-interactive (auto-skip all prompts) even on a TTY
  -o, --output FILE report path (default: /tmp/trimui-hw-report-<timestamp>.md)

With no test ids and a TTY, an interactive menu opens (pick / re-run / skip).
Given test ids, only those run, e.g.:  sh $SELF pmic sticks   sh $SELF fel

Two groups (same report format):
  * BRING-UP phases (vendorboot recon backup fel sdboot) — the pre-rootfs runbook steps,
    run from the HOST / stock OS. The risky ones (eMMC backup dd, FEL, SD write)
    are GUIDED ONLY: the script prints the command to run by hand, loudly labels
    the risk, requires a typed acknowledgement, and records the result. It NEVER
    dds, enters FEL, or writes/partitions storage itself — you cannot brick the
    device by running this script.
  * SUBSYSTEM tests (identity storage pmic .. rtc thermal cpufreq) — actively
    exercise each block on a booted mainline rootfs.

Notes:
  * POSIX sh / busybox-ash safe. Mostly read-only; the only writes are explicit
    backlight/LED/fan/vibrator sweeps (and only if the sysfs node is writable),
    each of which restores the original value. It never flashes or repartitions.
  * If a tool is missing it prints how to install it and SKIPs the test.
  * If stdin is not a TTY, every prompt auto-skips (safe to pipe / run in CI).
EOF
}

list_tests() {
	printf 'Bring-up phases (pre-rootfs; host / stock OS):\n'
	for _id in $BRINGUP_IDS; do printf '  %-9s %s\n' "$_id" "$(test_title "$_id")"; done
	printf 'Subsystem tests (booted mainline rootfs):\n'
	for _id in $TEST_IDS; do printf '  %-9s %s\n' "$_id" "$(test_title "$_id")"; done
}

# ============================================================================
# main
# ============================================================================
REPORT=""
RUN_ALL=0
RUN_BRINGUP=0
ARGS_TESTS=""

while [ $# -gt 0 ]; do
	case "$1" in
		-h|--help)    usage; exit 0 ;;
		-l|--list)    list_tests; exit 0 ;;
		-a|--all)     RUN_ALL=1 ;;
		-b|--bringup) RUN_BRINGUP=1 ;;
		-d|--demo)    DEMO=1; INTERACTIVE=0 ;;
		-y|--yes)     INTERACTIVE=0 ;;
		-o|--output)  shift; REPORT=$1 ;;
		--output=*)   REPORT=${1#--output=} ;;
		-*)           say "unknown option: $1 (see --help)"; exit 2 ;;
		*)            ARGS_TESTS="$ARGS_TESTS $1" ;;
	esac
	shift
done

# Mode label for the report header.
if [ "$DEMO" = 1 ]; then
	MODE="demo (no device — PENDING skeleton)"
elif [ "$INTERACTIVE" = 1 ]; then
	MODE="interactive on-device"
else
	MODE="non-interactive (prompts auto-skipped)"
fi

# Working area (accumulators) + report path.
WORK=$(mktemp -d 2>/dev/null || echo "/tmp/hwverify.$$")
mkdir -p "$WORK" 2>/dev/null
TESTBUF="$WORK/testbuf"
SUMMARY="$WORK/summary"
CALIB="$WORK/calib"
: > "$SUMMARY"; : > "$CALIB"; : > "$TESTBUF"
[ -n "$REPORT" ] || REPORT="/tmp/trimui-hw-report-$(date +%Y%m%d-%H%M%S 2>/dev/null || echo now).md"

cleanup() { rm -rf "$WORK" 2>/dev/null; }
trap cleanup EXIT INT TERM

say "Trimui Smart Pro S — HW verification ($MODE)"
say "Report: $REPORT"

report_init

# Decide what to run.
if [ "$DEMO" = 1 ]; then
	# Full skeleton: bring-up phases + every subsystem test, all PENDING.
	for id in $ALL_IDS; do run_test "$id"; done
elif [ -n "$ARGS_TESTS" ]; then
	for id in $ARGS_TESTS; do run_test "$id"; done
elif [ "$RUN_BRINGUP" = 1 ] && [ "$RUN_ALL" = 1 ]; then
	for id in $ALL_IDS; do run_test "$id"; done
elif [ "$RUN_BRINGUP" = 1 ]; then
	for id in $BRINGUP_IDS; do run_test "$id"; done
elif [ "$RUN_ALL" = 1 ]; then
	for id in $TEST_IDS; do run_test "$id"; done
elif [ "$INTERACTIVE" = 1 ]; then
	menu
else
	# non-interactive, no explicit tests: run the subsystem tests, prompts auto-skip.
	for id in $TEST_IDS; do run_test "$id"; done
fi

report_finalize

say ""
say "=========================================================="
say "Report written to: $REPORT"
say "  (copy it off the device, e.g.  adb pull $REPORT )"
say "=========================================================="
