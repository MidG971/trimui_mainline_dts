// SPDX-License-Identifier: GPL-2.0-only
/*
 * Trimui Smart Pro S gamepad — serdev input driver.
 *
 * Copyright (C) 2026 Midgy BALON
 *
 * The pad is streamed by two 19200-baud, RX-only serial MCUs:
 *   uart5 (ttyAS5, PK17) carries the LEFT half  (D-pad, L1/L2/L3, Menu, left stick)
 *   uart7 (ttyAS7, PK13) carries the RIGHT half (A/B/X/Y, Select/Start, R1/R2/R3,
 *                                                right stick)
 * Each MCU emits a fixed 19-byte frame at a steady rate and fills only its own
 * half of the shared button bitmask (the other half reads 0). On stock, the
 * vendor daemon trimui_inputd merges both into ONE virtual uinput device that
 * spoofs a wired Xbox 360 pad (045e:028e "TRIMUI Player1"); SDL2's
 * gamecontrollerdb keys off that VID:PID so every emulator/frontend maps it with
 * zero config. This driver reproduces that single-pad contract natively.
 *
 * Wire protocol (reverse-engineered from trimui_inputd, see
 * docs/GAMEPAD-PROTOCOL.md; HW-verified 2026-09-25):
 *   [0]   = 0xFF header
 *   [1]   = 0x01
 *   [2:5] = u32 LE button bitmask
 *   [6:7] = left  stick X (u16 LE, 12-bit Hall ADC)   (LEFT MCU)
 *   [8:9] = left  stick Y                              (LEFT MCU)
 *   [10:11] = right stick X                            (RIGHT MCU)
 *   [12:13] = right stick Y                            (RIGHT MCU)
 *   [18]  = 0xFE footer
 * Button bitmask bits: B=0 Y=1 Select=2 Start=3 Up=4 Down=5 Left=6 Right=7
 *   A=8 X=9 L1=10 R1=11 L2=12 R2=13 L3=14 R3=15 Menu=16. L2/R2 are digital on
 *   this hardware (0/255 only). Sticks are 12-bit Hall centred ~2048, scaled here
 *   to the Xbox ±32767 range with a 10% deadzone; Y axes are inverted (Xbox up =
 *   negative). Home=LRADC, Fn=gpio, Power=AXP PEK — NOT on this stream.
 *
 * Single-pad merge: serdev nodes must be direct children of their UART
 * controllers, so the two MCUs cannot share a DT parent node. Instead the two
 * driver instances share one input_dev via a kref'd module-level singleton: the
 * first to probe builds and registers the pad, the second reuses it, and it is
 * torn down when the last instance unbinds. (A DT phandle linking the two nodes
 * would be the alternative; the singleton keeps the binding trivial since there
 * is exactly one gamepad on this board.)
 */

#include <linux/input.h>
#include <linux/kref.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/pwm.h>
#include <linux/regulator/consumer.h>
#include <linux/serdev.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#define TRIMUI_GP_BAUD		19200	/* trimui_inputd SetupSerial: 19200 8N1 */
#define TRIMUI_GP_FRAME_LEN	19	/* 0xFF hdr @0 ... 0xFE ftr @18 */
#define TRIMUI_GP_HDR		0xFF
#define TRIMUI_GP_FTR_OFF	18
#define TRIMUI_GP_FTR		0xFE
#define TRIMUI_GP_RXBUF		64	/* a few frames of slack */
#define TRIMUI_GP_AXMAX		32767	/* Xbox 360 stick full scale */
#define TRIMUI_GP_DZ_NUM	1	/* 10% deadzone = 1/10 of travel */
#define TRIMUI_GP_DZ_DEN	10

enum trimui_gp_side {
	TRIMUI_GP_LEFT,
	TRIMUI_GP_RIGHT,
};

struct trimui_gp_variant {
	enum trimui_gp_side side;
	const char *label;
};

/* 12-bit Hall calibration (min, centre, max) from vendor joypad*.config. */
struct trimui_gp_cal {
	int lo, ctr, hi;
};

static const struct trimui_gp_cal trimui_gp_cal_lx = {  344, 1958, 3655 };
static const struct trimui_gp_cal trimui_gp_cal_ly = {  315, 1713, 3231 };
static const struct trimui_gp_cal trimui_gp_cal_rx = {  539, 2032, 3790 };
static const struct trimui_gp_cal trimui_gp_cal_ry = {  101, 1836, 3774 };

/* The one shared pad, reference-counted across the two serdev instances. */
struct trimui_gp_pad {
	struct input_dev *input;
	struct kref kref;
	/*
	 * Rumble motor: PWM0 ch7 + a GPIO-gated supply, wired on the LEFT node
	 * (the motor is part of the pad). FF_RUMBLE on the shared input_dev
	 * drives it, so games rumble the pad directly. @pwm is the readiness
	 * gate — NULL until the LEFT instance has acquired both resources.
	 */
	struct pwm_device *pwm;
	struct regulator *vcc;
	struct work_struct rumble_work;
	u16 level;
	bool vcc_on;
};

struct trimui_gp {
	struct serdev_device *serdev;
	struct trimui_gp_pad *pad;
	enum trimui_gp_side side;
	u8 buf[TRIMUI_GP_RXBUF];
	unsigned int len;
};

static DEFINE_MUTEX(trimui_gp_pad_lock);	/* guards the singleton below */
static struct trimui_gp_pad *trimui_gp_the_pad;

/*
 * Scale a raw 12-bit Hall reading to a centred Xbox axis value, applying a 10%
 * deadzone and rescaling the remaining travel so full deflection still reaches
 * the end stop. Integer-only (no kernel FP); mirrors the vendor daemon's scale().
 */
static int trimui_gp_scale(int raw, const struct trimui_gp_cal *c)
{
	int span, num, sign;
	s64 out;

	if (raw <= c->ctr) {
		span = c->ctr - c->lo;
		num  = c->ctr - raw;
		sign = -1;
	} else {
		span = c->hi - c->ctr;
		num  = raw - c->ctr;
		sign = 1;
	}
	if (span <= 0)
		return 0;
	/* inside the deadzone (|travel| < 10%) -> report centred */
	if (num * TRIMUI_GP_DZ_DEN <= span * TRIMUI_GP_DZ_NUM)
		return 0;
	/* out = AXMAX * (travel - 10%) / 90%, computed on the raw span */
	out = (s64)TRIMUI_GP_AXMAX *
	      (TRIMUI_GP_DZ_DEN * num - TRIMUI_GP_DZ_NUM * span);
	out = div_s64(out, (TRIMUI_GP_DZ_DEN - TRIMUI_GP_DZ_NUM) * span);
	if (out > TRIMUI_GP_AXMAX)	/* raw beyond calibration range */
		out = TRIMUI_GP_AXMAX;
	return sign * (int)out;
}

static inline u32 trimui_gp_mask(const u8 *f)
{
	return f[2] | (f[3] << 8) | (f[4] << 16) | (f[5] << 24);
}

static inline int trimui_gp_u16(const u8 *f, int off)
{
	return f[off] | (f[off + 1] << 8);
}

static void trimui_gp_report_left(struct input_dev *in, const u8 *f)
{
	u32 m = trimui_gp_mask(f);
	int up    = (m >> 4) & 1;
	int down  = (m >> 5) & 1;
	int left  = (m >> 6) & 1;
	int right = (m >> 7) & 1;

	input_report_abs(in, ABS_HAT0X, right - left);		/* -1 L / +1 R */
	input_report_abs(in, ABS_HAT0Y, down - up);		/* -1 U / +1 D */
	input_report_key(in, BTN_TL,     (m >> 10) & 1);	/* L1 */
	input_report_key(in, BTN_THUMBL, (m >> 14) & 1);	/* L3 */
	input_report_key(in, BTN_MODE,   (m >> 16) & 1);	/* Menu/Guide */
	input_report_abs(in, ABS_Z, ((m >> 12) & 1) ? 255 : 0);	/* L2 digital */
	input_report_abs(in, ABS_X,  trimui_gp_scale(trimui_gp_u16(f, 6),
						     &trimui_gp_cal_lx));
	input_report_abs(in, ABS_Y, -trimui_gp_scale(trimui_gp_u16(f, 8),
						     &trimui_gp_cal_ly));
	input_sync(in);
}

static void trimui_gp_report_right(struct input_dev *in, const u8 *f)
{
	u32 m = trimui_gp_mask(f);

	input_report_key(in, BTN_B,      (m >> 0)  & 1);
	input_report_key(in, BTN_Y,      (m >> 1)  & 1);
	input_report_key(in, BTN_SELECT, (m >> 2)  & 1);
	input_report_key(in, BTN_START,  (m >> 3)  & 1);
	input_report_key(in, BTN_A,      (m >> 8)  & 1);
	input_report_key(in, BTN_X,      (m >> 9)  & 1);
	input_report_key(in, BTN_TR,     (m >> 11) & 1);	/* R1 */
	input_report_key(in, BTN_THUMBR, (m >> 15) & 1);	/* R3 */
	input_report_abs(in, ABS_RZ, ((m >> 13) & 1) ? 255 : 0);/* R2 digital */
	input_report_abs(in, ABS_RX,  trimui_gp_scale(trimui_gp_u16(f, 10),
						      &trimui_gp_cal_rx));
	input_report_abs(in, ABS_RY, -trimui_gp_scale(trimui_gp_u16(f, 12),
						      &trimui_gp_cal_ry));
	input_sync(in);
}

/* serdev RX: accumulate, frame on 0xFF@0 + 0xFE@18, report each whole frame. */
static size_t trimui_gp_receive(struct serdev_device *serdev,
				const u8 *data, size_t count)
{
	struct trimui_gp *gp = serdev_device_get_drvdata(serdev);
	size_t n = min(count, sizeof(gp->buf) - gp->len);

	memcpy(gp->buf + gp->len, data, n);
	gp->len += n;

	while (gp->len >= TRIMUI_GP_FRAME_LEN) {
		const u8 *f = gp->buf;

		if (f[0] != TRIMUI_GP_HDR || f[TRIMUI_GP_FTR_OFF] != TRIMUI_GP_FTR) {
			/* not aligned here: drop one byte and re-scan */
			memmove(gp->buf, gp->buf + 1, --gp->len);
			continue;
		}
		if (gp->side == TRIMUI_GP_LEFT)
			trimui_gp_report_left(gp->pad->input, f);
		else
			trimui_gp_report_right(gp->pad->input, f);
		gp->len -= TRIMUI_GP_FRAME_LEN;
		memmove(gp->buf, gp->buf + TRIMUI_GP_FRAME_LEN, gp->len);
	}
	return count;
}

static const struct serdev_device_ops trimui_gp_serdev_ops = {
	.receive_buf = trimui_gp_receive,
};

/* ---- rumble (FF_RUMBLE on the shared pad -> PWM motor) ---- */

static void trimui_gp_rumble_on(struct trimui_gp_pad *pad)
{
	struct pwm_state state;

	if (!pad->vcc_on && regulator_enable(pad->vcc) == 0)
		pad->vcc_on = true;
	/* init from DT args (period + polarity); get_state can return period 0
	 * on a freshly-acquired channel, which makes the apply a no-op */
	pwm_init_state(pad->pwm, &state);
	pwm_set_relative_duty_cycle(&state, pad->level, 0xffff);
	state.enabled = true;
	pwm_apply_might_sleep(pad->pwm, &state);
}

static void trimui_gp_rumble_off(struct trimui_gp_pad *pad)
{
	pwm_disable(pad->pwm);
	if (pad->vcc_on) {
		regulator_disable(pad->vcc);
		pad->vcc_on = false;
	}
}

/* runs in process context; the play callback below is atomic so it defers here */
static void trimui_gp_rumble_work(struct work_struct *work)
{
	struct trimui_gp_pad *pad = container_of(work, struct trimui_gp_pad,
						 rumble_work);

	if (!pad->pwm)
		return;
	if (pad->level)
		trimui_gp_rumble_on(pad);
	else
		trimui_gp_rumble_off(pad);
}

static int trimui_gp_play(struct input_dev *in, void *data,
			  struct ff_effect *effect)
{
	struct trimui_gp_pad *pad = input_get_drvdata(in);
	u16 mag;

	/* single motor: use the stronger of the two magnitudes */
	if (!smp_load_acquire(&pad->pwm))
		return 0;	/* LEFT node not bound yet: no motor */
	mag = effect->u.rumble.strong_magnitude;
	if (!mag)
		mag = effect->u.rumble.weak_magnitude;
	pad->level = mag;
	schedule_work(&pad->rumble_work);
	return 0;
}

/* Acquire the motor from the LEFT node's DT (pwm0 ch7 + vcc-supply). */
static void trimui_gp_rumble_acquire(struct trimui_gp_pad *pad,
				     struct device *dev)
{
	struct regulator *vcc;
	struct pwm_device *pwm;

	pwm = pwm_get(dev, NULL);
	if (IS_ERR(pwm)) {
		dev_warn(dev, "no rumble pwm (%ld); rumble disabled\n",
			 PTR_ERR(pwm));
		return;
	}
	vcc = regulator_get(dev, "vcc");
	if (IS_ERR(vcc)) {
		dev_warn(dev, "no rumble vcc-supply (%ld); rumble disabled\n",
			 PTR_ERR(vcc));
		pwm_put(pwm);
		return;
	}
	pad->vcc = vcc;
	smp_store_release(&pad->pwm, pwm);	/* publish: @pwm gates trimui_gp_play */
	dev_info(dev, "rumble motor wired (pwm + vcc-supply)\n");
}

/* ---- shared-pad (singleton) lifecycle ---- */

static int trimui_gp_build_pad(struct trimui_gp_pad *pad)
{
	struct input_dev *in;
	int ret;

	in = input_allocate_device();
	if (!in)
		return -ENOMEM;

	in->name = "TRIMUI Player1";
	in->phys = "trimui-smart-pro-s/input0";
	in->id.bustype = BUS_USB;
	in->id.vendor  = 0x045e;	/* spoof wired Xbox 360 for SDL mapping */
	in->id.product = 0x028e;
	in->id.version = 0x0114;

	input_set_capability(in, EV_KEY, BTN_A);
	input_set_capability(in, EV_KEY, BTN_B);
	input_set_capability(in, EV_KEY, BTN_X);
	input_set_capability(in, EV_KEY, BTN_Y);
	input_set_capability(in, EV_KEY, BTN_TL);
	input_set_capability(in, EV_KEY, BTN_TR);
	input_set_capability(in, EV_KEY, BTN_SELECT);
	input_set_capability(in, EV_KEY, BTN_START);
	input_set_capability(in, EV_KEY, BTN_MODE);
	input_set_capability(in, EV_KEY, BTN_THUMBL);
	input_set_capability(in, EV_KEY, BTN_THUMBR);

	input_set_abs_params(in, ABS_X,  -TRIMUI_GP_AXMAX, TRIMUI_GP_AXMAX, 0, 0);
	input_set_abs_params(in, ABS_Y,  -TRIMUI_GP_AXMAX, TRIMUI_GP_AXMAX, 0, 0);
	input_set_abs_params(in, ABS_RX, -TRIMUI_GP_AXMAX, TRIMUI_GP_AXMAX, 0, 0);
	input_set_abs_params(in, ABS_RY, -TRIMUI_GP_AXMAX, TRIMUI_GP_AXMAX, 0, 0);
	input_set_abs_params(in, ABS_Z,  0, 255, 0, 0);
	input_set_abs_params(in, ABS_RZ, 0, 255, 0, 0);
	input_set_abs_params(in, ABS_HAT0X, -1, 1, 0, 0);
	input_set_abs_params(in, ABS_HAT0Y, -1, 1, 0, 0);

	/* FF_RUMBLE -> PWM motor (acquired later by the LEFT node). Must be set
	 * up before input_register_device(); failure is non-fatal (no rumble). */
	input_set_drvdata(in, pad);
	INIT_WORK(&pad->rumble_work, trimui_gp_rumble_work);
	input_set_capability(in, EV_FF, FF_RUMBLE);
	ret = input_ff_create_memless(in, NULL, trimui_gp_play);
	if (ret)
		pr_warn("gamepad-trimui-smart-pro-s: FF setup failed (%d)\n", ret);

	ret = input_register_device(in);
	if (ret) {
		input_free_device(in);
		return ret;
	}
	pad->input = in;
	return 0;
}

static void trimui_gp_pad_release(struct kref *kref)
{
	struct trimui_gp_pad *pad = container_of(kref, struct trimui_gp_pad, kref);

	/* called by kref_put_mutex() with trimui_gp_pad_lock held */
	trimui_gp_the_pad = NULL;
	input_unregister_device(pad->input);	/* stops FF + frees the input_dev */
	cancel_work_sync(&pad->rumble_work);
	if (pad->pwm) {
		trimui_gp_rumble_off(pad);
		regulator_put(pad->vcc);
		pwm_put(pad->pwm);
	}
	kfree(pad);
}

static void trimui_gp_pad_put(struct trimui_gp_pad *pad)
{
	if (kref_put_mutex(&pad->kref, trimui_gp_pad_release, &trimui_gp_pad_lock))
		mutex_unlock(&trimui_gp_pad_lock);
}

/* Get the shared pad, building it on first use. */
static struct trimui_gp_pad *trimui_gp_pad_get(void)
{
	struct trimui_gp_pad *pad;
	int ret;

	mutex_lock(&trimui_gp_pad_lock);
	if (trimui_gp_the_pad) {
		pad = trimui_gp_the_pad;
		kref_get(&pad->kref);
		mutex_unlock(&trimui_gp_pad_lock);
		return pad;
	}

	pad = kzalloc(sizeof(*pad), GFP_KERNEL);
	if (!pad) {
		mutex_unlock(&trimui_gp_pad_lock);
		return ERR_PTR(-ENOMEM);
	}
	ret = trimui_gp_build_pad(pad);
	if (ret) {
		kfree(pad);
		mutex_unlock(&trimui_gp_pad_lock);
		return ERR_PTR(ret);
	}
	kref_init(&pad->kref);
	trimui_gp_the_pad = pad;
	mutex_unlock(&trimui_gp_pad_lock);
	return pad;
}

/* ---- probe / remove ---- */

static int trimui_gp_probe(struct serdev_device *serdev)
{
	struct device *dev = &serdev->dev;
	const struct trimui_gp_variant *v;
	struct trimui_gp *gp;
	u32 baud = TRIMUI_GP_BAUD;
	int ret;

	v = of_device_get_match_data(dev);
	if (!v)
		return -EINVAL;

	gp = devm_kzalloc(dev, sizeof(*gp), GFP_KERNEL);
	if (!gp)
		return -ENOMEM;

	gp->serdev = serdev;
	gp->side = v->side;

	gp->pad = trimui_gp_pad_get();
	if (IS_ERR(gp->pad))
		return dev_err_probe(dev, PTR_ERR(gp->pad),
				     "failed to create shared pad\n");

	/* the LEFT node carries the rumble motor's pwm + vcc-supply */
	if (v->side == TRIMUI_GP_LEFT && !gp->pad->pwm)
		trimui_gp_rumble_acquire(gp->pad, dev);

	serdev_device_set_drvdata(serdev, gp);
	serdev_device_set_client_ops(serdev, &trimui_gp_serdev_ops);

	ret = serdev_device_open(serdev);
	if (ret) {
		dev_err_probe(dev, ret, "failed to open serdev\n");
		goto err_put;
	}

	device_property_read_u32(dev, "current-speed", &baud);
	serdev_device_set_baudrate(serdev, baud);
	serdev_device_set_flow_control(serdev, false);

	dev_info(dev, "Trimui Smart Pro S gamepad (%s) @ %u baud\n",
		 v->label, baud);
	return 0;

err_put:
	trimui_gp_pad_put(gp->pad);
	return ret;
}

static void trimui_gp_remove(struct serdev_device *serdev)
{
	struct trimui_gp *gp = serdev_device_get_drvdata(serdev);

	serdev_device_close(serdev);	/* stop RX before dropping the pad */
	trimui_gp_pad_put(gp->pad);
}

static const struct trimui_gp_variant trimui_gp_left = {
	.side = TRIMUI_GP_LEFT, .label = "left",
};
static const struct trimui_gp_variant trimui_gp_right = {
	.side = TRIMUI_GP_RIGHT, .label = "right",
};

static const struct of_device_id trimui_gp_of_match[] = {
	{ .compatible = "trimui,smart-pro-s-gamepad-left",  .data = &trimui_gp_left  },
	{ .compatible = "trimui,smart-pro-s-gamepad-right", .data = &trimui_gp_right },
	{ }
};
MODULE_DEVICE_TABLE(of, trimui_gp_of_match);

static struct serdev_device_driver trimui_gp_driver = {
	.probe	= trimui_gp_probe,
	.remove	= trimui_gp_remove,
	.driver	= {
		.name		= "gamepad-trimui-smart-pro-s",
		.of_match_table	= trimui_gp_of_match,
	},
};
module_serdev_device_driver(trimui_gp_driver);

MODULE_AUTHOR("Midgy BALON");
MODULE_DESCRIPTION("Trimui Smart Pro S serdev gamepad (merged Xbox 360 pad)");
MODULE_LICENSE("GPL");
