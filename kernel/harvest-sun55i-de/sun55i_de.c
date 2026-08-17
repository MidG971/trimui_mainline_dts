// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Allwinner sun55iw3 v35x display engine RCQ backend.
 *
 * See sun55i_de.h for the architecture. This drives the minimal pipeline
 * (de_top control via MMIO; blender + output formatter via the RCQ DMA) needed
 * to scan out a solid background-color frame, as the foundation for layers.
 *
 * Register semantics are taken from the vendor BSP (disp2 lowlevel_v35x/de35x:
 * de_top.c, de_rtmx.c, de_bld.c, de_fmt.c) for sun55iw3 / de352.
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/hrtimer.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/moduleparam.h>
#include <linux/of_reserved_mem.h>
#include <linux/preempt.h>	/* in_hardirq() — TEMP instrumentation */
#include <linux/spinlock.h>

#include <drm/drm_atomic.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_vblank.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_modes.h>
#include <drm/drm_print.h>
#include <drm/drm_rect.h>

#include "sun8i_mixer.h"
#include "sun55i_de.h"
#include "sun55i_de_scaler.h"

/*
 * de_top control registers, relative to the "detop" regmap (DE base + 0x8000).
 * de_top.c: de_top_set_de2tcon_mux / de_top_set_rtmx_enable.
 */
#define SUN55I_DETOP_RESET		0x00	/* bit N: disp N core un-reset */
#define SUN55I_DETOP_CLK		0x04	/* bit N: disp N core clock */
#define SUN55I_DETOP_CLK_KEY		BIT(16)	/* vendor ORs it on every write */
#define SUN55I_DETOP_MBUS_CLK		0x08	/* bit0: DE master-port (DRAM) clk */
#define SUN55I_DETOP_DE2TCON_MUX	0x10	/* disp N @ bits[N*4 +:4] = TCON */
#define SUN55I_DETOP_UCH2CORE_MUX	0x24	/* phys UI chn -> core/disp mux */
#define SUN55I_DETOP_PORT2CHN_MUX	0x28	/* blender port -> phys chn mux */
#define SUN55I_DETOP_ASYNC_BRIDGE	0x4c
#define SUN55I_DETOP_BUF_DEPTH		0x50

#define SUN55I_DETOP_MBUS_CLK_EN	BIT(0)

/*
 * de_off (== RCQ head reg_offset, dest = DE base 0x5000000 + off) of the
 * datapath blocks. disp0: DE_DISP_OFFSET(0)=0x280000.
 */
#define SUN55I_DE_BLD_BASE		0x281000	/* +DISP_BLD_OFFSET 0x1000 */
#define SUN55I_DE_FMT_BASE		0x285000	/* +DISP_FMT_OFFSET 0x5000 */

/* blender, split into the same 3 blocks the vendor uses (de_bld.c) */
#define SUN55I_DE_BLD_ATTR_OFF		0x00	/* size 0x60: pipe.en + attrs  */
#define SUN55I_DE_BLD_ATTR_SIZE		0x60
#define SUN55I_DE_BLD_CTL_OFF		0x80	/* size 0x24: route/bg/outsize */
#define SUN55I_DE_BLD_CTL_SIZE		0x24
#define SUN55I_DE_BLD_CK_OFF		0xa0	/* size 0x60: colorkey/out_ctl */
#define SUN55I_DE_BLD_CK_SIZE		0x60

/* field offsets within the blender blocks (bld_reg, de_bld_type.h) */
#define SUN55I_BLD_ATTR_PIPE_EN		0x00	/* in ATTR block */
#define SUN55I_BLD_CTL_BG_COLOR		0x08	/* in CTL block (0x88-0x80) */
#define SUN55I_BLD_CTL_OUT_SIZE		0x0c	/* in CTL block (0x8c-0x80) */
#define SUN55I_BLD_CK_OUT_CTL		0x5c	/* in CK block  (0xfc-0xa0) */

/* output formatter (fmt_reg, de_fmt_type.h) */
#define SUN55I_DE_FMT_SIZE		0x2c	/* sizeof(struct fmt_reg) */
#define SUN55I_FMT_CTL			0x00	/* 0:RGB 1:YUV-csc */
#define SUN55I_FMT_SIZE_REG		0x04
#define SUN55I_FMT_SWAP			0x08
#define SUN55I_FMT_BITDEPTH		0x0c	/* 0:8-bit 1:10-bit */
#define SUN55I_FMT_TYPE			0x10	/* 0:444 1:422 2:420 */
#define SUN55I_FMT_LIMIT_Y		0x20
#define SUN55I_FMT_LIMIT_C0		0x24
#define SUN55I_FMT_LIMIT_C1		0x28
#define SUN55I_FMT_LIMIT_FULL		0x0fff0000	/* RGB/444 full range */

/*
 * UI overlay (de_ovl.c, struct ovl_u_reg) for the first UI channel.
 *
 * de352 logical->phys map = {0,6,7,8} (mainline cfg): the primary UI plane is
 * logical channel 1 = phys channel 6. de_top.h: DE_CHN_OFFSET(phy) = 0x100000 +
 * 0x20000*phy, CHN_OVL_OFFSET = 0x1000. So phys6 overlay base = 0x1c1000.
 */
#define SUN55I_DE_OVL_UI_PHYS_CHN	6
#define SUN55I_DE_OVL_UI_LOGIC_CHN	1	/* map[1]=6: blender route + port */
#define SUN55I_DE_OVL_BASE		0x1c1000

/*
 * RCQ blocks within the UI overlay (de_ovl.c:675-703):
 *   LAY_0 @ +0x00 size 0x1c, PARA @ +0x80 size 0x0c.
 * Only layer 0 + PARA are used (no scaling -> DS block left clean).
 */
#define SUN55I_DE_OVL_LAY_OFF(n)	((n) * 0x20)	/* LAY_n @ +0x00..0x60 */
#define SUN55I_DE_OVL_LAY0_OFF		0x00
#define SUN55I_DE_OVL_LAY0_SIZE		0x1c
#define SUN55I_DE_OVL_PARA_OFF		0x80
#define SUN55I_DE_OVL_PARA_SIZE		0x0c
#define SUN55I_DE_OVL_DS_OFF		0xe0	/* hori_ds/vert_ds (de_ovl.c:705) */
#define SUN55I_DE_OVL_DS_SIZE		0x1c

/*
 * VSU8 scaler for phys channel 6: DE_CHN_OFFSET(6) + CHN_SCALER_OFFSET =
 * 0x1c0000 + 0x4000 (de_top.h). de352 makes this channel a DE_SCALER_TYPE_VSU8.
 *
 * The v35x channel datapath is ALWAYS overlay -> VSU -> blender; the VSU is not
 * bypassable, it convolves every output pixel against its filter-coefficient
 * SRAM. After we power-gate/ungate PD_DE that SRAM powers up RANDOM, so a layer
 * with only the CTL block staged (en=0) is still filtered by garbage taps and
 * collapses to a single (per-boot-random) source column. The vendor only avoids
 * this because Android never power-gates the DE. We therefore program a real 1:1
 * passthrough: enable + in=out size + unity step + unity coefficients, exactly
 * as de_vsu8_set_para() does for a non-scaled RGB layer (de_vsu.c:677-779).
 *
 * The seven RCQ blocks and their de-offsets (struct vsu8_reg, de_vsu_type.h;
 * VSU8_REG_BLK_* in de_vsu.c):
 */
#define SUN55I_DE_VSU_BASE		0x1c4000
#define SUN55I_DE_VSU_CTL_OFF		0x000	/* ctl.en + scale_mode */
#define SUN55I_DE_VSU_CTL_SIZE		0x14
#define SUN55I_DE_VSU_ATTR_OFF		0x040	/* out_size + glb_alpha */
#define SUN55I_DE_VSU_ATTR_SIZE		0x08
#define SUN55I_DE_VSU_YPARA_OFF		0x080	/* y in_size/step/phase */
#define SUN55I_DE_VSU_CPARA_OFF		0x0c0	/* c in_size/step/phase */
#define SUN55I_DE_VSU_PARA_SIZE		0x1c
#define SUN55I_DE_VSU_COEFF0_OFF	0x200	/* y_hori_coeff[32] */
#define SUN55I_DE_VSU_COEFF1_OFF	0x400	/* y_vert_coeff[32] */
#define SUN55I_DE_VSU_COEFF2_OFF	0x600	/* c_hori_coeff[32] */
#define SUN55I_DE_VSU_COEFF_SIZE	0x80

/* fields within the CTL / ATTR / *PARA blocks (struct vsu8_reg) */
#define SUN55I_VSU_CTL_EN		BIT(0)
#define SUN55I_VSU_ATTR_OUT_SIZE	0x00	/* in ATTR block (0x40) */
#define SUN55I_VSU_ATTR_GLB_ALPHA	0x04	/* in ATTR block (0x44) */
#define SUN55I_VSU_PARA_IN_SIZE		0x00	/* in *PARA block */
#define SUN55I_VSU_PARA_HSTEP		0x08
#define SUN55I_VSU_PARA_VSTEP		0x0c
/*
 * step.dwval = ratio << SUN55I_VSU_STEP_VALID_START_BIT (de_vsu.c). The ratio
 * is src/dst in SUN55I_VSU_STEP_FRAC_BITS fixed point (1.0 == 1 << 19), so a
 * 1:1 step lands at reg 1 << 20. The polyphase coefficients live in
 * sun55i_de_scaler.h (sun55i_vsu8_lan2_coef / sun55i_vsu8_coef_index).
 */
#define SUN55I_VSU_STEP_VALID_START_BIT	1

/*
 * Other per-channel sub-modules that sit in the phys-6 datapath and must be
 * disabled/bypassed for a 1:1 linear-RGB layer (de_rtmx_chn_layer_apply):
 *   TFBD tiled-FB decoder @ +CHN_TFBD_OFFSET 0x5400 (de_tfbd_disable: ctrl=0)
 *   channel CSC @ +CHN_CCSC_OFFSET 0x800 (de_ccsc_enable 0: ctl=0 bypass)
 *   CDC color/gamut @ +CHN_CDC_OFFSET 0x8000 (de_cdc_disable: ctl=0)
 * de352 marks TFBD and CDC supported on UCH0/phys6 (de352_feat.c); left stale
 * they mangle the fetch / apply a garbage colour transform. The CDC is the
 * channel colour-management block the vendor bypasses for an SDR RGB->RGB
 * layer (de_rtmx_chn_apply_csc: cdc_check_bypass == 1 -> de_cdc_disable).
 */
#define SUN55I_DE_TFBD_BASE		0x1c5400

/*
 * AFBD (ARM AFBC decoder) of UI channel phys 6: channel_base+0x5000.
 * Port of the BSP de_fbd_atw.c (struct fbd_u_reg, 0x58-byte UI block, RCQ).
 */
#define SUN55I_DE_AFBD_BASE		0x1c5000
#define SUN55I_AFBD_CTL			0x00
#define SUN55I_AFBD_CTL_EN		BIT(0)
#define SUN55I_AFBD_CTL_ALPHA_MODE(m)	((m) << 2)	/* 1=global 2=mixed */
#define SUN55I_AFBD_CTL_ALPHA(a)	((u32)(a) << 24)
#define SUN55I_AFBD_FMT_SEQ		0x04
#define SUN55I_AFBD_IMG_SIZE		0x08
#define SUN55I_AFBD_BLK_SIZE		0x0c
#define SUN55I_AFBD_SRC_CROP		0x10
#define SUN55I_AFBD_LAY_CROP		0x14
#define SUN55I_AFBD_FMT			0x18
#define SUN55I_AFBD_HD_LADDR		0x20
#define SUN55I_AFBD_HD_HADDR		0x24
#define SUN55I_AFBD_OVL_SIZE		0x30
#define SUN55I_AFBD_OVL_COOR		0x34
#define SUN55I_AFBD_BG_COLOR		0x38
#define SUN55I_AFBD_COLOR0		0x50
#define SUN55I_AFBD_COLOR1		0x54

#define SUN55I_AFBD_FMT_RGBA8888	0x02	/* FBD_RGBA8888 from the BSP */
#define SUN55I_DE_CCSC_BASE		0x1c0800
#define SUN55I_DE_CDC_BASE		0x1c8000

/* per-layer fields within the LAY_0 block (union ovl_u_lay_reg) */
#define SUN55I_OVL_LAY_ATTCTL		0x00
#define SUN55I_OVL_LAY_MBSIZE		0x04	/* crop (w-1)|(h-1)<<16 */
#define SUN55I_OVL_LAY_MBCOOR		0x08	/* layer-in-chn coord (0) */
#define SUN55I_OVL_LAY_PITCH		0x0c	/* byte stride */
#define SUN55I_OVL_LAY_TOP_LADDR	0x10	/* fb DMA addr [31:0] */
#define SUN55I_OVL_LAY_BOT_LADDR	0x14	/* 0 (progressive) */
/* fields within the PARA block (top_haddr @0x80, win_size @0x88) */
#define SUN55I_OVL_PARA_TOP_HADDR	0x00	/* fb addr high byte, per layer */
#define SUN55I_OVL_PARA_WIN_SIZE	0x08	/* channel out (w-1)|(h-1)<<16 */

/*
 * attctl (union ovl_u_attctl_reg): en[0] | alpha_mode[2:1] | fmt[12:8] |
 * glb_alpha[31:24]. Use global-alpha mode (1) with 0xff so the layer is opaque
 * regardless of any X/undefined alpha bits in the source pixels.
 */
#define SUN55I_OVL_ATTCTL_EN		BIT(0)
#define SUN55I_OVL_ATTCTL_ALPHA_MODE(m)	(((m) & 0x3) << 1)
#define SUN55I_OVL_ATTCTL_GLOBAL_ALPHA	1
#define SUN55I_OVL_ATTCTL_FMT(f)	(((f) & 0x1f) << 8)
#define SUN55I_OVL_ATTCTL_ALPHA(a)	(((a) & 0xff) << 24)

/* blender pipe fields (de_bld.c / struct bld_reg) for showing the channel */
#define SUN55I_BLD_ATTR_PIPE_IN_SIZE(p)	(0x08 + (p) * 0x10)	/* ATTR block */
#define SUN55I_BLD_ATTR_PIPE_IN_COORD(p) (0x0c + (p) * 0x10)	/* ATTR block */
#define SUN55I_BLD_CTL_ROUT		0x00	/* CTL block: rout_ctl @0x80 */
#define SUN55I_BLD_CTL_BLEND(p)		(0x10 + (p) * 0x04)	/* blend_ctl[p] */
#define SUN55I_BLD_PIPE_EN(p)		(BIT(0) | BIT(8 + (p)))
#define SUN55I_BLD_ROUT(p, chn)		(((chn) & 0xf) << ((p) << 2))
#define SUN55I_BLD_BLEND_SRCOVER	0x03010301

#define SUN55I_DE_OUT_SIZE(w, h) \
	((((w) - 1) & 0x1fff) | ((((h) - 1) & 0x1fff) << 16))

#define SUN55I_RCQ_ALIGN		32

/*
 * CCU DE_BGR: bit0 = BUS_DE gate (held enabled by de33-clk, never touched
 * here), bit16 = RST_BUS_DE, active low - the reset of the ENTIRE DE complex
 * (cores, detop, de33-clk block). See the @bus_reset comment in sun55i_de.h.
 */
#define SUN55I_CCU_DE_BGR_PHYS		0x0200160c
#define SUN55I_CCU_DE_BGR_RST		BIT(16)

/*
 * TEMP tunable: the TCON scanline the deferred arm aims for, and the latest
 * line at which an in-IRQ arm is still considered safe. <0 derives both from
 * the mode (the middle of the leading vertical-blanking region). Exposed so the
 * arm point can be walked across the blanking window on hardware without a
 * rebuild; read back the de->arm_* counters to confirm where arms land.
 */
static int arm_target_line = -1;
module_param(arm_target_line, int, 0644);
MODULE_PARM_DESC(arm_target_line,
		 "v35x RCQ arm target scanline; <0 = derive from mode");

/*
 * The TV-TCON programs its vertical total as ver_total*2 for progressive modes
 * (vendor tcon1_cfg; mainline sun4i_tcon: V_TOTAL(crtc_vtotal * 2)), so the line
 * counter (TCON+0xfc) runs 0..2*vtotal. This matches the vendor's own arm-target
 * math (disp_mgr_protect_reg_for_rcq targets ~line 69 at 1080p60, which is only
 * in blanking when the counter is doubled). Default on; left as a live knob in
 * case a specific output path differs - flip it and watch arm_line_max vs
 * de_line_total. Takes effect at the next modetest (mode_set).
 */
static bool tcon_line_double = true;
module_param(tcon_line_double, bool, 0644);
MODULE_PARM_DESC(tcon_line_double,
		 "TCON line counter runs 0..2*vtotal (half-lines)");

/*
 * TEMP read-only diagnostics, exposed on sun8i_mixer.ko under
 * /sys/module/sun8i_mixer/parameters/. de_* report the timing derived in
 * mode_set (so a wrong line-unit/derivation is visible at a glance);
 * arm_line_* report the TRUE TCON scanline at the moment of every arm
 * (immediate AND hrtimer-deferred), so we can tell whether a deferred arm is
 * actually landing in blanking. arm_now/arm_late count the two paths.
 * Reset at each mode_set (i.e. each modetest run).
 */
static int dbg_line_active = -1;
static int dbg_line_total  = -1;
static int dbg_ns_per_line = -1;
static int dbg_arm_target  = -1;
static int dbg_arm_line_last = -1;
static int dbg_arm_line_min  = 0x7fffffff;
static int dbg_arm_line_max  = -1;
static unsigned int dbg_arm_now;	/* immediate (in-IRQ) arms */
static unsigned int dbg_arm_late;	/* hrtimer-deferred arms scheduled */
static unsigned int dbg_commits;	/* commits staged */
static unsigned int dbg_coalesced;	/* stages overwritten before an arm */
static unsigned int dbg_arm_busy;	/* arms skipped: load still in flight */
static unsigned int dbg_arm_gated;	/* arms deferred: beam outside blanking */
static unsigned int dbg_status;		/* last RCQ STATUS read at arm */
module_param_named(de_active_line, dbg_line_active, int, 0444);
module_param_named(de_line_total,  dbg_line_total,  int, 0444);
module_param_named(de_ns_per_line, dbg_ns_per_line, int, 0444);
module_param_named(de_arm_target,  dbg_arm_target,  int, 0444);
module_param_named(arm_line_last, dbg_arm_line_last, int, 0444);
module_param_named(arm_line_min,  dbg_arm_line_min,  int, 0444);
module_param_named(arm_line_max,  dbg_arm_line_max,  int, 0444);
module_param_named(arm_now,   dbg_arm_now,   uint, 0444);
module_param_named(arm_late,  dbg_arm_late,  uint, 0444);
module_param_named(commits,   dbg_commits,   uint, 0444);
module_param_named(coalesced, dbg_coalesced, uint, 0444);
module_param_named(arm_busy,  dbg_arm_busy,  uint, 0444);
module_param_named(arm_gated, dbg_arm_gated, uint, 0444);
module_param_named(rcq_status, dbg_status,   uint, 0444);

/*
 * Busy-gate: skip an arm while STATUS reports BUSY. DEFAULT OFF - measured on
 * hardware, BUSY (bit4) is NOT a transient in-flight flag; it is the DE's
 * active-region flag and is already set by TCON line ~69 (the DE prefetches
 * ahead of TCON active video at line 82). Gating on it blocks every arm,
 * including the initial datapath load -> black screen. Kept only as a probe.
 */
static bool arm_busy_gate;
module_param(arm_busy_gate, bool, 0644);
MODULE_PARM_DESC(arm_busy_gate, "skip RCQ arm while STATUS BUSY (DANGEROUS)");

/*
 * Arm exactly at the target line like the vendor (sleep from the current line
 * up to arm_target before arming), rather than arming immediately whenever the
 * beam is already before the target. The vendor arms late in blanking (~line 69
 * at 1080p60, ~13 lines before active), which likely compensates for the
 * DE->TCON async-bridge pipeline delay: TCON-blanking != DE-blanking. Toggle to
 * A/B early-vs-late arming on hardware. Watch arm_line_min/max move to ~target.
 */
/*
 * DEFAULT OFF: arm early, as soon as the vblank IRQ finds the beam before the
 * target. On hardware the DE is idle in early blanking (TCON line ~5) but
 * already BUSY by the vendor target (~69), so arming early lands in the DE's
 * genuine idle window with the most margin. On = sleep to the target line like
 * the vendor (kept for A/B testing; the vendor's late arm suits its pipeline,
 * not necessarily ours).
 */
static bool arm_at_target;
module_param(arm_at_target, bool, 0644);
MODULE_PARM_DESC(arm_at_target, "sleep to the target line before arming (vendor)");

/*
 * Minimal page flip: once the datapath has been armed once, a subsequent commit
 * reloads ONLY the framebuffer-address blocks (the overlay layer + its high-addr
 * para block), leaving the blender, scaler and formatter loaded from the first
 * arm. A static image works (one arm); only flips flicker, and the sole thing
 * that changes per flip is the fb address - so reloading the blender/route/VSU
 * coefficients every frame (separate RCQ blocks, not necessarily latched
 * atomically together) is the prime suspect. Off = reload all blocks every
 * frame (old behaviour). Assumes constant geometry between flips (true for
 * modetest -v); a geometry/format change still forces a full reload via
 * mode_set.
 */
static bool flip_min = true;
module_param(flip_min, bool, 0644);
MODULE_PARM_DESC(flip_min, "page flips reload only the fb-address blocks");

/*
 * Shadow->register transport: false = vendor AHB mode (CPU/MMIO write
 * in the blanking window; today the ONLY one that applies the BLD/VSU/FMT
 * blocks on the A523), true = RCQ DMA (UNDER DEBUG: reports FINISH but
 * does not apply the blocks targeting BLD 0x281xxx; see C4B).
 */
static bool use_rcq;
module_param(use_rcq, bool, 0644);
MODULE_PARM_DESC(use_rcq, "use the RCQ DMA instead of AHB/MMIO writes");

/*
 * TEMP: phys address of the bound TCON's current-scan register (TCON_tv0 +0xfc,
 * line in bits[11:0]). Mapped once at init; read in do_arm to record the real
 * arm scanline for the deferred path too (the de has no other handle to the
 * TCON). Overridable in case the HDMI TCON base differs.
 */
static unsigned int tcon_curline_phys = 0x55030fc;
module_param(tcon_curline_phys, uint, 0644);
static void __iomem *tcon_curline_io;

static enum hrtimer_restart sun55i_de_arm_timer(struct hrtimer *t);
static u32 sun55i_de_arm_target(struct sun55i_de *de);
static u32 sun55i_de_lines_to_target(struct sun55i_de *de, bool next_frame);
static void sun55i_de_layer_disable(struct sun8i_mixer *mixer);

static struct sun55i_de_block *de_blk(struct sun8i_mixer *mixer, unsigned int i)
{
	return &mixer->de->blocks[i];
}

/* an old framebuffer held until the RCQ arm that retires it (see the .h) */
struct sun55i_de_fb_hold {
	struct list_head	node;
	struct drm_framebuffer	*fb;
};

/*
 * Reference an fb a commit is replacing, so its GEM object (and its IOMMU
 * mapping) outlives the deferred RCQ arm that stops the DE fetching it.
 * BSP semantics: buffers retire on RCQ FINISH, not on the vblank event.
 */
void sun55i_de_hold_fb(struct sun8i_mixer *mixer, struct drm_framebuffer *fb)
{
	struct sun55i_de *de = mixer->de;
	struct sun55i_de_fb_hold *hold;
	unsigned long flags;

	hold = kmalloc(sizeof(*hold), GFP_KERNEL);
	if (!hold) {
		/*
		 * Best effort: behaves like the unpatched driver, whose
		 * freed-fb fetch is precisely the MBUS-wedge bug this hold
		 * prevents - leave a trace so a post-OOM wedge is attributable.
		 */
		pr_warn_once("sun55i_de: fb hold alloc failed; early fb free may fault the DE\n");
		return;
	}

	drm_framebuffer_get(fb);
	hold->fb = fb;

	spin_lock_irqsave(&de->arm_lock, flags);
	if (de->quiesced) {
		/*
		 * DE gated + in reset: nothing can fetch, so the hold is not
		 * needed - and with the TCON down no arm would ever age it
		 * (a DPMS-off client flipping away would pin GEM unbounded).
		 */
		spin_unlock_irqrestore(&de->arm_lock, flags);
		drm_framebuffer_put(fb);
		kfree(hold);
		return;
	}
	list_add_tail(&hold->node, &de->fbs_pending);
	spin_unlock_irqrestore(&de->arm_lock, flags);
}

/* put the retired fbs from work context (a final put can sleep in GEM) */
static void sun55i_de_fb_release_work(struct work_struct *work)
{
	struct sun55i_de *de = container_of(work, struct sun55i_de,
					    fb_release_work);
	struct sun55i_de_fb_hold *hold, *tmp;
	unsigned long flags;
	LIST_HEAD(gone);

	spin_lock_irqsave(&de->arm_lock, flags);
	list_splice_init(&de->fbs_release, &gone);
	spin_unlock_irqrestore(&de->arm_lock, flags);

	list_for_each_entry_safe(hold, tmp, &gone, node) {
		drm_framebuffer_put(hold->fb);
		kfree(hold);
	}
}

/* age the holds one arm; caller holds arm_lock */
static void sun55i_de_age_fb_holds(struct sun55i_de *de)
{
	list_splice_tail_init(&de->fbs_armed2, &de->fbs_release);
	list_splice_tail_init(&de->fbs_armed1, &de->fbs_armed2);
	list_splice_tail_init(&de->fbs_pending, &de->fbs_armed1);
	if (!list_empty(&de->fbs_release))
		schedule_work(&de->fb_release_work);
}

/* assert/deassert RST_BUS_DE (raw CCU MMIO; bit0 bus gate preserved) */
static void sun55i_de_bus_reset(struct sun55i_de *de, bool assert)
{
	u32 val;

	if (!de->bus_reset)
		return;

	val = readl(de->bus_reset);
	if (assert)
		val &= ~SUN55I_CCU_DE_BGR_RST;
	else
		val |= SUN55I_CCU_DE_BGR_RST;
	writel(val, de->bus_reset);
}

/*
 * write a u32 into a block's shadow; only a real CHANGE marks the block
 * stage-dirty (so a pure flip only dirties the address blocks and the
 * rest is not recopied every frame - what flip_min tried to do by hand)
 */
static void de_blk_write(struct sun55i_de_block *blk, u32 off, u32 val)
{
	__le32 *p = (__le32 *)((u8 *)blk->shadow + off);

	if (*p == cpu_to_le32(val))
		return;

	*p = cpu_to_le32(val);
	blk->stage_dirty = true;
}

static void de_blk_set_head(struct sun55i_de_block *blk)
{
	struct sun55i_de_rcq_head *hd = blk->head;

	hd->low_addr = cpu_to_le32(lower_32_bits(blk->phys));
	hd->dw0 = cpu_to_le32((blk->size & 0xffffff) |
			      ((upper_32_bits(blk->phys) & 0xff) << 24));
	hd->dirty = cpu_to_le32(0);
	hd->reg_offset = cpu_to_le32(blk->reg_off);
}

/*
 * Allocate the coherent RCQ pool (head array + block data) and lay out the
 * blocks. The shadow defaults (zero) already mean: all blender pipes disabled,
 * RGB progressive output, no color key.
 */
int sun55i_de_init(struct sun8i_mixer *mixer)
{
	static const struct {
		u32 reg_base, off, size;
	} layout[SUN55I_DE_BLK_NUM] = {
		[SUN55I_DE_BLK_BLD_ATTR] = { SUN55I_DE_BLD_BASE,
			SUN55I_DE_BLD_ATTR_OFF, SUN55I_DE_BLD_ATTR_SIZE },
		[SUN55I_DE_BLK_BLD_CTL]  = { SUN55I_DE_BLD_BASE,
			SUN55I_DE_BLD_CTL_OFF, SUN55I_DE_BLD_CTL_SIZE },
		[SUN55I_DE_BLK_BLD_CK]   = { SUN55I_DE_BLD_BASE,
			SUN55I_DE_BLD_CK_OFF, SUN55I_DE_BLD_CK_SIZE },
		[SUN55I_DE_BLK_FMT]      = { SUN55I_DE_FMT_BASE,
			0, SUN55I_DE_FMT_SIZE },
		[SUN55I_DE_BLK_OVL_LAY0] = { SUN55I_DE_OVL_BASE,
			SUN55I_DE_OVL_LAY_OFF(0), SUN55I_DE_OVL_LAY0_SIZE },
		[SUN55I_DE_BLK_OVL_LAY1] = { SUN55I_DE_OVL_BASE,
			SUN55I_DE_OVL_LAY_OFF(1), SUN55I_DE_OVL_LAY0_SIZE },
		[SUN55I_DE_BLK_OVL_LAY2] = { SUN55I_DE_OVL_BASE,
			SUN55I_DE_OVL_LAY_OFF(2), SUN55I_DE_OVL_LAY0_SIZE },
		[SUN55I_DE_BLK_OVL_LAY3] = { SUN55I_DE_OVL_BASE,
			SUN55I_DE_OVL_LAY_OFF(3), SUN55I_DE_OVL_LAY0_SIZE },
		[SUN55I_DE_BLK_OVL_PARA] = { SUN55I_DE_OVL_BASE,
			SUN55I_DE_OVL_PARA_OFF, SUN55I_DE_OVL_PARA_SIZE },
		[SUN55I_DE_BLK_OVL_DS]   = { SUN55I_DE_OVL_BASE,
			SUN55I_DE_OVL_DS_OFF, SUN55I_DE_OVL_DS_SIZE },
		[SUN55I_DE_BLK_VSU_CTL]  = { SUN55I_DE_VSU_BASE,
			SUN55I_DE_VSU_CTL_OFF, SUN55I_DE_VSU_CTL_SIZE },
		[SUN55I_DE_BLK_VSU_ATTR] = { SUN55I_DE_VSU_BASE,
			SUN55I_DE_VSU_ATTR_OFF, SUN55I_DE_VSU_ATTR_SIZE },
		[SUN55I_DE_BLK_VSU_YPARA] = { SUN55I_DE_VSU_BASE,
			SUN55I_DE_VSU_YPARA_OFF, SUN55I_DE_VSU_PARA_SIZE },
		[SUN55I_DE_BLK_VSU_CPARA] = { SUN55I_DE_VSU_BASE,
			SUN55I_DE_VSU_CPARA_OFF, SUN55I_DE_VSU_PARA_SIZE },
		[SUN55I_DE_BLK_VSU_COEFF0] = { SUN55I_DE_VSU_BASE,
			SUN55I_DE_VSU_COEFF0_OFF, SUN55I_DE_VSU_COEFF_SIZE },
		[SUN55I_DE_BLK_VSU_COEFF1] = { SUN55I_DE_VSU_BASE,
			SUN55I_DE_VSU_COEFF1_OFF, SUN55I_DE_VSU_COEFF_SIZE },
		[SUN55I_DE_BLK_VSU_COEFF2] = { SUN55I_DE_VSU_BASE,
			SUN55I_DE_VSU_COEFF2_OFF, SUN55I_DE_VSU_COEFF_SIZE },
		[SUN55I_DE_BLK_TFBD_CTL] = { SUN55I_DE_TFBD_BASE, 0, 4 },
		[SUN55I_DE_BLK_AFBD]     = { SUN55I_DE_AFBD_BASE, 0, 0x58 },
		[SUN55I_DE_BLK_CCSC_CTL] = { SUN55I_DE_CCSC_BASE, 0, 4 },
		[SUN55I_DE_BLK_CDC_CTL]  = { SUN55I_DE_CDC_BASE, 0, 4 },
	};
	struct sun55i_de *de;
	size_t heads_sz, off;
	unsigned int i;
	int ret;

	de = devm_kzalloc(mixer->dev, sizeof(*de), GFP_KERNEL);
	if (!de)
		return -ENOMEM;
	mixer->de = de;
	de->mixer = mixer;

	spin_lock_init(&de->arm_lock);
	hrtimer_setup(&de->arm_timer, sun55i_de_arm_timer, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL);
	INIT_LIST_HEAD(&de->fbs_pending);
	INIT_LIST_HEAD(&de->fbs_armed1);
	INIT_LIST_HEAD(&de->fbs_armed2);
	INIT_LIST_HEAD(&de->fbs_release);
	INIT_WORK(&de->fb_release_work, sun55i_de_fb_release_work);
	de->quiesced = true;	/* off until the first enable() */

	/* TEMP: map the TCON current-scan reg for arm-line diagnostics */
	tcon_curline_io = devm_ioremap(mixer->dev, tcon_curline_phys & ~0x3, 4);

	/* HDMI is wired to tcon_tv0 = vendor tcon2 */
	de->tcon_id = 2;

	/* full DE map for AHB mode (reg of the BSP de@5000000) */
	de->mmio = devm_ioremap(mixer->dev, 0x5000000, 0x400000);
	if (!de->mmio)
		dev_warn(mixer->dev,
			 "sun55i_de: no DE MMIO map; forcing RCQ mode\n");

	/* RST_BUS_DE from the CCU for the wedge fix (see sun55i_de.h) */
	de->bus_reset = devm_ioremap(mixer->dev, SUN55I_CCU_DE_BGR_PHYS, 4);
	if (!de->bus_reset)
		dev_warn(mixer->dev,
			 "sun55i_de: no DE_BGR map; no anti-wedge reset\n");

	de->nheads = ALIGN(SUN55I_DE_BLK_NUM, 2);
	heads_sz = ALIGN(de->nheads * sizeof(struct sun55i_de_rcq_head),
			 SUN55I_RCQ_ALIGN);

	off = heads_sz;
	for (i = 0; i < SUN55I_DE_BLK_NUM; i++)
		off += ALIGN(layout[i].size, SUN55I_RCQ_ALIGN);
	de->pool_size = off;

	/*
	 * The RCQ fetch does not go through the IOMMU and only reaches low
	 * DRAM (~<0x4b000000 on the A523). The pool is small and without this
	 * dma_alloc_coherent takes it from the buddy (high pages, e.g.
	 * 0x52100000 observed): the DE reads garbage and no datapath block
	 * lands (only the green blender background shows). The mixer node
	 * memory-region points at the low CMA.
	 */
	ret = of_reserved_mem_device_init(mixer->dev);
	if (ret && ret != -ENODEV)
		dev_warn(mixer->dev,
			 "no memory-region for the RCQ pool (%d)\n", ret);

	de->pool = dmam_alloc_coherent(mixer->dev, de->pool_size,
				       &de->pool_dma, GFP_KERNEL);
	if (!de->pool) {
		of_reserved_mem_device_release(mixer->dev);
		mixer->de = NULL;
		return -ENOMEM;
	}

	de->heads = de->pool;
	de->heads_dma = de->pool_dma;

	off = heads_sz;
	for (i = 0; i < SUN55I_DE_BLK_NUM; i++) {
		struct sun55i_de_block *blk = &de->blocks[i];

		blk->reg_off = layout[i].reg_base + layout[i].off;
		blk->size = layout[i].size;
		blk->shadow = (u8 *)de->pool + off;
		blk->phys = de->pool_dma + off;
		blk->head = &de->heads[i];
		de_blk_set_head(blk);
		off += ALIGN(layout[i].size, SUN55I_RCQ_ALIGN);
	}

	/*
	 * VSU global alpha to opaque FROM THE START: the channel alpha
	 * stage applies EVEN with the scaler in bypass (ctl=0), so a zero
	 * shadow = a 100% transparent layer = black. U-Boot leaves 0xFF
	 * (its logo goes through this channel) and the milestone pipeline
	 * "worked" by accident because the gated RCQ never applied our 0
	 * shadow; AHB mode did. (BSP: de_scaler.c glb_alpha = plane
	 * alpha, 0xFF by default.)
	 */
	de_blk_write(de_blk(mixer, SUN55I_DE_BLK_VSU_ATTR),
		     SUN55I_VSU_ATTR_GLB_ALPHA, 0xff);

	/* static output-formatter setup: RGB, 8-bit, full range */
	de_blk_write(de_blk(mixer, SUN55I_DE_BLK_FMT),
		     SUN55I_FMT_LIMIT_Y, SUN55I_FMT_LIMIT_FULL);
	de_blk_write(de_blk(mixer, SUN55I_DE_BLK_FMT),
		     SUN55I_FMT_LIMIT_C0, SUN55I_FMT_LIMIT_FULL);
	de_blk_write(de_blk(mixer, SUN55I_DE_BLK_FMT),
		     SUN55I_FMT_LIMIT_C1, SUN55I_FMT_LIMIT_FULL);

	/* program the RCQ head array pointer (de_top_set_rcq_head) */
	regmap_write(mixer->top_regs, SUN55I_MIXER_RCQ_HEAD_LADDR,
		     lower_32_bits(de->heads_dma));
	regmap_write(mixer->top_regs, SUN55I_MIXER_RCQ_HEAD_HADDR,
		     upper_32_bits(de->heads_dma));
	regmap_write(mixer->top_regs, SUN55I_MIXER_RCQ_HEAD_LEN,
		     de->nheads * sizeof(struct sun55i_de_rcq_head));

	dev_dbg(mixer->dev,
		 "sun55i_de: RCQ pool %zu bytes @%pad, %u heads, blocks bld{%06x,%06x,%06x} fmt{%06x} ovl{%06x,%06x}\n",
		 de->pool_size, &de->pool_dma, de->nheads,
		 de->blocks[SUN55I_DE_BLK_BLD_ATTR].reg_off,
		 de->blocks[SUN55I_DE_BLK_BLD_CTL].reg_off,
		 de->blocks[SUN55I_DE_BLK_BLD_CK].reg_off,
		 de->blocks[SUN55I_DE_BLK_FMT].reg_off,
		 de->blocks[SUN55I_DE_BLK_OVL_LAY0].reg_off,
		 de->blocks[SUN55I_DE_BLK_OVL_PARA].reg_off);

	return 0;
}

/*
 * Component-unbind teardown. The devm allocations (de, the dmam pool, the
 * ioremaps) only die at platform-driver detach, AFTER this - but the hrtimer
 * and fb_release_work can still be queued/running here (the atomic shutdown's
 * quiesce() schedules the work moments earlier), and nothing else stops them
 * before devm frees the memory they touch. Drain them, then drop any holds
 * the last quiesce left in flight, and detach the reserved-mem region so a
 * re-bind (component defer, sysfs unbind/bind) does not attach it twice.
 */
void sun55i_de_fini(struct sun8i_mixer *mixer)
{
	struct sun55i_de *de = mixer->de;
	struct sun55i_de_fb_hold *hold, *tmp;
	unsigned long flags;
	LIST_HEAD(gone);

	if (!de)
		return;

	spin_lock_irqsave(&de->arm_lock, flags);
	de->arm_pending = false;
	de->quiesced = true;
	spin_unlock_irqrestore(&de->arm_lock, flags);

	hrtimer_cancel(&de->arm_timer);
	cancel_work_sync(&de->fb_release_work);

	spin_lock_irqsave(&de->arm_lock, flags);
	list_splice_tail_init(&de->fbs_pending, &gone);
	list_splice_tail_init(&de->fbs_armed1, &gone);
	list_splice_tail_init(&de->fbs_armed2, &gone);
	list_splice_tail_init(&de->fbs_release, &gone);
	spin_unlock_irqrestore(&de->arm_lock, flags);

	list_for_each_entry_safe(hold, tmp, &gone, node) {
		drm_framebuffer_put(hold->fb);
		kfree(hold);
	}

	of_reserved_mem_device_release(mixer->dev);

	/* the global diag mapping dies with this device's devm */
	tcon_curline_io = NULL;
	mixer->de = NULL;
}

/*
 * Program the per-mode state: arm timing + datapath sizes (RCQ shadow only).
 * The de_top/GLB MMIO programming lives in sun55i_de_enable(): the atomic
 * helper only calls mode_set when the MODE changed, while an active-only
 * transition (DPMS, session switch) still goes through quiesce()+enable() -
 * any register the quiesce/reset tears down must be rebuilt in enable(),
 * which runs on every light-up.
 */
void sun55i_de_mode_set(struct sun8i_mixer *mixer,
			const struct drm_display_mode *mode)
{
	struct sun55i_de *de = mixer->de;
	u32 w = mode->hdisplay, h = mode->vdisplay;
	u32 size = SUN55I_DE_OUT_SIZE(w, h);
	u32 vtotal = mode->crtc_vtotal, clock = mode->crtc_clock;
	unsigned long flags;
	u32 vbp;

	/*
	 * Beam-gated arm timing (see sun55i_de_vblank_quirk). The TV-TCON line
	 * counter (cur_line, TCON+0xfc) is zeroed at the start of vertical sync
	 * and counts up to the programmed vertical total, which is doubled for
	 * progressive modes (ver_total*2) -> 0..2*vtotal. The RCQ must be armed
	 * during the leading blanking. We replicate the vendor target exactly
	 * (disp_mgr_protect_reg_for_rcq): arm_target = ver_back_porch + 3% of
	 * ver_total, where the Allwinner ver_back_porch = vtotal - vsync_end.
	 * ns_per_line = htotal / pixel_clock, per counter tick.
	 */
	hrtimer_cancel(&de->arm_timer);

	/* timing + shadow are consumed from IRQ context: stage under arm_lock */
	spin_lock_irqsave(&de->arm_lock, flags);
	vbp = mode->crtc_vtotal - mode->crtc_vsync_end;
	de->line_total = vtotal ? vtotal : 1;
	de->arm_target = vbp + (vtotal * 3) / 100;
	de->ns_per_line = clock ?
		div_u64((u64)mode->crtc_htotal * NSEC_PER_MSEC, clock) : 0;

	/*
	 * If the counter is in half-lines the modulus and tick period scale by
	 * two. arm_target follows the vendor, which uses the raw (un-doubled)
	 * back-porch line number as the counter target, so it is NOT scaled.
	 */
	if (tcon_line_double) {
		de->line_total  *= 2;
		de->ns_per_line /= 2;
	}

	/*
	 * Start of active video in counter units: the end of the safe arm
	 * window. do_arm() beam-gates every arm against it (fix wedge
	 * 2026-07-18).
	 */
	de->line_active = (tcon_line_double ? 2 : 1) *
			  (mode->crtc_vtotal - mode->crtc_vsync_start);

	/*
	 * TEMP: publish the derived timing + reset the arm-line stats. The
	 * active-video start (leading blanking end) lets the arm target and the
	 * observed arm line be sanity-checked against it: a tear-free arm needs
	 * arm_line_max < de_active_line.
	 */
	dbg_line_active = de->line_active;
	dbg_line_total  = de->line_total;
	dbg_ns_per_line = de->ns_per_line;
	dbg_arm_target  = sun55i_de_arm_target(de);
	dbg_arm_line_last = -1;
	dbg_arm_line_min  = 0x7fffffff;
	dbg_arm_line_max  = -1;
	dbg_arm_now = dbg_arm_late = dbg_commits = dbg_coalesced = 0;
	dbg_arm_busy = 0;
	dbg_arm_gated = 0;

	/* --- datapath: staged into the RCQ shadow --- */

	/* blender: all pipes off -> output is the background color */
	de_blk_write(de_blk(mixer, SUN55I_DE_BLK_BLD_ATTR),
		     SUN55I_BLD_ATTR_PIPE_EN, 0);
	de_blk_write(de_blk(mixer, SUN55I_DE_BLK_BLD_CTL),
		     SUN55I_BLD_CTL_OUT_SIZE, size);
	de_blk_write(de_blk(mixer, SUN55I_DE_BLK_BLD_CTL),
		     SUN55I_BLD_CTL_BG_COLOR, mixer->de_bg_color);
	/* out_ctl: fmt_space=RGB(0), premul=0, interlace=0 */
	de_blk_write(de_blk(mixer, SUN55I_DE_BLK_BLD_CK),
		     SUN55I_BLD_CK_OUT_CTL, 0);

	/* output formatter size */
	de_blk_write(de_blk(mixer, SUN55I_DE_BLK_FMT),
		     SUN55I_FMT_SIZE_REG, size);
	spin_unlock_irqrestore(&de->arm_lock, flags);

	/* TEMP diagnostic (dev_info) — revert later */
	dev_info(mixer->dev,
		 "sun55i_de: mode_set %ux%u size=%08x bg=%08x\n",
		 w, h, size, mixer->de_bg_color);
}

/*
 * (Re)initialize the DE for scanout, with the TCON still stopped. Runs on
 * EVERY CRTC enable (sun4i_crtc_atomic_enable), including the active-only
 * transitions that skip mode_set. Mirrors the vendor sunxi_de_enable():
 * deassert rst_bus_de + de_top re-init (rtmx_start).
 *
 * The full-DE reset pulse is the actual wedge fix (C4C/fix-wedge-v2): a
 * fault on the DE's MBUS port during a teardown (fetch of a freed buffer /
 * unmapped IOVA while a session dies) leaves an unacknowledged transaction
 * that permanently hangs the port - registers read back fine but nothing is
 * fetched (screen stuck at the background color), and further traffic can
 * drag down the whole interconnect. Only RST_BUS_DE clears it (live-proven
 * by the C4C de-bringup pulse; the DETOP core reset that quiesce() asserts
 * does NOT). Safe here by construction: the TCON is off, so nothing is
 * scanning, and everything the reset clears is reprogrammed right below
 * (de_top/GLB by MMIO) or re-staged by the post-enable full RCQ reload
 * (full_arms_left: blender/VSU/OVL/AFBD/FMT shadows survive in RAM).
 */
void sun55i_de_enable(struct sun8i_mixer *mixer,
		      const struct drm_display_mode *mode)
{
	struct sun55i_de *de = mixer->de;
	u32 size = SUN55I_DE_OUT_SIZE(mode->hdisplay, mode->vdisplay);
	unsigned long flags;
	u32 mux;

	hrtimer_cancel(&de->arm_timer);

	/*
	 * Under arm_lock: these are read by do_arm()/vblank_quirk() from IRQ
	 * context. The TCON is off at this point so no vblank should fire, but
	 * that is an ordering invariant of the atomic helper, not of this
	 * driver - the lock makes enable() safe on its own.
	 */
	spin_lock_irqsave(&de->arm_lock, flags);
	de->armed_once = false;	/* re-stage the whole datapath */
	de->full_arms_left = 4;	/* re-issue the full reload on the next vblanks */
	de->quiesced = false;	/* commits may hold fbs and arm again */
	de->rcq_kicked = false;	/* the reset pulse below clears the RCQ state */
	de->rcq_skips = 0;
	de->arm_gate_skips = 0;
	de->stall_commits = 0;
	spin_unlock_irqrestore(&de->arm_lock, flags);

	/*
	 * Reset pulse. quiesce() left the reset asserted (BSP model: the DE
	 * is held in reset the whole time the display is off); assert again
	 * anyway so a first-enable/boot path gets the same clean slate.
	 */
	sun55i_de_bus_reset(de, true);
	udelay(20);
	sun55i_de_bus_reset(de, false);
	udelay(10);

	/* --- de_top control registers: plain MMIO, latch immediately --- */

	/*
	 * Ungate the disp0 core (clock + reset release, vendor
	 * __de_mod_clk_enable order). The quiesce() of the previous
	 * disable gated it so the channel could not fetch a freed
	 * framebuffer while the display was down.
	 */
	regmap_read(mixer->detop_regs, SUN55I_DETOP_CLK, &mux);
	regmap_write(mixer->detop_regs, SUN55I_DETOP_CLK,
		     mux | BIT(0) | SUN55I_DETOP_CLK_KEY);
	regmap_read(mixer->detop_regs, SUN55I_DETOP_RESET, &mux);
	regmap_write(mixer->detop_regs, SUN55I_DETOP_RESET,
		     mux | BIT(0) | SUN55I_DETOP_CLK_KEY);

	/*
	 * Enable the DE master-port (MBUS/DRAM) clock. This internal gate is
	 * not a CCU clock; the BSP sets it in de_top_set_clk_enable. Without it
	 * the blender can still scan out a register-sourced background, but any
	 * layer's framebuffer DMA fetch underruns -> sheared/torn/miscolored
	 * output. RMW so we only touch bit0.
	 */
	regmap_read(mixer->detop_regs, SUN55I_DETOP_MBUS_CLK, &mux);
	regmap_write(mixer->detop_regs, SUN55I_DETOP_MBUS_CLK,
		     mux | SUN55I_DETOP_MBUS_CLK_EN);

	/*
	 * Re-program the RCQ head array pointer: cleared by the reset pulse,
	 * and otherwise only written at init.
	 */
	regmap_write(mixer->top_regs, SUN55I_MIXER_RCQ_HEAD_LADDR,
		     lower_32_bits(de->heads_dma));
	regmap_write(mixer->top_regs, SUN55I_MIXER_RCQ_HEAD_HADDR,
		     upper_32_bits(de->heads_dma));
	regmap_write(mixer->top_regs, SUN55I_MIXER_RCQ_HEAD_LEN,
		     de->nheads * sizeof(struct sun55i_de_rcq_head));

	/* bind disp0 -> tcon2 (HDMI); RMW just our nibble */
	regmap_read(mixer->detop_regs, SUN55I_DETOP_DE2TCON_MUX, &mux);
	mux &= ~0xf;
	mux |= de->tcon_id & 0xf;
	regmap_write(mixer->detop_regs, SUN55I_DETOP_DE2TCON_MUX, mux);

	/* de_top_set_rtmx_enable extras (sun55iw3 / soc_ver >= 2) */
	regmap_write(mixer->detop_regs, SUN55I_DETOP_ASYNC_BRIDGE, 0x0);
	regmap_write(mixer->detop_regs, SUN55I_DETOP_BUF_DEPTH, 0x6000);

	/*
	 * Channel mux for the first UI plane (de_rtmx_set_chn_mux, sun55iw3):
	 *   uch2core (0x24): route phys chn 6 -> core/disp 0.  width 2,
	 *     shift ((phy-6)<<1)+16 = 16, value hw_disp(0) -> clear bits[17:16].
	 *   port2uchn (0x28): blender port 1 -> phys chn 6.  width 4,
	 *     shift port<<2 = 4, value phy+2 = 8 -> nibble[1] = 8 (== 0xa980).
	 * RMW so we only own phys6's routing; the reset defaults do for the rest
	 * (same values the C4C boot-time pulse + bind flow ran on for days).
	 */
	regmap_read(mixer->detop_regs, SUN55I_DETOP_UCH2CORE_MUX, &mux);
	mux &= ~(0x3 << (((SUN55I_DE_OVL_UI_PHYS_CHN - 6) << 1) + 16));
	regmap_write(mixer->detop_regs, SUN55I_DETOP_UCH2CORE_MUX, mux);

	regmap_read(mixer->detop_regs, SUN55I_DETOP_PORT2CHN_MUX, &mux);
	mux &= ~(0xf << (SUN55I_DE_OVL_UI_LOGIC_CHN << 2));
	mux |= (SUN55I_DE_OVL_UI_PHYS_CHN + 2) << (SUN55I_DE_OVL_UI_LOGIC_CHN << 2);
	regmap_write(mixer->detop_regs, SUN55I_DETOP_PORT2CHN_MUX, mux);

	regmap_write(mixer->top_regs, SUN50I_MIXER_GLOBAL_SIZE, size);
	regmap_write(mixer->top_regs, SUN50I_MIXER_GLOBAL_CLK, 1);	/* AUTO_CLK */
	regmap_write(mixer->top_regs, SUN8I_MIXER_GLOBAL_CTL,
		     SUN8I_MIXER_GLOBAL_CTL_RT_EN);

	/* TEMP diagnostic (dev_info) — revert later */
	dev_info(mixer->dev,
		 "sun55i_de: enable %ux%u size=%08x port2chn_mux=%08x\n",
		 mode->hdisplay, mode->vdisplay, size, mux);
}

/*
 * DRM format -> v35x overlay "fmt" field. For UI (RGB) channels the vendor
 * uses the raw disp_pixel_format enum as the fmt value (de_ovl.c:202-205,
 * default case: fmt = format; sunxi_display2.h enum). The word order matches
 * DRM's little-endian fourcc naming (MSB..LSB == [31:24]..[7:0]).
 */
static int sun55i_de_fmt(u32 drm_fmt, u32 *fmt)
{
	switch (drm_fmt) {
	case DRM_FORMAT_ARGB8888: *fmt = 0x00; break;
	case DRM_FORMAT_ABGR8888: *fmt = 0x01; break;
	case DRM_FORMAT_RGBA8888: *fmt = 0x02; break;
	case DRM_FORMAT_BGRA8888: *fmt = 0x03; break;
	case DRM_FORMAT_XRGB8888: *fmt = 0x04; break;
	case DRM_FORMAT_XBGR8888: *fmt = 0x05; break;
	case DRM_FORMAT_RGBX8888: *fmt = 0x06; break;
	case DRM_FORMAT_BGRX8888: *fmt = 0x07; break;
	case DRM_FORMAT_RGB888:   *fmt = 0x08; break;
	case DRM_FORMAT_BGR888:   *fmt = 0x09; break;
	case DRM_FORMAT_RGB565:   *fmt = 0x0a; break;
	case DRM_FORMAT_BGR565:   *fmt = 0x0b; break;
	default:
		return -EINVAL;
	}
	return 0;
}

/* src/dst ratio in SUN55I_VSU_STEP_FRAC_BITS fixed point (1.0 == 1 << 19) */
static u32 sun55i_de_vsu_step(u32 src, u32 dst)
{
	if (!dst)
		return 1U << SUN55I_VSU_STEP_FRAC_BITS;
	return (u32)div_u64((u64)src << SUN55I_VSU_STEP_FRAC_BITS, dst);
}

/*
 * Stage the VSU8 scaler to resample the @src_w x @src_h overlay output up/down
 * to the on-screen @dst_w x @dst_h the blender expects (de_vsu8_set_para +
 * de_vsu_calc_lay_scale_para for an RGB layer).
 *
 * A 1:1 layer must BYPASS the VSU (ctl.en=0), not run a unity step through it:
 * this channel's (UCH0) scaler line buffer is only 2560 px wide (de352_feat
 * scale_line_buffer_rgb), so an enabled VSU corrupts every line of a 3840-wide
 * layer beyond px 2560 (the "static right third" at 4K; live-verified
 * VSU_CTL=1 -> broken, VSU_CTL=0 -> perfect). Bypassed, the channel passes
 * pixels through untouched and the line buffer is not used.
 *
 * For RGB the chroma parameters equal the luma ones. Phase is left at 0 (whole-
 * pixel crop origin); the hardware accumulates per-output sub-pixel phase from
 * the step. Actual scaling of layers wider than the line buffer needs the
 * overlay coarse down-sampler / a different channel; not handled here.
 */
static void sun55i_de_vsu_setup(struct sun8i_mixer *mixer,
				u32 src_w, u32 src_h, u32 dst_w, u32 dst_h)
{
	u32 in_size = SUN55I_DE_OUT_SIZE(src_w, src_h);
	u32 out_size = SUN55I_DE_OUT_SIZE(dst_w, dst_h);
	u32 hstep = sun55i_de_vsu_step(src_w, dst_w);
	u32 vstep = sun55i_de_vsu_step(src_h, dst_h);
	u32 hidx = sun55i_vsu8_coef_index(hstep);
	u32 vidx = sun55i_vsu8_coef_index(vstep);
	struct sun55i_de_block *ctl, *attr, *yp, *cp;
	unsigned int i;

	ctl  = de_blk(mixer, SUN55I_DE_BLK_VSU_CTL);
	attr = de_blk(mixer, SUN55I_DE_BLK_VSU_ATTR);
	yp   = de_blk(mixer, SUN55I_DE_BLK_VSU_YPARA);
	cp   = de_blk(mixer, SUN55I_DE_BLK_VSU_CPARA);

	if (src_w == dst_w && src_h == dst_h) {
		/* 1:1: bypass the scaler entirely (see comment above) */
		de_blk_write(ctl, 0, 0);
		return;
	}

	/* ctl.en=1; scale_mode stays 0 (RGB) from the zeroed shadow */
	de_blk_write(ctl, 0, SUN55I_VSU_CTL_EN);

	de_blk_write(attr, SUN55I_VSU_ATTR_OUT_SIZE, out_size);
	de_blk_write(attr, SUN55I_VSU_ATTR_GLB_ALPHA, 0xff);

	/* luma + chroma (RGB: identical): in_size = src, step = ratio, phase 0 */
	de_blk_write(yp, SUN55I_VSU_PARA_IN_SIZE, in_size);
	de_blk_write(yp, SUN55I_VSU_PARA_HSTEP,
		     hstep << SUN55I_VSU_STEP_VALID_START_BIT);
	de_blk_write(yp, SUN55I_VSU_PARA_VSTEP,
		     vstep << SUN55I_VSU_STEP_VALID_START_BIT);
	de_blk_write(cp, SUN55I_VSU_PARA_IN_SIZE, in_size);
	de_blk_write(cp, SUN55I_VSU_PARA_HSTEP,
		     hstep << SUN55I_VSU_STEP_VALID_START_BIT);
	de_blk_write(cp, SUN55I_VSU_PARA_VSTEP,
		     vstep << SUN55I_VSU_STEP_VALID_START_BIT);

	/* per-axis polyphase coefficient sets for the actual ratio */
	for (i = 0; i < SUN55I_VSU_PHASE_NUM; i++) {
		u32 off = i * sizeof(u32);

		de_blk_write(de_blk(mixer, SUN55I_DE_BLK_VSU_COEFF0), off,
			     sun55i_vsu8_lan2_coef[hidx + i]);	/* y_hori */
		de_blk_write(de_blk(mixer, SUN55I_DE_BLK_VSU_COEFF1), off,
			     sun55i_vsu8_lan2_coef[vidx + i]);	/* y_vert */
		de_blk_write(de_blk(mixer, SUN55I_DE_BLK_VSU_COEFF2), off,
			     sun55i_vsu8_lan2_coef[hidx + i]);	/* c_hori */
	}
}

/* turn the blender pipe 0 off (output falls back to the background color) */
/*
 * AFBC modifier the v350 AFBD decodes for RGB 8888. The BSP gate
 * (de_afbc_format_mod_supported) ONLY accepts 32x8|SPARSE|YTR|SPLIT for
 * RGB (SUNXI_RGB_AFBC_MOD): in fill_afbc_info that forces wide_block=1 and
 * thus block_layout = superblock_layout[1] = 3 (32x8) ALWAYS. The 16x16
 * branch (superblock_layout[0]=0) for RGB is dead code the silicon never
 * runs -> we used to advertise 16x16 and the decode came out garbage. Now
 * we advertise exactly the mode the vendor uses daily, which is also
 * the one the AN1 libmali blob emits and the RGB modifier Mesa/panfrost
 * prefers (pan_best_modifiers). RGBA8888 (32b >=17) requires SPLIT.
 */
static const u64 sun55i_de_afbc_modifiers[] = {
	DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_32x8 |
				AFBC_FORMAT_MOD_SPARSE | AFBC_FORMAT_MOD_YTR |
				AFBC_FORMAT_MOD_SPLIT),
};

/* BSP fmt_seq (de_afbd_apply_lay) per format */
static int sun55i_de_afbc_fmt_seq(u32 drm_fmt, u32 *seq)
{
	switch (drm_fmt) {
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_XRGB8888:
		*seq = 0x1230;
		return 0;
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_XBGR8888:
		*seq = 0x3210;
		return 0;
	case DRM_FORMAT_RGBA8888:
	case DRM_FORMAT_RGBX8888:
		*seq = 0x0123;
		return 0;
	case DRM_FORMAT_BGRA8888:
	case DRM_FORMAT_BGRX8888:
		*seq = 0x2103;
		return 0;
	}
	return -EINVAL;
}

/*
 * AFBC scanout via the AFBD: the DE decodes compressed RGB8888 in the
 * vendor mode 32x8|SPARSE|YTR|SPLIT (block_layout=3). VISUALLY CONFIRMED
 * 2026-07-12: perfect Plasma desktop with kwin scanning AR24 in that
 * modifier (before, in 16x16, it came out black/squares: 16x16 RGB is a
 * mode this AFBD does not decode). On by default; afbc=0 disables it
 * (forces linear scanout) if needed.
 */
bool sun55i_afbc_enable = true;
module_param_named(afbc, sun55i_afbc_enable, bool, 0444);
MODULE_PARM_DESC(afbc, "advertise AFBC modifiers (AFBD-compressed scanout)");

bool sun55i_de_afbc_mod_supported(u32 drm_fmt, u64 modifier)
{
	u32 seq;
	unsigned int i;

	if (!sun55i_afbc_enable)
		return false;

	if (sun55i_de_afbc_fmt_seq(drm_fmt, &seq))
		return false;
	for (i = 0; i < ARRAY_SIZE(sun55i_de_afbc_modifiers); i++)
		if (modifier == sun55i_de_afbc_modifiers[i])
			return true;
	return false;
}

/*
 * Program the AFBD block for an AFBC RGB8888 16x16 fb (port of
 * de_afbd_apply_lay + de_fbd_get_info from the BSP). The AFBD replaces
 * the OVL: it produces the src window (0,0 src_w x src_h) into the VSU,
 * as the overlay would in the linear path.
 */
static void sun55i_de_afbd_stage(struct sun8i_mixer *mixer,
				 struct drm_framebuffer *fb,
				 u32 src_w, u32 src_h)
{
	struct sun55i_de_block *afbd = de_blk(mixer, SUN55I_DE_BLK_AFBD);
	struct drm_gem_dma_object *gem = drm_fb_dma_get_gem_obj(fb, 0);
	u64 addr;
	bool ytr = fb->modifier &
		   DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_YTR);
	/*
	 * BSP wide_block (fill_afbc_info): the mode the vendor uses for
	 * RGB is 32x8 (block_layout = superblock_layout[1] = 3, blocks /32
	 * x /8). Keep the 16x16 path (layout 0) in case it is ever
	 * advertised, but RGB only decodes in 32x8 on this silicon.
	 */
	bool wide = (fb->modifier &
		     DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_32x8)) ==
		    DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_32x8);
	u32 blk_layout = wide ? 3 : 0;
	u32 blk_w = wide ? DIV_ROUND_UP(fb->width, 32)
			 : DIV_ROUND_UP(fb->width, 16);
	u32 blk_h = wide ? DIV_ROUND_UP(fb->height, 8)
			 : DIV_ROUND_UP(fb->height, 16);
	u32 seq, fmt;

	/*
	 * Both unreachable today (layer_update already validated fmt+modifier
	 * via afbc_mod_supported, and a DRM fb always has a GEM on plane 0),
	 * but a badly-added new format would program the AFBD with stack
	 * garbage: degrade to a disabled layer, never a corrupt fetch.
	 */
	if (!gem || sun55i_de_afbc_fmt_seq(fb->format->format, &seq)) {
		DRM_DEBUG_DRIVER("sun55i_de: afbd_stage without gem/seq, layer off\n");
		sun55i_de_layer_disable(mixer);
		return;
	}
	addr = gem->dma_addr + fb->offsets[0];

	/* enable + global alpha 0xff, no rotation (uch0 has none) */
	de_blk_write(afbd, SUN55I_AFBD_CTL, SUN55I_AFBD_CTL_EN |
		     SUN55I_AFBD_CTL_ALPHA_MODE(1) | SUN55I_AFBD_CTL_ALPHA(0xff));
	de_blk_write(afbd, SUN55I_AFBD_FMT_SEQ, seq);
	de_blk_write(afbd, SUN55I_AFBD_IMG_SIZE,
		     ((src_w - 1) & 0xfff) | (((src_h - 1) & 0xfff) << 16));
	/* stream blocks: over the TOTAL fb size, not the crop */
	de_blk_write(afbd, SUN55I_AFBD_BLK_SIZE,
		     (blk_w & 0x3ff) | ((blk_h & 0x3ff) << 16));
	de_blk_write(afbd, SUN55I_AFBD_SRC_CROP, 0);
	de_blk_write(afbd, SUN55I_AFBD_LAY_CROP, 0);
	/* fmt | yuv_tran<<7 | sb_layout<<8 | sbs {1,1} (BSP RGB8888) */
	fmt = SUN55I_AFBD_FMT_RGBA8888 | ((ytr ? 1 : 0) << 7) |
	      ((blk_layout & 0x3) << 8) | (1 << 16) | (1 << 18);
	de_blk_write(afbd, SUN55I_AFBD_FMT, fmt);
	de_blk_write(afbd, SUN55I_AFBD_HD_LADDR, lower_32_bits(addr));
	de_blk_write(afbd, SUN55I_AFBD_HD_HADDR, upper_32_bits(addr) & 0xff);
	de_blk_write(afbd, SUN55I_AFBD_OVL_SIZE,
		     ((src_w - 1) & 0x1fff) | (((src_h - 1) & 0x1fff) << 16));
	de_blk_write(afbd, SUN55I_AFBD_OVL_COOR, 0);
	de_blk_write(afbd, SUN55I_AFBD_BG_COLOR, 0xff000000);
	/* compbits RGBA8888 {8,9,9,8}: color0=c0|c3<<16, color1=c2|c1<<16 */
	de_blk_write(afbd, SUN55I_AFBD_COLOR0, 0x00ff00ff);
	de_blk_write(afbd, SUN55I_AFBD_COLOR1, 0x01ff01ff);
}

static void sun55i_de_afbd_disable(struct sun8i_mixer *mixer)
{
	de_blk_write(de_blk(mixer, SUN55I_DE_BLK_AFBD), SUN55I_AFBD_CTL, 0);
}

static void sun55i_de_layer_disable(struct sun8i_mixer *mixer)
{
	sun55i_de_afbd_disable(mixer);
	de_blk_write(de_blk(mixer, SUN55I_DE_BLK_OVL_LAY0),
		     SUN55I_OVL_LAY_ATTCTL, 0);
	de_blk_write(de_blk(mixer, SUN55I_DE_BLK_BLD_ATTR),
		     SUN55I_BLD_ATTR_PIPE_EN, 0);
}

/*
 * Stage one RGB UI layer (phys channel 6, pipe 0) into the RCQ shadow from a
 * DRM plane state: the de_ovl layer registers (de_ovl_u_apply_lay) plus the
 * blender pipe enable/route/attr that makes the channel visible
 * (de_bld_set_pipe_ctl / de_bld_set_pipe_attr / de_bld_set_blend_mode).
 *
 * Only the primary UI channel is wired this milestone; other planes are left
 * disabled. No scaling: the overlay crop == ovl-out == on-screen size.
 */
void sun55i_de_layer_update(struct sun8i_mixer *mixer, struct sun8i_layer *layer,
			    struct drm_plane_state *state)
{
	struct drm_framebuffer *fb = state->fb;
	struct sun55i_de_block *lay, *para, *attr, *ctl;
	u32 src_w, src_h, dst_x, dst_y, dst_w, dst_h;
	dma_addr_t addr;
	u32 fmt, attctl;

	lockdep_assert_held(&mixer->de->arm_lock);

	/*
	 * this milestone only drives the first UI channel (logic 1 = phys 6).
	 * In 6.18 layer->channel is the LOGICAL channel (vi_num + index); the
	 * comparison against the physical (7.1 semantics) made ALL planes
	 * return here, leaving the RCQ pool without OVL/BLD (green).
	 */
	if (layer->type != SUN8I_LAYER_TYPE_UI ||
	    layer->channel != SUN55I_DE_OVL_UI_LOGIC_CHN)
		return;

	if (!state->crtc || !state->visible || !fb) {
		sun55i_de_layer_disable(mixer);
		return;
	}

	if (sun55i_de_fmt(fb->format->format, &fmt)) {
		DRM_DEBUG_DRIVER("sun55i_de: unsupported format %p4cc\n",
				 &fb->format->format);
		sun55i_de_layer_disable(mixer);
		return;
	}

	if (fb->modifier != DRM_FORMAT_MOD_LINEAR &&
	    !sun55i_de_afbc_mod_supported(fb->format->format, fb->modifier)) {
		DRM_DEBUG_DRIVER("sun55i_de: unsupported modifier %016llx\n",
				 fb->modifier);
		sun55i_de_layer_disable(mixer);
		return;
	}

	/* source crop (16.16) and on-screen destination */
	src_w = drm_rect_width(&state->src) >> 16;
	src_h = drm_rect_height(&state->src) >> 16;
	dst_x = state->dst.x1;
	dst_y = state->dst.y1;
	dst_w = drm_rect_width(&state->dst);
	dst_h = drm_rect_height(&state->dst);

	addr = drm_fb_dma_get_gem_addr(fb, state, 0);

	/*
	 * The channel datapath always runs through the VSU8 scaler. Program it
	 * for the real src->dst ratio: the overlay outputs the src-sized crop
	 * and the VSU resamples it to the on-screen dst the blender expects.
	 */
	sun55i_de_vsu_setup(mixer, src_w, src_h, dst_w, dst_h);

	lay  = de_blk(mixer, SUN55I_DE_BLK_OVL_LAY0);
	para = de_blk(mixer, SUN55I_DE_BLK_OVL_PARA);
	attr = de_blk(mixer, SUN55I_DE_BLK_BLD_ATTR);
	ctl  = de_blk(mixer, SUN55I_DE_BLK_BLD_CTL);

	if (fb->modifier == DRM_FORMAT_MOD_LINEAR) {
		sun55i_de_afbd_disable(mixer);

		/* --- de_ovl layer 0 (de_ovl_u_apply_lay) --- */
		attctl = SUN55I_OVL_ATTCTL_EN |
			 SUN55I_OVL_ATTCTL_ALPHA_MODE(SUN55I_OVL_ATTCTL_GLOBAL_ALPHA) |
			 SUN55I_OVL_ATTCTL_FMT(fmt) | SUN55I_OVL_ATTCTL_ALPHA(0xff);
		de_blk_write(lay, SUN55I_OVL_LAY_ATTCTL, attctl);
		de_blk_write(lay, SUN55I_OVL_LAY_MBSIZE,
			     SUN55I_DE_OUT_SIZE(src_w, src_h));
		de_blk_write(lay, SUN55I_OVL_LAY_MBCOOR, 0);
		de_blk_write(lay, SUN55I_OVL_LAY_PITCH, fb->pitches[0]);
		de_blk_write(lay, SUN55I_OVL_LAY_TOP_LADDR, lower_32_bits(addr));
		de_blk_write(lay, SUN55I_OVL_LAY_BOT_LADDR, 0);

		/* top_haddr: high byte packed per layer; layer 0 -> bits[7:0] */
		de_blk_write(para, SUN55I_OVL_PARA_TOP_HADDR,
			     upper_32_bits(addr) & 0xff);
		de_blk_write(para, SUN55I_OVL_PARA_WIN_SIZE,
			     SUN55I_DE_OUT_SIZE(src_w, src_h));
	} else {
		/*
		 * AFBC: the AFBD replaces the OVL fetch (BSP:
		 * layer_en_cnt=0) and produces the same src window into the
		 * VSU; the rest of the datapath (VSU/BLD) does not change.
		 */
		de_blk_write(lay, SUN55I_OVL_LAY_ATTCTL, 0);
		sun55i_de_afbd_stage(mixer, fb, src_w, src_h);
	}

	/* --- blender: enable pipe 0, route to the channel, set geometry --- */
	de_blk_write(attr, SUN55I_BLD_ATTR_PIPE_EN, SUN55I_BLD_PIPE_EN(0));
	de_blk_write(attr, SUN55I_BLD_ATTR_PIPE_IN_SIZE(0),
		     SUN55I_DE_OUT_SIZE(dst_w, dst_h));
	de_blk_write(attr, SUN55I_BLD_ATTR_PIPE_IN_COORD(0),
		     (dst_x & 0xffff) | ((dst_y & 0xffff) << 16));
	de_blk_write(ctl, SUN55I_BLD_CTL_ROUT,
		     SUN55I_BLD_ROUT(0, SUN55I_DE_OVL_UI_LOGIC_CHN));
	de_blk_write(ctl, SUN55I_BLD_CTL_BLEND(0), SUN55I_BLD_BLEND_SRCOVER);

	/*
	 * TEMP diagnostic disabled: this runs on every page flip and a
	 * synchronous serial-console printk here (~9.5ms at 115200, IRQs off)
	 * delays the vblank IRQ that arms the RCQ, scattering the arm point
	 * across the frame -> moving tear.
	 */
	/*
	dev_info(mixer->dev,
		 "sun55i_de: layer fb=%ux%u %p4cc cpp=%u pitch=%u | src=%ux%u dst=%ux%u+%u+%u | fmt=%u hstep=%05x vstep=%05x addr=%pad\n",
		 fb->width, fb->height, &fb->format->format, fb->format->cpp[0],
		 fb->pitches[0], src_w, src_h, dst_w, dst_h, dst_x, dst_y, fmt,
		 sun55i_de_vsu_step(src_w, dst_w), sun55i_de_vsu_step(src_h, dst_h),
		 &addr);
	*/
}

/*
 * Stage the RCQ for arming: mark the heads dirty so the next arm reloads them.
 * The actual RCQ_CTL kick is *not* done here. atomic_flush runs at an arbitrary
 * point in the active scanout, and the RCQ load is not frame-gated by hardware,
 * so arming now applies the new datapath mid-frame -> tearing and momentary
 * background-colour flashes (the bug the vendor avoids by arming in blanking).
 * Defer the arm to the next blanking window in sun55i_de_vblank_quirk().
 */
void sun55i_de_commit(struct sun8i_mixer *mixer)
{
	struct sun55i_de *de = mixer->de;
	unsigned int i;

	lockdep_assert_held(&de->arm_lock);

	/*
	 * Convert each block ACCUMULATED dirty (only real changes, see
	 * de_blk_write) into a budget of 2 arms (arms_left). The dirty head
	 * the hardware consumes is written in do_arm: an RCQ load can drop
	 * blocks, and writing the head here (and only once) left the drop
	 * without retry forever (see arms_left in the .h). Full reload until
	 * the datapath has been armed once / during the full-arms of a
	 * mode_set. flip_min=N forces the old behaviour (everything dirty
	 * always).
	 */
	for (i = 0; i < SUN55I_DE_BLK_NUM; i++) {
		struct sun55i_de_block *blk = &de->blocks[i];
		bool dirty = !flip_min || !de->armed_once ||
			     de->full_arms_left > 0 ||
			     blk->stage_dirty;

		if (dirty)
			blk->arms_left = 2;
		blk->stage_dirty = false;
	}

	/* ensure the staged shadow is visible to the DMA before any arm */
	wmb();

	/*
	 * Quiesced (CRTC inactive): the enable() full reload will apply the
	 * shadow staged above; an arm now would poke a core held in reset.
	 */
	if (de->quiesced)
		return;

	/* TEMP: a stage landing while one is still pending = no vblank in between */
	if (de->arm_pending) {
		de->coalesced++;
		dbg_coalesced++;
	}
	de->commit_seq++;
	dbg_commits++;
	de->arm_pending = true;

	/*
	 * A dead arm chain = a stuck frame with not ONE error in dmesg
	 * (the compositor keeps committing, the holds do not age and the DE
	 * scans the old fb forever). Catch it: ~2 s of commits with no arm
	 * applying them.
	 */
	if (++de->stall_commits == 120)
		pr_warn("sun55i_de: 120 commits without an arm (arm_seq=%u); arm chain stalled\n",
			de->arm_seq);

	/*
	 * Backstop for the initial arm (wedge fix 2026-07-18): the first arm
	 * of a commit depended 100% on the TCON vblank IRQ (the hrtimer was
	 * only armed from do_arm for retries); if that IRQ dies, arm_pending
	 * stays pending forever. Scheduled at the next safe point; if the
	 * vblank IRQ arrives first it reclaims the timer
	 * (hrtimer_try_to_cancel in vblank_quirk) and arms, so in normal
	 * operation this timer never gets to fire.
	 */
	if (!hrtimer_active(&de->arm_timer) && de->ns_per_line && de->line_total)
		hrtimer_start(&de->arm_timer,
			      ns_to_ktime((u64)sun55i_de_lines_to_target(de, false) *
					  de->ns_per_line),
			      HRTIMER_MODE_REL);
}

/*
 * Staging exclusion for the atomic-flush path (sun8i_mixer_commit): the whole
 * plane-staging loop + sun55i_de_commit() run under arm_lock, so an arm from
 * the vblank IRQ or the hrtimer - live between commits thanks to the retry
 * budgets (arms_left/full_arms_left) - can never apply a half-written shadow
 * (e.g. a new OVL low address with the old high address/pitch: a fetch from a
 * nonexistent IOVA -> IOMMU fault -> wedged MBUS port).
 */
void sun55i_de_stage_lock(struct sun8i_mixer *mixer, unsigned long *flags)
{
	spin_lock_irqsave(&mixer->de->arm_lock, *flags);
}

void sun55i_de_stage_unlock(struct sun8i_mixer *mixer, unsigned long *flags)
{
	spin_unlock_irqrestore(&mixer->de->arm_lock, *flags);
}

/*
 * Issue the RCQ load (RCQ_CTL = 1) / the AHB copy. Caller must hold arm_lock.
 * The beam position is (re)checked HERE, covering every caller: returns 0 when
 * the arm was issued, or the number of counter lines to wait before retrying
 * when the beam was outside the safe window (the caller schedules the hrtimer;
 * the timer callback uses hrtimer_forward). Mirrors de_top_set_rcq_update().
 */
/*
 * TEMP C4 black-screen diagnostic: dump the RCQ header + shadow of the
 * key blocks on each arm. Enable with
 * /sys/module/sun8i_mixer/parameters/dump_arms = N.
 */
static unsigned int dbg_dump_arms;
module_param_named(dump_arms, dbg_dump_arms, uint, 0644);

static void sun55i_de_dump_blocks(struct sun55i_de *de, const char *tag,
				  u32 status)
{
	static const unsigned int ids[] = {
		SUN55I_DE_BLK_BLD_ATTR, SUN55I_DE_BLK_BLD_CTL,
		SUN55I_DE_BLK_OVL_LAY0,
	};
	unsigned int k;

	pr_info("rcqdbg %s seq=%u sts=%03x\n", tag, de->arm_seq, status);
	for (k = 0; k < ARRAY_SIZE(ids); k++) {
		struct sun55i_de_block *b = &de->blocks[ids[k]];
		const __le32 *sh = b->shadow;

		pr_info("rcqdbg  blk%u head={a=%08x dw0=%08x d=%u off=%06x} sh={%08x %08x %08x %08x %08x}\n",
			ids[k], le32_to_cpu(b->head->low_addr),
			le32_to_cpu(b->head->dw0),
			le32_to_cpu(b->head->dirty),
			le32_to_cpu(b->head->reg_offset),
			le32_to_cpu(sh[0]), le32_to_cpu(sh[1]),
			le32_to_cpu(sh[2]), le32_to_cpu(sh[3]),
			le32_to_cpu(sh[4]));
	}
}

static u32 sun55i_de_do_arm(struct sun55i_de *de)
{
	u32 status = 0;

	/*
	 * Beam-gate on EVERY arm (wedge fix 2026-07-18). Before, only the
	 * vblank IRQ arm checked the beam; the hrtimer ones (retries) ran in
	 * open loop at line_total*ns_per_line per hop (truncated ns,
	 * CLOCK_MONOTONIC vs the real PLL dotclock) and, since vblank_quirk
	 * skipped the arm while the timer was active, with sustained commits
	 * at 60 fps the timer was the ONLY arm path: the point drifted across
	 * the whole frame and the AHB blocks were applied mid scanout /
	 * prefetch of the DE. An OVL<->AFBD toggle (wlroots direct-scanout
	 * transition) applied mid-fetch is the prime suspect for wedging the
	 * channel config latch (stuck frame 2026-07-18). Re-reading the real
	 * line here covers all callers; outside the safe window
	 * [0, line_active) the arm is deferred to the next target, capped
	 * at 3 hops (an unreadable/broken counter can only delay the chain,
	 * never stop it: an occasional tear > frozen forever).
	 */
	if (tcon_curline_io && de->line_active && de->ns_per_line) {
		u32 cur = readl(tcon_curline_io) & 0xfff;

		if (cur >= de->line_active && de->arm_gate_skips < 3) {
			de->arm_gate_skips++;
			dbg_arm_gated++;
			return sun55i_de_lines_to_target(de, false);
		}
	}
	de->arm_gate_skips = 0;

	regmap_read(de->mixer->top_regs, SUN55I_MIXER_RCQ_STATUS, &status);
	dbg_status = status;

	if (dbg_dump_arms) {
		dbg_dump_arms--;
		sun55i_de_dump_blocks(de, "pre-arm", status);
	}

	/*
	 * A previous load is still in flight: don't stomp it. Leave arm_pending
	 * set and retry on the next vblank (BUSY clears well within one frame).
	 */
	if (arm_busy_gate && (status & SUN55I_MIXER_RCQ_STATUS_BUSY)) {
		dbg_arm_busy++;
		return 0;
	}

	/*
	 * RCQ (DMA) transport: a kicked load walks the head array and shadow
	 * asynchronously. FINISHED is W1C and cleared before every kick, so it
	 * being unset here means the previous load may still be reading -
	 * rewriting the heads below would hand the walker torn data. Skip and
	 * retry next vblank (arm_pending stays set). Bounded: a load the
	 * hardware dropped might never report FINISHED, so never skip more
	 * than 3 in a row - the shadow is idempotent and an occasional stomp
	 * is strictly better than arming never again. N/A to the AHB path,
	 * where the CPU copy completes synchronously under arm_lock.
	 */
	if ((use_rcq || !de->mmio) && de->rcq_kicked &&
	    !(status & SUN55I_MIXER_RCQ_STATUS_FINISHED)) {
		if (de->rcq_skips < 3) {
			de->rcq_skips++;
			dbg_arm_busy++;
			return 0;
		}
	}
	de->rcq_skips = 0;

	de->arm_pending = false;
	de->armed_once = true;
	de->stall_commits = 0;

	/*
	 * Write the head dirty flags the hardware consumes NOW, from the
	 * per-block arms_left budget (2 arms per staged change, see the .h):
	 * a dropped load gets one guaranteed retry, and re-applying the
	 * idempotent shadow is harmless.
	 */
	{
		/* a pending post-modeset full reload marks everything dirty */
		bool full = de->full_arms_left > 0;
		bool more = false;
		unsigned int i;

		for (i = 0; i < SUN55I_DE_BLK_NUM; i++) {
			struct sun55i_de_block *blk = &de->blocks[i];

			if (full || blk->arms_left) {
				blk->head->dirty = cpu_to_le32(1);
				if (blk->arms_left) {
					blk->arms_left--;
					if (blk->arms_left)
						more = true;
				}
			} else {
				blk->head->dirty = cpu_to_le32(0);
			}
		}
		/* heads visible to the RCQ DMA before the load triggers */
		wmb();
		if (more)
			de->arm_pending = true;
	}

	/*
	 * Post-modeset full reloads: one load does not reliably latch every
	 * block, so keep re-arming the (still fully-dirty) list on subsequent
	 * vblanks until the retry budget drains. The shadow is idempotent -
	 * re-applying it is harmless - and the arms land in blanking like any
	 * other, so the datapath converges to the staged config within a few
	 * frames even when an individual load is dropped.
	 */
	if (de->full_arms_left) {
		de->full_arms_left--;
		if (de->full_arms_left)
			de->arm_pending = true;
	}
	de->arm_seq++;

	if (!use_rcq && de->mmio) {
		/*
		 * AHB mode (BSP de_update_mode enum): copy the dirty blocks
		 * to their registers by CPU, right here (we are already in the
		 * blanking window). On the A523 the RCQ DMA says FINISH but
		 * does not apply the BLD blocks (probable internal bus gate,
		 * C4B); AHB does reach all registers.
		 */
		unsigned int i, w;

		for (i = 0; i < SUN55I_DE_BLK_NUM; i++) {
			struct sun55i_de_block *blk = &de->blocks[i];
			const __le32 *sh = blk->shadow;
			void __iomem *dst = de->mmio + blk->reg_off;

			if (!le32_to_cpu(blk->head->dirty))
				continue;
			for (w = 0; w < blk->size / 4; w++)
				writel(le32_to_cpu(sh[w]), dst + w * 4);
			blk->head->dirty = cpu_to_le32(0);
		}
	} else {
		/*
		 * W1C-clear the latched state first so FINISHED unambiguously
		 * refers to THIS load at the next arm (vendor
		 * de_top_query_state_with_clear semantics).
		 */
		regmap_write(de->mixer->top_regs, SUN55I_MIXER_RCQ_STATUS,
			     SUN55I_MIXER_RCQ_STATUS_W1C);
		regmap_write(de->mixer->top_regs, SUN55I_MIXER_RCQ_CTL, 1);
		de->rcq_kicked = true;
	}

	/* TEMP: record the TCON scanline the arm actually landed on */
	if (tcon_curline_io) {
		int line = readl(tcon_curline_io) & 0xfff;

		dbg_arm_line_last = line;
		if (line < dbg_arm_line_min)
			dbg_arm_line_min = line;
		if (line > dbg_arm_line_max)
			dbg_arm_line_max = line;
	}

	/* this arm retires fb holds two arms old (see sun55i_de.h) */
	sun55i_de_age_fb_holds(de);

	/*
	 * Scheduling the retry (pending arms_left / full_arms_left) is the
	 * caller responsibility: both the vblank IRQ and the timer callback
	 * re-anchor each hop to the REAL TCON line via
	 * sun55i_de_lines_to_target(), never a fixed period in open
	 * loop (the accumulated drift of that scheme took the arms out of
	 * blanking: the 2026-07-18 wedge).
	 */
	return 0;
}

/*
 * Quiesce the datapath while the TCON is still running (CRTC disable, i.e.
 * every modeset). The channel keeps fetching its ACTIVE framebuffer until a
 * RCQ FINISH latches a new config, and the RCQ only applies on frame
 * boundaries: once the TCON stops, nothing can be latched anymore. Without
 * this wait, the disable staged by the atomic commit never lands, DRM frees
 * the framebuffer, and when the TCON restarts with the new mode the DE
 * resumes fetching the stale, now-unmapped address -> IOMMU fault on master
 * 5 -> the DE MBUS port wedges until a full reset (C4C).
 *
 * No RCQ dance can work here: by the time the CRTC disable runs, the helper
 * has already disabled the encoder/bridge chain and the TCON no longer
 * delivers frame pulses (observed live: STS frozen at 0x000/0x010 for 100ms,
 * not even FRAME_END re-latches), so a staged disable can never be applied.
 * Instead do what the vendor display_config(enable=0) does: clock-gate and
 * hold in reset the disp core in de_top - plain MMIO that works with the
 * timing dead. A gated core cannot fetch, so DRM can free the scanned
 * framebuffer; mode_set() ungates and the full reload re-stages everything.
 */
void sun55i_de_quiesce(struct sun8i_mixer *mixer, struct drm_crtc *crtc)
{
	struct sun55i_de *de = mixer->de;
	unsigned long flags;
	u32 val;

	/*
	 * Fence off the arm paths BEFORE touching the hardware. The vblank IRQ
	 * is still live here (quiesce runs before drm_crtc_vblank_off, and the
	 * retry budget keeps arm_pending set between commits), so clearing
	 * arm_pending without the lock leaves a window where another CPU's
	 * do_arm() writes AHB registers - or kicks the RCQ DMA - into a core
	 * this function is about to gate and hold in reset: an MBUS transaction
	 * against a block in reset is exactly the wedge this driver fights.
	 * Taking arm_lock drains any do_arm() in flight; quiesced makes both
	 * arm paths (vblank_quirk, arm_timer) no-ops from here on. Cancel the
	 * hrtimer only after that: a callback firing in between sees
	 * arm_pending == false and does nothing.
	 */
	spin_lock_irqsave(&de->arm_lock, flags);
	de->arm_pending = false;
	de->quiesced = true;
	/* stage the channel off: first config the reload applies on enable */
	sun55i_de_layer_disable(mixer);
	spin_unlock_irqrestore(&de->arm_lock, flags);

	hrtimer_cancel(&de->arm_timer);

	/* vendor __de_mod_clk_enable(disp0, 0): clk gate, then reset assert */
	regmap_read(mixer->detop_regs, SUN55I_DETOP_CLK, &val);
	regmap_write(mixer->detop_regs, SUN55I_DETOP_CLK,
		     (val & ~BIT(0)) | SUN55I_DETOP_CLK_KEY);
	regmap_read(mixer->detop_regs, SUN55I_DETOP_RESET, &val);
	regmap_write(mixer->detop_regs, SUN55I_DETOP_RESET,
		     (val & ~BIT(0)) | SUN55I_DETOP_CLK_KEY);

	/*
	 * BSP sunxi_de_disable(): hold the WHOLE DE in reset while the display
	 * is off. This aborts any MBUS transaction a teardown fault left
	 * unacknowledged (the wedge) at its source, instead of letting the
	 * hung port sit there - or drag the interconnect down - until the
	 * next enable. sun55i_de_enable() deasserts and reprograms everything.
	 * Note the asymmetry vs the core gate above is deliberate: this line
	 * resets the whole DE complex including the de33-clk/detop block, so
	 * it can only be released by the full re-init of enable(), never by a
	 * plane update.
	 */
	sun55i_de_bus_reset(de, true);

	/* in reset the DE cannot fetch: every held fb is retirable now */
	spin_lock_irqsave(&de->arm_lock, flags);
	list_splice_tail_init(&de->fbs_pending, &de->fbs_release);
	list_splice_tail_init(&de->fbs_armed1, &de->fbs_release);
	list_splice_tail_init(&de->fbs_armed2, &de->fbs_release);
	if (!list_empty(&de->fbs_release))
		schedule_work(&de->fb_release_work);
	spin_unlock_irqrestore(&de->arm_lock, flags);
}

/*
 * The arm target scanline: a point safely inside the leading blanking. The
 * vendor value (arm_target = ver_back_porch + 3% of ver_total) is used unless
 * overridden for on-hardware tuning.
 */
static u32 sun55i_de_arm_target(struct sun55i_de *de)
{
	if (arm_target_line >= 0)
		return arm_target_line;
	return de->arm_target;
}

/*
 * Counter lines to the next safe arm point (the vendor target, inside
 * the initial blanking), measured over the REAL TCON line. Every hrtimer
 * hop is computed with this: re-anchored to the beam each hop, never a
 * fixed period in open loop (the accumulated drift took the arms out of
 * blanking, see the do_arm beam-gate). @next_frame jumps the current
 * frame target even if it is still ahead (retries: max one arm per frame).
 */
static u32 sun55i_de_lines_to_target(struct sun55i_de *de, bool next_frame)
{
	u32 target = sun55i_de_arm_target(de);
	u32 cur, lines;

	if (!tcon_curline_io || !de->line_total)
		return de->line_total ? de->line_total : 1;

	cur = readl(tcon_curline_io) & 0xfff;
	if (!next_frame && cur < target)
		return target - cur;

	lines = (cur < de->line_total ? de->line_total - cur : 0) + target;
	return lines ? lines : 1;
}

/*
 * hrtimer callback: backstop for the initial arm (commit), deferred arm
 * (the vblank IRQ caught the beam outside the window) or retry
 * (arms_left/full_arms_left). Runs in hardirq context; the AHB copy is
 * non-blocking MMIO. Each hop re-anchors to the real TCON line (the
 * do_arm beam-gate returns the exact wait; for a retry it aims at the
 * next frame target) - never a fixed period in open loop (wedge fix 2026-07-18).
 */
static enum hrtimer_restart sun55i_de_arm_timer(struct hrtimer *t)
{
	struct sun55i_de *de = container_of(t, struct sun55i_de, arm_timer);
	enum hrtimer_restart ret = HRTIMER_NORESTART;
	unsigned long flags;
	u32 wait = 0;

	spin_lock_irqsave(&de->arm_lock, flags);
	if (de->arm_pending)
		wait = sun55i_de_do_arm(de);

	/*
	 * Keep the chain alive from the timer itself. quiesce() clears
	 * arm_pending under the lock first, so its hrtimer_cancel() never
	 * races against restart. wait != 0 = do_arm deferred the arm (beam-gate);
	 * otherwise a pending retry aims at the next frame target.
	 */
	if (de->arm_pending && de->ns_per_line && de->line_total) {
		if (!wait)
			wait = sun55i_de_lines_to_target(de, true);
		hrtimer_forward_now(t, ns_to_ktime((u64)wait * de->ns_per_line));
		ret = HRTIMER_RESTART;
	}
	spin_unlock_irqrestore(&de->arm_lock, flags);

	return ret;
}

/*
 * Arm a staged RCQ, gated to the vertical blanking region. Called from the TCON
 * vblank IRQ (sunxi_engine_ops.vblank_quirk) with @cur_line = the TCON's current
 * scanline. The RCQ latches ~immediately when armed and is not frame-gated, so
 * the arm must land while the beam is blanking:
 *
 *   - beam still safely inside the leading blanking -> arm now;
 *   - otherwise (the IRQ was serviced late, into active video, or past the arm
 *     target) -> schedule the hrtimer for the computed instant the beam next
 *     re-enters blanking and arm there.
 *
 * Either way the arm lands in blanking by construction, so IRQ-servicing jitter
 * (which otherwise scatters the arm - and the new fb address - across the frame,
 * the moving-tear bug) cannot place it in active video. This is the mainline
 * equivalent of the vendor measuring cur_line and delaying to the safe beam
 * position before de_top_set_rcq_update().
 */
void sun55i_de_vblank_quirk(struct sun8i_mixer *mixer, unsigned int cur_line)
{
	struct sun55i_de *de = mixer->de;
	unsigned long flags;
	u32 target, lines;

	if (!de)
		return;

	spin_lock_irqsave(&de->arm_lock, flags);

	if (!de->arm_pending)
		goto out;

	/*
	 * There may be a timer in flight (commit backstop, retry or deferred
	 * arm from the previous frame). The IRQ arm is anchored to the REAL
	 * vblank, so it takes precedence: reclaim the timer - unless its
	 * callback is already running on another CPU (it is spinning on
	 * arm_lock and will do the work itself as soon as we drop the lock).
	 * The old scheme (skipping the arm while the timer was active) made
	 * the timer the ONLY arm path under sustained commits, and its
	 * open-loop drift took the applies out of blanking (wedge 2026-07-18).
	 */
	if (hrtimer_try_to_cancel(&de->arm_timer) < 0)
		goto out;

	target = sun55i_de_arm_target(de);
	lines = 0;

	if (!de->ns_per_line) {
		/* no timing info (mode_set not run yet): arm now */
		sun55i_de_do_arm(de);
	} else if (cur_line <= target && !arm_at_target) {
		/* already before the target and not asked to wait: arm now */
		dbg_arm_now++;
		lines = sun55i_de_do_arm(de);
		/* pending retries: max one arm per frame, at the next target */
		if (!lines && de->arm_pending)
			lines = sun55i_de_lines_to_target(de, true);
	} else {
		/*
		 * Sleep to the target line and arm there. If the target is
		 * still ahead this frame, wait that many lines; otherwise wait
		 * out the rest of the frame to the target of the next one. The
		 * subtraction is guarded so an unexpectedly large cur_line can
		 * only shorten the wait, never underflow.
		 */
		if (cur_line <= target) {
			lines = target - cur_line;
		} else {
			u32 ahead = cur_line < de->line_total ?
				    de->line_total - cur_line : 0;

			lines = ahead + target;
		}
		de->arm_deferred++;
		dbg_arm_late++;
	}

	if (lines && de->ns_per_line)
		hrtimer_start(&de->arm_timer,
			      ns_to_ktime((u64)lines * de->ns_per_line),
			      HRTIMER_MODE_REL);

out:
	spin_unlock_irqrestore(&de->arm_lock, flags);
}
