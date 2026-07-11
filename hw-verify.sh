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
#     sh hw-verify.sh --all      # run every test in order
#     sh hw-verify.sh pmic lradc # run just those tests
#     sh hw-verify.sh --demo     # no-device skeleton (everything PENDING)
#     sh hw-verify.sh --list     # list test ids
#     sh hw-verify.sh --help
#
# POSIX sh / busybox-ash safe, read-mostly. The only writes are optional and
# explicit (backlight/LED/fan sweeps, and only after confirming the sysfs node
# is writable); each restores the original value. It never flashes or partitions.
# If a tool is missing it prints how to get it and SKIPs; if stdin is not a TTY
# it auto-skips every prompt so piping/CI can never wedge it.
#
# Cross-references: recon.sh, dts/sun55i-a523-trimui-smart-pro-s.dts,
# docs/HARDWARE-BRINGUP.md (Phase 5/7), docs/UPSTREAMING.md, FIRMWARE-FINDINGS.md.
# ============================================================================

SELF=hw-verify.sh
MAXLINES=40

# ---- mode / tty detection --------------------------------------------------
DEMO=0
INTERACTIVE=1
[ -t 0 ] || INTERACTIVE=0

TEST_IDS="identity pmic lradc gamepad sticks display audio leds wifi bluetooth usb battery thermal cpufreq"

# ---- primitives (echo recon.sh's helper style) -----------------------------
sec()  { printf '\n\n========== %s ==========\n' "$1"; }
say()  { printf '%s\n' "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }

need() {
	# need TOOL "install hint" -> 0 if present, else print hint + return 1
	have "$1" && return 0
	say "  [tool missing] '$1' not found — $2"
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
	printf '  Verdict [P]ass/[F]ail/[S]kip (default %s): ' "${1:-SKIP}"
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
	printf '  %s [%s]: ' "$1" "$2"
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
		identity)  echo "Identity, kernel & live DTB" ;;
		pmic)      echo "PMIC identity (AXP717C vs AXP2202) + CPU regulator" ;;
		lradc)     echo "LRADC side keys (Home / Vol+ / Vol-)" ;;
		gamepad)   echo "Gamepad / buttons (D-pad, ABXY, L/R) source" ;;
		sticks)    echo "Analog sticks (GPADC) calibration" ;;
		display)   echo "Display (DRM connector) + backlight sweep" ;;
		audio)     echo "Audio codec (speaker / mic / headphone jack)" ;;
		leds)      echo "LEDC RGB array (17 LEDs, colour order)" ;;
		wifi)      echo "WiFi (AIC8800 SDIO on mmc1)" ;;
		bluetooth) echo "Bluetooth (AIC8800 UART / hci0)" ;;
		usb)       echo "USB host / gadget / USB-C DP alt-mode" ;;
		battery)   echo "Battery / charger" ;;
		thermal)   echo "Thermal zones + PWM fan" ;;
		cpufreq)   echo "CPU frequency / OPP (little + big cluster)" ;;
		*)         echo "$1" ;;
	esac
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
	begin_test gamepad "Identify the D-pad/ABXY/L/R source (open question: gpio-keys? USB/i2c MCU? hidraw?) and confirm the buttons register."
	cap "cat /proc/bus/input/devices 2>/dev/null"
	cap "ls -l /dev/input/ 2>/dev/null"
	cap "ls -l /dev/hidraw* /dev/ttyS* 2>/dev/null"
	have lsusb && cap "lsusb" || say "  (lsusb absent — install usbutils to see an internal USB gamepad MCU)"
	cap "dmesg 2>/dev/null | grep -iE 'input:|gamepad|joystick|hid|gpio-key' | tail -20"
	_ev=""; [ "$DEMO" != 1 ] && _ev=$(find_event "gamepad\\|joystick\\|controller\\|pad\\|trimui\\|hid")
	if [ -n "$_ev" ]; then
		say "  Candidate gamepad device = /dev/input/$_ev"
		have evtest && manual "evtest gamepad" "evtest /dev/input/$_ev   # press D-pad, A/B/X/Y, L/R, Start/Select"
	else
		say "  No obvious gamepad input node — inspect /proc/bus/input/devices above."
	fi
	pause "  Press Enter after exercising every button in evtest..."
	_src=$(askval "What is the kernel source of the pad? (usb-hid / i2c-mcu / gpio-keys / platform / unknown)" "unknown")
	_ok=$(askval "Did every button register? (yes/some/no)" "yes")
	calib "gamepad source = ${_src}; node = /dev/input/${_ev:-?}  (informs the DT/driver decision — do NOT fabricate gpio-keys)"
	_dflt=SKIP; [ "$INTERACTIVE" = 1 ] && [ "$_ok" = yes ] && _dflt=PASS
	finish "$(verdict "$_dflt")" "source=${_src}; node=/dev/input/${_ev:-?}; buttons=${_ok}" \
		"gamepad input node/driver (open question in PORTING-NOTES §1); NOT gpio-keys" \
		"Vendor uses userspace trimui_inputd over an internal MCU; capture what the kernel actually exposes."
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
	say "  For DP-out: plug a USB-C DisplayPort sink; expect an alt-mode with SVID 0xff01."
	_host=$(askval "Did the USB host port enumerate a device? (yes/no)" "yes")
	_dp=$(askval "USB-C DisplayPort alt-mode (svid ff01) present? (yes/no/skip)" "skip")
	calib "USB: otg dr_mode=otg; DP alt-mode svid 0xff01 via husb311 TCPC + ps8743 mux (DP-out is Phase-6+)"
	_dflt=SKIP; [ "$INTERACTIVE" = 1 ] && [ "$_host" = yes ] && _dflt=PASS
	finish "$(verdict "$_dflt")" "host-enum=${_host}; dp-altmode=${_dp}" \
		"&usb_otg / &ehci0-1 / &ohci0-1 / &usbphy; typec DP alt-mode (husb311 + ps8743)" \
		"USB2 data + PD charging work now; DP-out depends on the display stack (Phase 6)."
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
		identity)  t_identity ;;
		pmic)      t_pmic ;;
		lradc)     t_lradc ;;
		gamepad)   t_gamepad ;;
		sticks)    t_sticks ;;
		display)   t_display ;;
		audio)     t_audio ;;
		leds)      t_leds ;;
		wifi)      t_wifi ;;
		bluetooth) t_bluetooth ;;
		usb)       t_usb ;;
		battery)   t_battery ;;
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
		for id in $TEST_IDS; do
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
		_i=0
		for _id in $TEST_IDS; do
			_i=$((_i + 1))
			printf '  %2d) %-9s %s\n' "$_i" "$_id" "$(test_title "$_id")"
		done
		printf '   a) run ALL    r) show report path    q) finish & quit\n'
		printf 'select> '
		read -r _sel || break
		case "$_sel" in
			q|Q|'') break ;;
			a|A) for _id in $TEST_IDS; do run_test "$_id"; done ;;
			r|R) printf 'report: %s\n' "$REPORT" ;;
			*)
				_id=$(id_for_num "$_sel")
				if [ -n "$_id" ]; then
					run_test "$_id"
				else
					case " $TEST_IDS " in
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
$SELF — interactive on-device hardware verification for the Trimui Smart Pro S
(Allwinner A523). Guides you through exercising each subsystem and writes a
mainlining-ready Markdown report. Companion to the read-only recon.sh.

Usage:
  sh $SELF [options] [test ...]

Options:
  -h, --help        this help
  -l, --list        list the test ids and titles
  -a, --all         run every test in order (non-interactive-friendly)
  -d, --demo        no-device self-test: emit a PENDING report skeleton
  -y, --yes         non-interactive (auto-skip all prompts) even on a TTY
  -o, --output FILE report path (default: /tmp/trimui-hw-report-<timestamp>.md)

With no test ids and a TTY, an interactive menu opens (pick / re-run / skip).
Given test ids, only those run, e.g.:  sh $SELF pmic sticks

Notes:
  * POSIX sh / busybox-ash safe. Mostly read-only; the only writes are explicit
    backlight/LED/fan sweeps (and only if the sysfs node is writable), each of
    which restores the original value. It never flashes or repartitions.
  * If a tool is missing it prints how to install it and SKIPs the test.
  * If stdin is not a TTY, every prompt auto-skips (safe to pipe / run in CI).
EOF
}

list_tests() {
	for _id in $TEST_IDS; do printf '  %-9s %s\n' "$_id" "$(test_title "$_id")"; done
}

# ============================================================================
# main
# ============================================================================
REPORT=""
RUN_ALL=0
ARGS_TESTS=""

while [ $# -gt 0 ]; do
	case "$1" in
		-h|--help)   usage; exit 0 ;;
		-l|--list)   list_tests; exit 0 ;;
		-a|--all)    RUN_ALL=1 ;;
		-d|--demo)   DEMO=1; INTERACTIVE=0 ;;
		-y|--yes)    INTERACTIVE=0 ;;
		-o|--output) shift; REPORT=$1 ;;
		--output=*)  REPORT=${1#--output=} ;;
		-*)          say "unknown option: $1 (see --help)"; exit 2 ;;
		*)           ARGS_TESTS="$ARGS_TESTS $1" ;;
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
	for id in $TEST_IDS; do run_test "$id"; done
elif [ -n "$ARGS_TESTS" ]; then
	for id in $ARGS_TESTS; do run_test "$id"; done
elif [ "$RUN_ALL" = 1 ]; then
	for id in $TEST_IDS; do run_test "$id"; done
elif [ "$INTERACTIVE" = 1 ]; then
	menu
else
	# non-interactive, no explicit tests: run all, prompts auto-skip.
	for id in $TEST_IDS; do run_test "$id"; done
fi

report_finalize

say ""
say "=========================================================="
say "Report written to: $REPORT"
say "  (copy it off the device, e.g.  adb pull $REPORT )"
say "=========================================================="
