// SPDX-License-Identifier: GPL-2.0-only
/*
 * Trimui Smart Pro S gamepad — serdev input driver (SKELETON).
 *
 * Copyright (C) 2026 Midgy BALON
 *
 * The pad is streamed by two 9600-baud, RX-only serial MCUs:
 *   uart5 (/dev/ttyAS5, PK17) and uart7 (/dev/ttyAS7, PK13).
 * Each MCU emits a fixed-length frame at a steady rate; together they carry the
 * whole pad (D-pad, ABXY, L1/R1, L2/R2 analog, L3/R3, both sticks, rumble back-
 * channel). On stock the vendor daemon trimui_inputd reads them and republishes
 * a virtual Xbox360 uinput pad; this driver replaces that with a native serdev
 * input device.
 *
 * ==========================================================================
 *  STATUS: SKELETON. The wire protocol is not yet reverse-engineered on this
 *  tree. Everything marked TODO(protocol-map) must be filled from the output of
 *  gamepad-logger/analyze-gamepad.py (protocol-map.json): frame length, sync
 *  byte, per-button byte+bit, per-axis byte+range, and which controls each MCU
 *  (uart5 vs uart7) carries. Until then the parser is a stub that only frame-
 *  syncs and counts frames (dev_dbg), reporting no events.
 * ==========================================================================
 *
 * Design note — one pad vs two: each MCU carries roughly one half of the pad,
 * but games expect ONE controller. This skeleton registers one input device per
 * MCU (simplest, self-contained). For the final driver, merge to a single pad —
 * e.g. a shared context looked up via a DT phandle between the two nodes, or a
 * small parent/child arrangement — decided once the split of controls is known.
 */

#include <linux/input.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/serdev.h>
#include <linux/slab.h>

#define TRIMUI_GP_BAUD		19200	/* vendor trimui_inputd SetupSerial: 19200 8N1 */
#define TRIMUI_GP_RXBUF		256	/* a few frames of slack */
#define TRIMUI_GP_FRAME_LEN	19	/* 0xFF hdr @0 ... 0xFE ftr @18 (period 19) */
#define TRIMUI_GP_HDR		0xFF
#define TRIMUI_GP_FTR_OFF	18
#define TRIMUI_GP_FTR		0xFE

/* One control-map entry. Filled from protocol-map.json per MCU side. */
struct trimui_gp_btn {
	u16 code;	/* KEY_*/BTN_* */
	u8  byte;	/* frame byte offset */
	u8  bit;	/* bit within that byte (active-? see .active_low) */
};

struct trimui_gp_axis {
	u16 code;	/* ABS_* */
	u8  byte;	/* frame byte offset of the u16 LE (12-bit ADC) value */
	u16 min, max;	/* raw calibration range (joypad.config) */
	bool invert;
};

/*
 * Per-side (per-MCU) descriptor. Selected by of_device_id.data. All values are
 * TODO(protocol-map) placeholders — DO NOT trust until a capture confirms them.
 */
struct trimui_gp_desc {
	const char *name;
	u8 frame_len;			/* TODO(protocol-map) fixed frame size */
	u8 sync_byte;			/* TODO(protocol-map) constant header byte */
	u8 sync_off;			/* offset of the sync byte in the frame */
	const struct trimui_gp_btn *btns;
	unsigned int nbtns;
	const struct trimui_gp_axis *axes;
	unsigned int naxes;
	bool active_low;		/* buttons pressed = bit clear? */
};

struct trimui_gp {
	struct serdev_device *serdev;
	struct input_dev *input;
	const struct trimui_gp_desc *desc;

	u8 buf[TRIMUI_GP_RXBUF];
	unsigned int len;		/* bytes currently buffered */
	bool synced;
};

/*
 * ---- protocol map (HW-verified 2026-09-25; RE'd from trimui_inputd) ----
 *
 * 19200 8N1, RX-only. LEFT=ttyAS5(ttyS2), RIGHT=ttyAS7(ttyS3). 19-byte frame:
 *   [0]=0xFF hdr  [1]=0x01  [2..5]=u32 LE button bitmask  [6:7]=Lstick X u16 LE
 *   [8:9]=Lstick Y  [10:11]=Rstick X  [12:13]=Rstick Y  [14:15]=analog trig (0 on
 *   this unit; L2/R2 are digital bits)  [16:17]=?  [18]=0xFE ftr.
 * Each MCU fills only its own half (buttons + its stick); other half's bytes = 0.
 * Button bits: B0 Y1 Sel2 Start3 Up4 Down5 Left6 Right7 A8 X9 L1_10 R1_11 L2_12
 *   R2_13 L3_14 R3_15 Menu16. byte(off)=2+bit/8, bit=bit%8.
 * Sticks are 12-bit ADC in the u16; calib ranges from joypad.config.
 * See docs/GAMEPAD-PROTOCOL.md. (Home=LRADC, Fn=PL11 gpio, Power=AXP — NOT here.)
 */

/* LEFT MCU (uart5): D-pad, L1/L2/L3, Menu + left stick. */
static const struct trimui_gp_btn trimui_gp_btns_left[] = {
	{ BTN_DPAD_UP,    2, 4 }, { BTN_DPAD_DOWN,  2, 5 },
	{ BTN_DPAD_LEFT,  2, 6 }, { BTN_DPAD_RIGHT, 2, 7 },
	{ BTN_TL,         3, 2 },	/* L1  = mask bit 10 */
	{ BTN_TL2,        3, 4 },	/* L2  = mask bit 12 (digital) */
	{ BTN_THUMBL,     3, 6 },	/* L3  = mask bit 14 */
	{ BTN_MODE,       4, 0 },	/* Menu = mask bit 16 */
};
static const struct trimui_gp_axis trimui_gp_axes_left[] = {
	{ ABS_X, 6, 344, 3655, false },	/* left stick X (joypad.config) */
	{ ABS_Y, 8, 315, 3231, false },	/* left stick Y */
};

/* RIGHT MCU (uart7): A/B/X/Y, Select/Start, R1/R2/R3 + right stick. */
static const struct trimui_gp_btn trimui_gp_btns_right[] = {
	{ BTN_B,      2, 0 }, { BTN_Y,      2, 1 },
	{ BTN_SELECT, 2, 2 }, { BTN_START,  2, 3 },
	{ BTN_A,      3, 0 },	/* mask bit 8 */
	{ BTN_X,      3, 1 },	/* mask bit 9 */
	{ BTN_TR,     3, 3 },	/* R1 = mask bit 11 */
	{ BTN_TR2,    3, 5 },	/* R2 = mask bit 13 (digital) */
	{ BTN_THUMBR, 3, 7 },	/* R3 = mask bit 15 */
};
static const struct trimui_gp_axis trimui_gp_axes_right[] = {
	{ ABS_RX, 10, 539, 3790, false },	/* right stick X (joypad_right.config) */
	{ ABS_RY, 12, 101, 3774, false },	/* right stick Y */
};

static const struct trimui_gp_desc trimui_gp_uart5 = {
	.name      = "Trimui Smart Pro S gamepad (left)",
	.frame_len = TRIMUI_GP_FRAME_LEN,	/* 19 */
	.sync_byte = TRIMUI_GP_HDR,		/* 0xFF @ off 0 (footer 0xFE @ 18) */
	.sync_off  = 0,
	.btns = trimui_gp_btns_left, .nbtns = ARRAY_SIZE(trimui_gp_btns_left),
	.axes = trimui_gp_axes_left, .naxes = ARRAY_SIZE(trimui_gp_axes_left),
	.active_low = false,		/* mask bit SET = pressed */
};

static const struct trimui_gp_desc trimui_gp_uart7 = {
	.name      = "Trimui Smart Pro S gamepad (right)",
	.frame_len = TRIMUI_GP_FRAME_LEN,	/* 19 */
	.sync_byte = TRIMUI_GP_HDR,		/* 0xFF @ off 0 */
	.sync_off  = 0,
	.btns = trimui_gp_btns_right, .nbtns = ARRAY_SIZE(trimui_gp_btns_right),
	.axes = trimui_gp_axes_right, .naxes = ARRAY_SIZE(trimui_gp_axes_right),
	.active_low = false,
};

/* ---- frame parse ---- */

static void trimui_gp_report_frame(struct trimui_gp *gp, const u8 *f)
{
	const struct trimui_gp_desc *d = gp->desc;
	unsigned int i;

	for (i = 0; i < d->nbtns; i++) {
		bool on = f[d->btns[i].byte] & BIT(d->btns[i].bit);

		if (d->active_low)
			on = !on;
		input_report_key(gp->input, d->btns[i].code, on);
	}
	for (i = 0; i < d->naxes; i++) {
		const struct trimui_gp_axis *a = &d->axes[i];
		int v = f[a->byte] | (f[a->byte + 1] << 8);	/* u16 LE, 12-bit ADC */

		if (a->invert)
			v = a->max - (v - a->min);
		input_report_abs(gp->input, a->code, v);
	}
	input_sync(gp->input);
}

/*
 * serdev RX. Accumulate, find the sync byte, and emit whole frames. This is a
 * minimal byte-stream framer; refine (checksum? per-frame counter?) once the
 * protocol is known.
 */
static ssize_t trimui_gp_receive(struct serdev_device *serdev,
				 const u8 *data, size_t count)
{
	struct trimui_gp *gp = serdev_device_get_drvdata(serdev);
	const struct trimui_gp_desc *d = gp->desc;
	size_t n = min(count, (size_t)(sizeof(gp->buf) - gp->len));

	memcpy(gp->buf + gp->len, data, n);
	gp->len += n;

	/* Frame on 0xFF header @0 AND 0xFE footer @18; emit each valid frame. */
	while (gp->len >= d->frame_len) {
		u8 *f = gp->buf;

		if (f[d->sync_off] != d->sync_byte ||
		    f[TRIMUI_GP_FTR_OFF] != TRIMUI_GP_FTR) {
			/* not a valid frame here: drop one byte and re-scan */
			memmove(gp->buf, gp->buf + 1, --gp->len);
			gp->synced = false;
			continue;
		}
		trimui_gp_report_frame(gp, f);
		gp->synced = true;
		memmove(gp->buf, gp->buf + d->frame_len, gp->len - d->frame_len);
		gp->len -= d->frame_len;
	}
	return count;
}

static const struct serdev_device_ops trimui_gp_serdev_ops = {
	.receive_buf  = trimui_gp_receive,
	.write_wakeup = serdev_device_write_wakeup,
};

/* ---- probe / remove ---- */

static int trimui_gp_probe(struct serdev_device *serdev)
{
	struct device *dev = &serdev->dev;
	struct trimui_gp *gp;
	unsigned int i;
	u32 baud = TRIMUI_GP_BAUD;
	int ret;

	gp = devm_kzalloc(dev, sizeof(*gp), GFP_KERNEL);
	if (!gp)
		return -ENOMEM;

	gp->serdev = serdev;
	gp->desc = of_device_get_match_data(dev);
	if (!gp->desc)
		return -EINVAL;

	serdev_device_set_drvdata(serdev, gp);
	serdev_device_set_client_ops(serdev, &trimui_gp_serdev_ops);

	ret = devm_serdev_device_open(dev, serdev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to open serdev\n");

	device_property_read_u32(dev, "current-speed", &baud);
	serdev_device_set_baudrate(serdev, baud);
	serdev_device_set_flow_control(serdev, false);

	gp->input = devm_input_allocate_device(dev);
	if (!gp->input)
		return -ENOMEM;

	gp->input->name = gp->desc->name;
	gp->input->phys = dev_name(dev);
	gp->input->id.bustype = BUS_HOST;
	gp->input->dev.parent = dev;

	for (i = 0; i < gp->desc->nbtns; i++)
		input_set_capability(gp->input, EV_KEY, gp->desc->btns[i].code);
	for (i = 0; i < gp->desc->naxes; i++)
		input_set_abs_params(gp->input, gp->desc->axes[i].code,
				     gp->desc->axes[i].min, gp->desc->axes[i].max,
				     0, 0);

	ret = input_register_device(gp->input);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register input\n");

	dev_info(dev, "%s: serdev gamepad @ %u baud (protocol %s)\n",
		 gp->desc->name, baud,
		 gp->desc->frame_len ? "mapped" : "STUB — fill protocol-map");
	return 0;
}

static const struct of_device_id trimui_gp_of_match[] = {
	{ .compatible = "trimui,smart-pro-s-gamepad-uart5", .data = &trimui_gp_uart5 },
	{ .compatible = "trimui,smart-pro-s-gamepad-uart7", .data = &trimui_gp_uart7 },
	{ }
};
MODULE_DEVICE_TABLE(of, trimui_gp_of_match);

static struct serdev_device_driver trimui_gp_driver = {
	.probe	= trimui_gp_probe,
	.driver	= {
		.name		= "gamepad-trimui-smart-pro-s",
		.of_match_table	= trimui_gp_of_match,
	},
};
module_serdev_device_driver(trimui_gp_driver);

MODULE_AUTHOR("Midgy BALON");
MODULE_DESCRIPTION("Trimui Smart Pro S serdev gamepad (skeleton)");
MODULE_LICENSE("GPL");
