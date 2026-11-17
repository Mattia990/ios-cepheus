/*
 * Panel re-initialization at shim entry (built with -DSHIM_PANEL_INIT).
 *
 * Warm resets skip ABL's panel initialization, and XNU never re-initializes
 * the EA8076: SweetFramebuffer only sends the backlight DCS command.  TWRP,
 * which resets and re-inits the panel (dmesg: DSI_CMD_SET_OFF at 1.706 s,
 * DSI_CMD_SET_ON at 1.838 s) has working touch after the same warm reset,
 * while our FTS driver fails calibration 2-4 s after sense-on.  Replay the
 * live DTB's 34-command f1mp on-command sequence through the DSI0 command
 * FIFO before jumping to XNU, so the DDIC is in a known state when the
 * sensor scans.
 *
 * The FIFO image format is the one from downstream dsi_cmd_dma_add():
 *
 *   long:  wc_lo wc_hi dtype 0xc0 payload... 0xff padding
 *   short: data0 data1 dtype 0x80
 *
 * and the kickoff sequence is dsi_ctrl_hw_cmn_reset_cmd_fifo() followed by
 * dsi_ctrl_hw_cmn_kickoff_fifo_command(): TPG ctrl, FIFO words (plus one
 * zero word when the word count is odd), embedded mode, DMA length, SW
 * trigger, poll CMD_MODE_DMA_DONE.  This is the same path patch 0136 uses
 * for the brightness command, which works on hardware.
 */
#include <stdint.h>
#include "log.h"
#include "panel.h"
#include "rpmh.h"
#include "sde.h"
#include "util.h"

#define DSI0_PA				0xae94000UL

#define DSI_HW_VERSION			0x0000
#define DSI_CTRL			0x0004
#define DSI_STATUS			0x0008
#define DSI_FIFO_STATUS			0x000c
#define DSI_COMMAND_MODE_DMA_CTRL	0x003c
#define DSI_DMA_CMD_LENGTH		0x004c
#define DSI_CMD_MODE_DMA_SW_TRIGGER	0x0090
#define DSI_INT_CTRL			0x0110
#define DSI_TEST_PATTERN_GEN_CTRL	0x015c
#define DSI_TEST_PATTERN_GEN_CMD_DMA_INIT_VAL 0x017c
#define DSI_TPG_DMA_FIFO_RESET		0x01ec

#define DSI_INT_CMD_DMA_DONE		(1u << 0)
#define DSI_INT_ERROR			(1u << 24)

/* South TLMM, GPIO 6: panel reset, active low. */
#define TLMM_SOUTH_PA			0x03d00000UL
#define TLMM_CFG(n)			((uint32_t)(n) * 0x1000u)
#define TLMM_IO(n)			(TLMM_CFG(n) + 4u)
#define PANEL_RESET_PIN			6

/* North TLMM, GPIO 8: panel TE output, sampled to prove the rails drop. */
#define TLMM_NORTH_PA			0x03900000UL
#define PANEL_TE_PIN			8

/*
 * East TLMM, GPIO 21: disp_vci_vreg enable (panel VCI, 3.0 V).  The live DTB
 * has only two panel supplies, vddio (PM8150 LDO14, 1.8 V) and this VCI
 * switch, so dropping VCI is a real DDIC power-on reset; ELVDD/ELVSS are
 * internal to the EA8076.
 */
#define TLMM_EAST_PA			0x03500000UL
#define PANEL_VCI_PIN			21

/*
 * qcom,mdss-dsi-on-command of
 * /soc/qcom,mdss_dsi_samsung_fhd_ea8076_f1mp_cmd/.../timing@0
 * (458 bytes, 34 commands).  Each entry is the downstream command format:
 * type, last, vc, ack, wait-ms, len-hi, len-lo, payload...
 */
static const uint8_t f1mp_on[] = {
	0x39, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xf0, 0x5a, 0x5a, 0x39, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x03, 0xfc, 0x5a, 0x5a, 0x39, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x02, 0xb0, 0x0c, 0x39, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
	0xff, 0x10, 0x39, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xb0, 0x2f, 0x39,
	0x01, 0x00, 0x00, 0x00, 0x00, 0x02, 0xd1, 0x01, 0x39, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x03, 0xf0, 0xa5, 0xa5, 0x39, 0x01, 0x00, 0x00, 0x00, 0x00,
	0x03, 0xfc, 0xa5, 0xa5, 0x05, 0x01, 0x00, 0x00, 0x0a, 0x00, 0x02, 0x11,
	0x00, 0x39, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xf0, 0x5a, 0x5a, 0x39,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x35, 0x00, 0x39, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x03, 0xb7, 0x01, 0x4b, 0x39, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x02, 0xb0, 0x06, 0x39, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xb7, 0x10,
	0x39, 0x01, 0x00, 0x00, 0x00, 0x00, 0x03, 0xf0, 0xa5, 0xa5, 0x39, 0x01,
	0x00, 0x00, 0x00, 0x00, 0x05, 0x2b, 0x00, 0x00, 0x09, 0x23, 0x39, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x03, 0xf0, 0x5a, 0x5a, 0x39, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x03, 0xfc, 0x5a, 0x5a, 0x39, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x02, 0xb0, 0x23, 0x39, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xd1, 0x33,
	0x39, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0xe9, 0x11, 0x55, 0xa6, 0x75,
	0xa3, 0xa6, 0x37, 0xbe, 0x00, 0x1a, 0xb8, 0x39, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x0c, 0xd9, 0x14, 0x00, 0x00, 0x8f, 0x6e, 0x00, 0x00, 0x8f, 0x2e,
	0x6e, 0x34, 0x39, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6d, 0xc0, 0x31, 0x01,
	0x03, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x50, 0x00, 0x00, 0x40, 0x3d, 0x00, 0x5e, 0x00, 0x00,
	0x00, 0x20, 0x07, 0x20, 0x30, 0x58, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0xe0, 0x0b, 0x75, 0xd8, 0x00, 0x00, 0x00, 0x00, 0xfc, 0x00, 0x00, 0x00,
	0xf6, 0x97, 0x0f, 0x6d, 0xe6, 0xcd, 0x05, 0x21, 0x8b, 0xec, 0xed, 0xed,
	0xed, 0x0d, 0x3f, 0x60, 0x00, 0x67, 0xfd, 0x6d, 0x51, 0xbc, 0xdd, 0xdd,
	0x00, 0x00, 0x07, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x70, 0x2f, 0x00, 0x00, 0x03, 0x2e,
	0x3c, 0x20, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x8c, 0x39, 0x01,
	0x00, 0x00, 0x00, 0x00, 0x02, 0xc0, 0x31, 0x39, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x07, 0xe1, 0x00, 0x00, 0x02, 0x02, 0x42, 0x02, 0x39, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x07, 0xe2, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x39,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xb0, 0x0c, 0x39, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x02, 0xe1, 0x19, 0x39, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
	0xf0, 0xa5, 0xa5, 0x39, 0x01, 0x00, 0x00, 0x00, 0x00, 0x03, 0xfc, 0xa5,
	0xa5, 0x39, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x53, 0x20, 0x39, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x03, 0x51, 0x00, 0x00, 0x39, 0x01, 0x00, 0x00,
	0x43, 0x00, 0x02, 0x55, 0x00, 0x05, 0x01, 0x00, 0x00, 0x00, 0x00, 0x02,
	0x29, 0x00,
};

static uint32_t dsi_rd(uint32_t off)
{
	return rd32(DSI0_PA + off);
}

static void dsi_wr(uint32_t off, uint32_t val)
{
	wr32(DSI0_PA + off, val);
}

static void log_dsi(const char *tag)
{
	log_printf("panel: dsi %s hw %x ctrl %x status %x fifo %x int %x dma %x\n",
		   tag, dsi_rd(DSI_HW_VERSION), dsi_rd(DSI_CTRL),
		   dsi_rd(DSI_STATUS), dsi_rd(DSI_FIFO_STATUS),
		   dsi_rd(DSI_INT_CTRL), dsi_rd(DSI_COMMAND_MODE_DMA_CTRL));
}

static void reset_cmd_fifo(void)
{
	dsi_wr(DSI_TEST_PATTERN_GEN_CTRL, 0);
	dsi_wr(DSI_TPG_DMA_FIFO_RESET, 1);
	delay_ms(1);
	dsi_wr(DSI_TPG_DMA_FIFO_RESET, 0);
}

/* Send one already-packed FIFO image.  lpm selects the DSI LP command mode. */
static int send_image(const uint8_t *image, unsigned size, int lpm)
{
	uint32_t ctrl, ints;
	uint64_t deadline;

	reset_cmd_fifo();
	dsi_wr(DSI_TEST_PATTERN_GEN_CTRL, (1u << 1) | (1u << 2) | (0x3u << 16));
	for (unsigned i = 0; i < size; i += 4) {
		uint32_t w = (uint32_t)image[i] | ((uint32_t)image[i + 1] << 8) |
			     ((uint32_t)image[i + 2] << 16) |
			     ((uint32_t)image[i + 3] << 24);
		dsi_wr(DSI_TEST_PATTERN_GEN_CMD_DMA_INIT_VAL, w);
	}
	/* The hardware wants an extra zero DWORD for an odd word count. */
	if ((size / 4) & 1u)
		dsi_wr(DSI_TEST_PATTERN_GEN_CMD_DMA_INIT_VAL, 0);

	ctrl = dsi_rd(DSI_COMMAND_MODE_DMA_CTRL);
	ctrl &= ~((1u << 31) | (1u << 30) | (1u << 26));
	if (lpm)
		ctrl |= 1u << 26;
	ctrl |= 1u << 28;		/* embedded FIFO mode */
	dsi_wr(DSI_COMMAND_MODE_DMA_CTRL, ctrl);

	ints = dsi_rd(DSI_INT_CTRL);
	if (ints & DSI_INT_CMD_DMA_DONE)
		dsi_wr(DSI_INT_CTRL, ints & ~DSI_INT_ERROR);

	dsi_wr(DSI_DMA_CMD_LENGTH, size);
	__sync_synchronize();
	dsi_wr(DSI_CMD_MODE_DMA_SW_TRIGGER, 1);

	deadline = ticks_now() + rsr("cntfrq_el0") / 1000 * 50;
	while (!(dsi_rd(DSI_INT_CTRL) & DSI_INT_CMD_DMA_DONE)) {
		if (ticks_now() > deadline)
			return -1;
	}

	ints = dsi_rd(DSI_INT_CTRL);
	dsi_wr(DSI_INT_CTRL, ints & ~DSI_INT_ERROR);
	reset_cmd_fifo();
	return 0;
}

/* Build the DSI FIFO image for one DTB command and send it. */
static int send_dtb_cmd(const uint8_t *cmd)
{
	uint8_t image[160];
	uint8_t dtype = cmd[0];
	unsigned len = ((unsigned)cmd[5] << 8) | cmd[6];
	unsigned size;

	if (dtype == 0x39) {
		if (len > sizeof(image) - 4)
			return -2;
		image[0] = (uint8_t)len;
		image[1] = (uint8_t)(len >> 8);
		image[2] = dtype;
		image[3] = 0xc0;	/* long packet, last packet */
		for (unsigned i = 0; i < len; i++)
			image[4 + i] = cmd[7 + i];
		size = (4 + len + 3u) & ~3u;
		for (unsigned i = 4 + len; i < size; i++)
			image[i] = 0xff;
	} else {
		/* Short packet: data0, data1, type, last-packet flag. */
		image[0] = cmd[7];
		image[1] = cmd[8];
		image[2] = dtype;
		image[3] = 0x80;
		size = 4;
	}
	return send_image(image, size, 1);
}

/* DCS 0x51: set the panel backlight level (2..0x7ff, 0 = off). */
static int send_backlight(uint32_t level)
{
	uint8_t image[8] = {
		0x03, 0x00, 0x39, 0xc0,
		0x51, (uint8_t)(level >> 8), (uint8_t)level, 0xff,
	};
	/* The on-command set is dsi_lp_mode; send this in LP too. */
	return send_image(image, sizeof(image), 1);
}

void
panel_set_backlight(unsigned level)
{
	int rc = send_backlight(level);
	log_printf("panel: backlight %#x -> %d int %x\n", level, rc,
	    dsi_rd(DSI_INT_CTRL));
}

#if defined(SHIM_PANEL_CYCLE) || defined(SHIM_PHY_CYCLE)
/* Count edges on the panel's TE output for the given window. */
static unsigned panel_te_edges(unsigned ms)
{
	uintptr_t io = TLMM_NORTH_PA + TLMM_IO(PANEL_TE_PIN);
	unsigned last = rd32(io) & 1u, edges = 0;
	uint64_t end = ticks_now() + rsr("cntfrq_el0") / 1000 * ms;

	while (ticks_now() < end) {
		unsigned v = rd32(io) & 1u;
		if (v != last) {
			edges++;
			last = v;
		}
	}
	return edges;
}
#endif

#ifdef SHIM_PANEL_CYCLE
/* Short DCS write with no argument (display off 0x28, sleep in 0x10). */
static int send_dcs_short(uint8_t cmd)
{
	uint8_t image[9] = {
		0x05, 0x01, 0x00, 0x00, 0x00, 0x00, 0x02, cmd, 0x00,
	};
	return send_dtb_cmd(image);
}

/*
 * Panel power-on reset.  Test 0143/0144 showed that once the DDIC is put to
 * sleep (DCS 0x10) and loses VCIE/VDDIO, the shim cannot bring the DSI link
 * back: the 34-command replay completes at the controller but the panel
 * stays dark.  TWRP recovers because dsi_panel_display_on() re-initializes
 * the DSI controller and PHY, which the shim does not have.
 *
 * So do the part of TWRP's cycle that changes the panel's analogue state --
 * reset low, VCIE off, PM8150 LDO14 (vddio) off/on -- without sending the
 * DCS sleep-in.  TE on GPIO 8 tells whether the DDIC really lost power:
 * edges before the drop, none while vddio is off means the RPMh vote works.
 *
 * If the RPMh probe or either vote fails, leave the panel exactly as ABL
 * left it: a partial cycle is worse than no cycle.
 */
static void panel_power_cycle(void)
{
	uintptr_t east = TLMM_EAST_PA;
	uint32_t vci = rd32(east + TLMM_IO(PANEL_VCI_PIN));
	unsigned te_before, te_off, te_up;
	int rc;

	if (shim_rpmh_probe("ldoa14")) {
		log_printf("panel: ldoa14 unavailable, keeping panel powered\n");
		return;
	}
	te_before = panel_te_edges(50);
	log_printf("panel: cycle vcie cfg %x io %x te 50ms %u edges\n",
		   rd32(east + TLMM_CFG(PANEL_VCI_PIN)), vci, te_before);
	sde_autorefresh(0);
	/* No reset yet: TE must show whether the rails themselves drop. */
	wr32(east + TLMM_IO(PANEL_VCI_PIN), vci & ~2u);
	rc = shim_rpmh_vreg("ldoa14", 1800, 0);
	log_printf("panel: vcie off, ldoa14 off -> %d\n", rc);
	if (rc) {
		log_printf("panel: restore after failed vddio off\n");
		goto restore;
	}
	delay_ms(20);
	te_off = panel_te_edges(50);
	log_printf("panel: te with rails off %u edges\n", te_off);
	rc = shim_rpmh_vreg("ldoa14", 1800, 1);
	log_printf("panel: ldoa14 on -> %d\n", rc);
	if (rc) {
		log_printf("panel: restore after failed vddio on\n");
		goto restore;
	}
	delay_ms(10);
	wr32(east + TLMM_IO(PANEL_VCI_PIN), vci | 2u);
	delay_ms(10);		/* vcie qcom,supply-post-on-sleep = 10 ms */
	te_up = panel_te_edges(50);
	log_printf("panel: rails cycled, vcie io %x te 50ms %u edges\n",
		   rd32(east + TLMM_IO(PANEL_VCI_PIN)), te_up);
	return;

restore:
	(void)shim_rpmh_vreg("ldoa14", 1800, 1);
	wr32(east + TLMM_IO(PANEL_VCI_PIN), vci | 2u);
	delay_ms(10);
}
#endif

#ifdef SHIM_PHY_CYCLE
/*
 * DSI0 PHY/controller digital cycle: the part of TWRP's non-cont-splash
 * dsi_display_prepare() that the shim's DCS replay alone was missing
 *
 *   dsi_display_phy_sw_reset()   controller PHY_SW_RESET
 *   dsi_display_phy_enable()    dsi_phy_hw_v4_0_disable/enable
 *   dsi_display_ctrl_host_enable() + dsi_ctrl_hw_cmn_soft_reset()
 *
 * The Q PHY driver re-programs the whole PHY from dsi_phy_cfg on enable.
 * The shim does not have those computed values, but it does have the live
 * ABL state, and the cold/warm dumps match word for word: snapshot the
 * registers the driver touches, re-enable the PHY, then write them back.
 * Only the volatile digital/link state is dropped.
 *
 * The PLL (0xae94900) is left running: TWRP logs "PLL is already enabled.
 * Skipping configuration." on this phone, and the panel is still off while
 * the PHY is reset.  The caller resets the panel and replays the on
 * commands immediately after this.
 */
#define DSI_PHY_PA		0xae94400UL
#define DSI_SOFT_RESET		0x0118
#define DSI_CLK_CTRL		0x011c
#define DSI_PHY_SW_RESET	0x012c
#define DSI_CTRL_DYNAMIC_FORCE_ON \
	(0x23fu | (1u << 8) | (1u << 9) | (1u << 11) | (1u << 21))

#define PHY_CMN_FIRST		0x010
#define PHY_CMN_END		0x140	/* PHY_STATUS (0x140) is read-only */

/* dsi_phy_hw_v4_0_lane_settings(): per-lane registers the driver writes. */
static const uint8_t phy_lane_off[7] = {
	0x00, 0x04, 0x08, 0x0c, 0x10, 0x14, 0x18,
};

static void panel_phy_cycle(void)
{
	static uint32_t cmn[(PHY_CMN_END - PHY_CMN_FIRST) / 4];
	uint32_t lanes[5][sizeof(phy_lane_off)];
	uintptr_t phy = DSI_PHY_PA;
	uint32_t ctrl, clk;
	unsigned i, j;

	for (i = 0; i < sizeof(cmn) / sizeof(cmn[0]); i++)
		cmn[i] = rd32(phy + PHY_CMN_FIRST + i * 4);
	for (i = 0; i < 5; i++)
		for (j = 0; j < sizeof(phy_lane_off); j++)
			lanes[i][j] = rd32(phy + 0x200 + 0x80 * i +
					   phy_lane_off[j]);

	log_printf("panel: phy entry ctrl0 %x pll %x rbuf %x vreg %x/%x ctrl3 %x cfg %x/%x clk %x/%x st %x\n",
		   rd32(phy + 0x24), rd32(phy + 0x3c), rd32(phy + 0x1c),
		   rd32(phy + 0x20), rd32(phy + 0x110), rd32(phy + 0x30),
		   rd32(phy + 0x34), rd32(phy + 0x38), rd32(phy + 0x10),
		   rd32(phy + 0x14), rd32(phy + 0x140));

	/* dsi_phy_hw_v4_0_disable(): LPRX off, lanes off, blocks off. */
	wr32(phy + 0x214, 0);
	wr32(phy + 0x24, rd32(phy + 0x24) & ~0x1fu);
	wr32(phy + 0xa0, 0);
	wr32(phy + 0x24, 0);
	__sync_synchronize();

	/* dsi_ctrl_hw_cmn_phy_sw_reset(): BIT(24)|BIT(0), 1 ms, 0, 100 us. */
	dsi_wr(DSI_PHY_SW_RESET, (1u << 24) | 1u);
	delay_ms(1);
	dsi_wr(DSI_PHY_SW_RESET, 0);
	delay_ms(1);

	/* dsi_phy_hw_v4_0_enable(): remove power down, then restore. */
	wr32(phy + 0x24, 0x7f);
	for (i = 0; i < sizeof(cmn) / sizeof(cmn[0]); i++)
		wr32(phy + PHY_CMN_FIRST + i * 4, cmn[i]);
	wr32(phy + 0xa0, 0x1f);
	for (i = 0; i < 5; i++)
		for (j = 0; j < sizeof(phy_lane_off); j++)
			wr32(phy + 0x200 + 0x80 * i + phy_lane_off[j],
			     lanes[i][j]);

	/* Wait for the REFGEN to come back (1 ms in the Q driver). */
	for (i = 0; i < 100 && !(rd32(phy + 0x140) & 1u); i++)
		delay_ms(1);
	log_printf("panel: phy status %x pll %x ctrl0 %x\n",
		   rd32(phy + 0x140), rd32(phy + 0x3c), rd32(phy + 0x24));

	/* dsi_phy_hw_v4_0_toggle_resync_fifo(). */
	wr32(phy + 0x1c, 0);
	__sync_synchronize();
	wr32(phy + 0x1c, 1);
	__sync_synchronize();

	/* dsi_ctrl_hw_cmn_soft_reset(). */
	ctrl = dsi_rd(DSI_CTRL);
	clk = dsi_rd(DSI_CLK_CTRL);
	dsi_wr(DSI_CTRL, ctrl & ~7u);
	dsi_wr(DSI_CLK_CTRL, clk | DSI_CTRL_DYNAMIC_FORCE_ON);
	dsi_wr(DSI_SOFT_RESET, 1);
	delay_ms(1);
	dsi_wr(DSI_SOFT_RESET, 0);
	dsi_wr(DSI_CLK_CTRL, clk);
	dsi_wr(DSI_CTRL, ctrl);
	log_printf("panel: phy cycle done ctrl %x clk %x status %x\n",
		   dsi_rd(DSI_CTRL), dsi_rd(DSI_CLK_CTRL), dsi_rd(0x8));
}
#endif

void panel_reinit(void)
{
	uintptr_t tlmm = TLMM_SOUTH_PA;
	uint32_t io, cfg, dma_ctrl;
	unsigned pos = 0, n = 0, errs = 0;

#ifdef SHIM_PANEL_CYCLE
	panel_power_cycle();
#endif
#ifdef SHIM_PHY_CYCLE
	panel_phy_cycle();
#endif
	log_dsi("entry");
	dma_ctrl = dsi_rd(DSI_COMMAND_MODE_DMA_CTRL);
	io = rd32(tlmm + TLMM_IO(PANEL_RESET_PIN));
	cfg = rd32(tlmm + TLMM_CFG(PANEL_RESET_PIN));
	log_printf("panel: reset gpio cfg %x io %x\n", cfg, io);

	/* qcom,mdss-dsi-reset-sequence = <0 1 1 10>: low 1 ms, high 10 ms. */
	wr32(tlmm + TLMM_IO(PANEL_RESET_PIN), io & ~2u);
	delay_ms(1);
	wr32(tlmm + TLMM_IO(PANEL_RESET_PIN), io | 2u);
	delay_ms(10);
	log_printf("panel: reset released, io %x\n",
		   rd32(tlmm + TLMM_IO(PANEL_RESET_PIN)));

	while (pos + 7 <= sizeof(f1mp_on)) {
		const uint8_t *cmd = f1mp_on + pos;
		unsigned len = ((unsigned)cmd[5] << 8) | cmd[6];
		unsigned wait = cmd[4];
		int rc;

		if (pos + 7 + len > sizeof(f1mp_on)) {
			log_printf("panel: cmd %u truncated at %u\n", n, pos);
			errs++;
			break;
		}
		rc = send_dtb_cmd(cmd);
		log_printf("panel: cmd %u t%02x len %u wait %u -> %d int %x\n",
			   n, cmd[0], len, wait, rc, dsi_rd(DSI_INT_CTRL));
		if (rc)
			errs++;
		if (wait)
			delay_ms(wait);
		pos += 7 + len;
		n++;
	}

	delay_ms(20);
	log_printf("panel: init %u cmds, %u errors, pos %u/%u\n", n, errs, pos,
		   (unsigned)sizeof(f1mp_on));
	if (!errs) {
		int rc = send_backlight(0x7ff);
		log_printf("panel: backlight 0x7ff -> %d int %x\n", rc,
			   dsi_rd(DSI_INT_CTRL));
	}
#if defined(SHIM_PANEL_CYCLE) || defined(SHIM_PHY_CYCLE)
	log_printf("panel: te after init %u edges\n", panel_te_edges(50));
#endif
	/* Leave the controller as ABL had it for the XNU scanout. */
	dsi_wr(DSI_COMMAND_MODE_DMA_CTRL, dma_ctrl);
	log_printf("panel: dma restored %x\n", dma_ctrl);
	log_dsi("exit");
}
