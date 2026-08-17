/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Allwinner sun55iw3 (A523/A527/T527/H728) v35x display engine ("DE33"/v350).
 *
 * Unlike the H616 DE33, the v35x DE is meant to commit its datapath (blender,
 * output formatter, layers, ...) through the RCQ (Register Config Queue): a
 * DMA engine that walks an array of 16-byte "heads" and copies marked register
 * blocks from DRAM into the live DE register space.
 *
 * On the A523 the RCQ DMA reports FINISH without applying the blocks aimed at
 * the blender (C4B), so production runs the vendor's AHB update mode instead:
 * sun55i_de_do_arm() copies the staged shadow blocks to the registers by CPU,
 * beam-gated to the leading vertical blanking (an apply landing mid-scanout
 * tears at best and can hang the channel's config latch at worst - the
 * 2026-07-18 scanout wedge).
 *
 * The "de_top" control registers (RT_EN, OUT_SIZE, AUTO_CLK, DE2TCON_MUX, the
 * RCQ head pointer and RCQ_CTL itself) are plain MMIO and latch directly.
 */

#ifndef _SUN55I_DE_H_
#define _SUN55I_DE_H_

#include <linux/hrtimer.h>
#include <linux/spinlock.h>
#include <linux/types.h>

struct sun8i_mixer;
struct sun8i_layer;
struct drm_crtc;
struct drm_display_mode;
struct drm_plane_state;

/* one RCQ head: if dirty, DMA @len bytes from phys -> DE_base + reg_offset */
struct sun55i_de_rcq_head {
	__le32 low_addr;	/* data block phys [31:0] */
	__le32 dw0;		/* len[23:0] | high_addr[31:24] (phys [39:32]) */
	__le32 dirty;		/* bit0: copy this block this frame */
	__le32 reg_offset;	/* dest offset from DE base (0x5000000) */
};

/* a shadow register block in the coherent DMA pool, mirrored to reg_offset */
struct sun55i_de_block {
	u32				reg_off;	/* offset from DE base */
	u32				size;		/* bytes (2-byte aligned) */
	void				*shadow;	/* CPU view in DMA pool */
	dma_addr_t			phys;		/* DMA phys of @shadow */
	struct sun55i_de_rcq_head	*head;		/* its head in the array */
	/*
	 * SOFTWARE dirty accumulated since the last commit: de_blk_write
	 * sets it only when the value CHANGES and commit() flushes it to
	 * the head instead of overwriting it (the old flip_min clobbered
	 * the dirty of blocks with real changes -> the blender enable
	 * never traveled without a mode_set in between: C4 black screen).
	 */
	bool				stage_dirty;
	/*
	 * Arms this block still has to be consumed by. An RCQ load can
	 * DROP blocks (that is why full_arms_left re-arms after a mode_set);
	 * with dirty-only-on-change a dropped block was never retried:
	 * the shadow already matches what is staged, the hardware does not,
	 * and the next commit wrote dirty=0 to the head -> permanent
	 * divergence (seen live: BLD pipe_en=0 in HW with 0x101 in the
	 * shadow = eternal black screen with the OVL correctly programmed).
	 * Each staged change now applies over 2 consecutive arms; re-applying
	 * the shadow is idempotent.
	 */
	u8				arms_left;
};

enum {
	SUN55I_DE_BLK_BLD_ATTR,	/* blender pipe enables / per-pipe attrs */
	SUN55I_DE_BLK_BLD_CTL,	/* blender route / bg color / out size */
	SUN55I_DE_BLK_BLD_CK,	/* blender color-key / out color ctl */
	SUN55I_DE_BLK_FMT,	/* output formatter */
	SUN55I_DE_BLK_OVL_LAY0,	/* UI overlay (phys ch6) layer-0 regs */
	SUN55I_DE_BLK_OVL_LAY1,	/* layers 1-3: staged zeroed (disabled) */
	SUN55I_DE_BLK_OVL_LAY2,
	SUN55I_DE_BLK_OVL_LAY3,
	SUN55I_DE_BLK_OVL_PARA,	/* UI overlay (phys ch6) high-addr/win_size */
	SUN55I_DE_BLK_OVL_DS,	/* overlay coarse down-sample: zeroed (bypass) */
	/*
	 * VSU8 scaler (phys ch6) staged for a 1:1 passthrough. The channel
	 * datapath is always overlay->VSU->blender; the VSU filters every
	 * pixel, so it must hold unity coefficients + 1:1 size/step or it
	 * collapses the image. de_vsu8_set_para's 7 blocks (de_vsu.c).
	 */
	SUN55I_DE_BLK_VSU_CTL,		/* ctl.en + scale_mode */
	SUN55I_DE_BLK_VSU_ATTR,		/* out_size + glb_alpha */
	SUN55I_DE_BLK_VSU_YPARA,	/* y in_size + h/v step + phase */
	SUN55I_DE_BLK_VSU_CPARA,	/* c in_size + h/v step + phase */
	SUN55I_DE_BLK_VSU_COEFF0,	/* y_hori_coeff (unity) */
	SUN55I_DE_BLK_VSU_COEFF1,	/* y_vert_coeff (unity) */
	SUN55I_DE_BLK_VSU_COEFF2,	/* c_hori_coeff (unity) */
	SUN55I_DE_BLK_TFBD_CTL,	/* tiled-FB decoder ctrl: zeroed (de_tfbd_disable) */
	/*
	 * AFBD: AFBC (ARM FBC) decoder of UI channel phys 6
	 * (channel_base + 0x5000). When active it REPLACES the OVL fetch
	 * (the BSP sets layer_en_cnt=0) and produces the same window into
	 * the VSU->blender. BSP: lowlevel_de/afbd/de_fbd_atw.c (fbd_u_reg).
	 */
	SUN55I_DE_BLK_AFBD,
	SUN55I_DE_BLK_CCSC_CTL,	/* channel CSC ctl: zeroed (en=0 bypass) */
	SUN55I_DE_BLK_CDC_CTL,	/* channel CDC colour ctl: zeroed (de_cdc_disable) */
	SUN55I_DE_BLK_NUM
};

/**
 * struct sun55i_de - v35x RCQ engine state (one display pipe)
 * @heads:	CPU view of the RCQ head array (head[0..nblocks))
 * @heads_dma:	DMA address of the head array (programmed into RCQ_HEAD_*)
 * @pool:	base of the whole coherent allocation (heads + block data)
 * @pool_dma:	DMA address of @pool
 * @pool_size:	size of @pool
 * @nheads:	number of heads programmed (block count, padded even)
 * @blocks:	per-block shadow descriptors
 * @tcon_id:	hardware TCON index this pipe drives (HDMI = tcon2)
 */
struct sun55i_de {
	struct sun55i_de_rcq_head	*heads;
	dma_addr_t			heads_dma;
	void				*pool;
	dma_addr_t			pool_dma;
	size_t				pool_size;
	unsigned int			nheads;
	struct sun55i_de_block		blocks[SUN55I_DE_BLK_NUM];
	unsigned int			tcon_id;
	struct sun8i_mixer		*mixer;		/* back-ref for the timer */
	/*
	 * MMIO map of the whole DE (0x5000000+0x400000, like the reg of
	 * the BSP de@5000000) for AHB mode: copy the shadows to the
	 * registers by CPU in blanking, instead of the RCQ DMA (whose
	 * write path to BLD/VSU/FMT is gated on the A523: C4B).
	 */
	void __iomem			*mmio;

	/*
	 * CCU DE_BGR (0x0200160c): bit16 = RST_BUS_DE, the reset line of the
	 * whole DE complex. The BSP (sunxi_de_enable/disable) asserts it while
	 * the display is off and deasserts + fully re-initializes on enable -
	 * it is the only reset that clears a hung transaction on the DE's MBUS
	 * port (a fault during teardown leaves the fetch engine wedged: regs
	 * read back fine but nothing is fetched until this reset). Mapped raw
	 * because the reset framework handle is owned (exclusive) by the
	 * de33-clk node - and the de33-clk RST_MIXER0 (0x8008 bit0) is NOT a
	 * reset at all on v35x: that register is the MBUS clock gate (v1 bug).
	 */
	void __iomem			*bus_reset;

	/*
	 * Beam-gated arm. The RCQ latches ~immediately when armed and is not
	 * frame-gated by hardware, so the arm (RCQ_CTL=1) must be issued while
	 * the beam is in the vertical blanking region or the new datapath -
	 * including the new framebuffer address - tears in mid-frame. The
	 * vblank IRQ that drives the arm is serviced with large, load-dependent
	 * latency, so arming straight from it drops the load at a random
	 * scanline. Instead we replicate the vendor BSP (disp_mgr_protect_reg_
	 * for_rcq): read the TCON's current scanline, and either arm now if the
	 * beam is safely inside the leading blanking, or schedule an hrtimer to
	 * fire at the computed time the beam next re-enters blanking and arm
	 * there. Either way the arm lands in blanking by construction,
	 * regardless of IRQ-servicing jitter.
	 */
	spinlock_t			arm_lock;	/* arm_pending + timer + shadow staging */
	struct hrtimer			arm_timer;	/* fires in next blanking */
	bool				arm_pending;	/* RCQ staged, not armed */
	bool				armed_once;	/* datapath fully loaded once */
	/*
	 * Set by quiesce(), cleared by enable(): the DE core is gated and held
	 * in reset, so nothing can fetch and no arm may be issued. While set,
	 * commits neither hold replaced fbs (a DPMS-off client would pin GEM
	 * without bound: no vblanks age the lists) nor mark an arm pending
	 * (the enable() full reload re-applies the whole staged shadow).
	 */
	bool				quiesced;
	/*
	 * RCQ (DMA) transport state: a kicked load walks the head array and
	 * shadow asynchronously. FINISHED (W1C, cleared before each kick)
	 * still unset at the next arm means the walker may still be reading:
	 * rewriting the heads then would hand it torn data, so that arm is
	 * skipped (bounded by rcq_skips so a dropped load cannot stall arming).
	 */
	bool				rcq_kicked;	/* a load was kicked since enable() */
	unsigned int			rcq_skips;	/* consecutive in-flight skips */
	/*
	 * Arm beam-gate (wedge fix 2026-07-18): do_arm() re-reads the real
	 * TCON line and defers the arm if the beam already entered active
	 * video ([0, line_active) is the safe window). arm_gate_skips caps
	 * consecutive deferrals (a broken counter can only delay the chain,
	 * never stop it); stall_commits counts commits staged since the
	 * last real arm to expose a dead arm chain in dmesg.
	 */
	unsigned int			arm_gate_skips;
	unsigned int			stall_commits;
	/*
	 * Full-reload arms left to issue after a modeset. A single full-reload
	 * RCQ load does not reliably latch every block (live-verified: the
	 * first apply after boot lands nothing; a repeat apply of the same
	 * list lands everything), so the first arms after a mode_set re-issue
	 * the full-dirty list on consecutive vblanks until this drains.
	 */
	unsigned int			full_arms_left;
	u32				line_total;	/* TCON line counter modulus */
	u32				line_active;	/* counter line where active video starts */
	u32				arm_target;	/* safe arm line (vendor calc) */
	u32				ns_per_line;	/* counter-tick period (ns) */

	/* TEMP instrumentation — revert with the dev_info diagnostics */
	u32				commit_seq;	/* commits staged */
	u32				arm_seq;	/* RCQ arms issued */
	u32				coalesced;	/* stages overwritten pre-arm */
	u32				arm_deferred;	/* arms pushed to the timer */

	/*
	 * Old-framebuffer holds: the BSP only releases a scanout buffer after
	 * the RCQ FINISH that applied its replacement
	 * (de_top_check_display_rcq_update_finish); mainline's atomic helper
	 * instead frees old fbs right after the vblank event, which can precede
	 * the (deferred, droppable) RCQ arm -> the DE keeps fetching a freed,
	 * IOMMU-unmapped buffer -> fault -> the MBUS port wedges. Session
	 * teardowns (the dying compositor destroys its GEM buffers) hit this
	 * every time. We take a reference on every replaced fb at commit and
	 * only drop it two arms later (pending -> armed1 -> armed2 -> release,
	 * one hop per arm; two frames covers a dropped load, the reason
	 * full_arms_left exists). All lists under arm_lock; the put runs in
	 * work context (GEM free can sleep).
	 */
	struct list_head		fbs_pending;	/* staged, not armed */
	struct list_head		fbs_armed1;	/* aged one arm */
	struct list_head		fbs_armed2;	/* aged two arms */
	struct list_head		fbs_release;	/* ready for the put */
	struct work_struct		fb_release_work;
};

extern bool sun55i_afbc_enable;	/* modparam afbc: advertise AFBC (WIP) */
bool sun55i_de_afbc_mod_supported(u32 drm_fmt, u64 modifier);
int sun55i_de_init(struct sun8i_mixer *mixer);
void sun55i_de_fini(struct sun8i_mixer *mixer);
void sun55i_de_stage_lock(struct sun8i_mixer *mixer, unsigned long *flags);
void sun55i_de_stage_unlock(struct sun8i_mixer *mixer, unsigned long *flags);
void sun55i_de_mode_set(struct sun8i_mixer *mixer,
			const struct drm_display_mode *mode);
void sun55i_de_enable(struct sun8i_mixer *mixer,
		      const struct drm_display_mode *mode);
void sun55i_de_layer_update(struct sun8i_mixer *mixer, struct sun8i_layer *layer,
			    struct drm_plane_state *state);
void sun55i_de_commit(struct sun8i_mixer *mixer);
void sun55i_de_hold_fb(struct sun8i_mixer *mixer, struct drm_framebuffer *fb);
void sun55i_de_quiesce(struct sun8i_mixer *mixer, struct drm_crtc *crtc);
void sun55i_de_vblank_quirk(struct sun8i_mixer *mixer, unsigned int cur_line);

#endif /* _SUN55I_DE_H_ */
