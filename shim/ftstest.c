/*
 * Pre-XNU FTS probe (-DSHIM_FTS_TEST): power the touch rails and run the same
 * bit-banged bring-up the XNU driver uses, but from the shim at ~1 s after
 * handoff, before any XNU code runs.  If the chip calibrates normally here,
 * the warm-boot poison is created by XNU's early boot; if it produces the
 * same right/top-edge ghost burst, the poison predates XNU (ABL warm path or
 * an always-on domain).
 *
 * The init is a faithful subset of work/fts/sweet_fts_i2c.c: VDDIO then
 * AVDD (RPMh ldoc1), reset low/high, controller-ready event, system-info
 * frame, sense-on, then FIFO events for a few seconds.
 *
 * -DSHIM_FTS_QUIET (with SHIM_FTS_TEST) replaces the FIFO polling loop with a
 * quiet window: sense-on, then six seconds with no I2C traffic at all, while
 * the TLMM latches falling edges on the IRQ pin in hardware.  The FIFO is
 * drained only afterwards, so it separates frames the chip generates on its
 * own from frames the host's polling provokes.
 *
 * -DSHIM_FTS_TWRPSEQ (with SHIM_FTS_TEST) replays TWRP's whole boot order
 * before the final scan: on, off, 50 ms, on, 350 ms, off, 250 ms, reset,
 * ready, on.  XNU's 0140/0142 cycles only reproduced the first toggle; with
 * SHIM_FTS_TWRPSEQ_PANEL the 250 ms gap contains TWRP's real DRM blank and
 * unblank (panel VCI/vddio drop, SET_ON replay), which is the one part of
 * its order the plain replay did not have.
 *
 * -DSHIM_FTS_TWRP_POWER powers the rails in TWRP's order instead: the driver
 * maps fts,pwr-reg-name="avdd" to L1 and fts,bus-reg-name="vdd" to the GPIO 84
 * fixed regulator, so Linux enables the analogue rail first, then the bus rail
 * (with its 4 ms startup delay), and drives reset only afterwards.  XNU and
 * the default shim do the opposite and hold reset low through the ramp.
 *
 * -DSHIM_FTS_PANELPWR (with SHIM_FTS_TEST) puts the panel through TWRP's real
 * blank/unblank first: dmesg 1.579-1.851 shows the DRM blank dropping the two
 * panel rails (PM8150 L14 vddio and the disp_vci_vreg switch on GPIO 21, per
 * dsi_amoled_panel_pwr_external_supply), then the unblank replaying SET_ON and
 * resetting the controller before the final sense-on.  TWRP's first scan sees
 * the same calibration events as ours, but the scan after the panel power
 * cycle is clean, so the DDIC analog state is what poisons the first
 * calibration.  Built with SHIM_PANEL_INIT/SHIM_PANEL_CYCLE, this calls
 * panel_reinit() here instead of at shim entry: real DDIC POR, SET_ON replay,
 * then the FTS power-up and TWRP sequence.
 */
#include <stdbool.h>
#include <stdint.h>

#include "ftstest.h"
#include "log.h"
#include "panel.h"
#include "rpmh.h"
#include "spmi.h"
#include "util.h"

#define FT_TLMM_SOUTH_PA 0x03d00000u	/* SDA 55 / SCL 56 / IRQ 122 */
#define FT_TLMM_NORTH_PA 0x03900000u	/* reset 12, VDDIO enable 84 */
#define FT_TLMM_EAST_PA  0x03500000u	/* panel VCI enable 21 */
#define FT_GPIO_CFG(n)  ((uint32_t)(n) * 0x1000u)
#define FT_GPIO_IO(n)   (FT_GPIO_CFG(n) + 4u)

#define FT_PIN_SDA	55
#define FT_PIN_SCL	56
#define FT_PIN_IRQ	122
#define FT_PIN_RESET	12
#define FT_PIN_VDDIO	84
#define FT_PIN_PANEL_RST	6	/* disp panel reset, south TLMM */
#define FT_PIN_PANEL_VCI	21	/* disp_vci_vreg enable, east TLMM */

#define FT_ADDR		0x49u
#define FT_SYSINFO_SIZE	208u
#define FT_EVENT_SIZE	8u
#define FT_RX_MAX	256u

#define FT_SETUP_US	4u
#define FT_LOW_US	2u
#define FT_HIGH_US	3u

#ifndef FT_POLL_MS
#define FT_POLL_MS	6000u		/* how long to watch the first scan */
#endif

/* How many events to print in the polling scan (the FIFO holds 32). */
#ifndef FT_LOG_MAX
#define FT_LOG_MAX	64u
#endif

/*
 * Panel-age test: after panel_reinit() the DDIC is freshly powered, while
 * under XNU the panel has been driven for ~30 s before the FTS test starts.
 * Wait this long with the panel lit before powering the touch chip, to see
 * whether DDIC state (not bus backpressure) is what poisons the calibration.
 */
#ifndef SHIM_FTS_PREDELAY_MS
#define SHIM_FTS_PREDELAY_MS 0
#endif

/* Repeat the panel POR after the age delay, i.e. right before FTS power-up. */
#ifndef SHIM_FTS_REPOR
#define SHIM_FTS_REPOR 0
#endif

/*
 * Force-cal recovery test: after the chip reports EVT_TYPE_STATUS_FORCE_CAL
 * with MS invalid (43 05 30) it stops reporting touches.  Wait for the
 * water-mode pair to finish, then send sense-off/on again and see whether
 * the chip re-emits its negative-detect status (43 05 01), i.e. whether the
 * driver can re-arm it without a panel or rail cycle.
 */
#ifndef SHIM_FTS_SOFT_RECOVER
#define SHIM_FTS_SOFT_RECOVER 0
#endif

/*
 * Panel-emission test: switch DCS 0x51 to 0 before the FTS rails come up and
 * back to SHIM_FTS_BLLEVEL after this many ms of scanning.  The OLED emission
 * changes the capacitive baseline the controller calibrates on; the warm
 * ghost fires ~3-4 s after sense-on, while this window keeps the panel dark.
 */
#ifndef SHIM_FTS_BLDIM_MS
#define SHIM_FTS_BLDIM_MS 0
#endif
#ifndef SHIM_FTS_BLLEVEL
#define SHIM_FTS_BLLEVEL 0x7ff
#endif

/*
 * -DSHIM_FTS_PREPROBE probes the controller on I2C before the rails are
 * enabled.  ABL should leave it unpowered (VDDIO GPIO 84 low, PM8150L L1
 * disabled), so a NACK is expected; an ACK means something else keeps it
 * alive, i.e. our "power cycle" never really removes power and the warm
 * state can survive in the chip.
 */
#ifndef SHIM_FTS_PREPROBE
#define SHIM_FTS_PREPROBE 0
#endif

/*
 * FIFO backpressure test: XNU prints every event synchronously to the serial
 * console (~6 ms per line at 115200 baud) while the shim logs to a RAM ring.
 * Delay this long after each logged event to model that stall.  If the chip
 * force-cals only when events are not drained promptly, this reproduces the
 * warm-boot 43 05 30 without any panel change.
 */
#ifndef SHIM_FTS_EVDELAY_MS
#define SHIM_FTS_EVDELAY_MS 0
#endif

/*
 * Optional wait before the rails are touched, with the panel left as ABL
 * powered it down (dark).  The fastboot-dwell test kept the logo lit, so this
 * is the only way to let panel/sensor charge decay before FTS calibration.
 */
#ifndef SHIM_FTS_DWELL_MS
#define SHIM_FTS_DWELL_MS 0
#endif

/*
 * Warm-boot diagnostic: keep the display panel unpowered while the touch chip
 * calibrates.  A warm reset never drops panel VCI (GPIO 21), so the DDIC
 * keeps internal state across the touch rails' off window; a cold boot powers
 * it from zero.  This drives VCI and panel reset low and holds them there for
 * SHIM_FTS_PANELOFF_MS before (and during) the FTS bring-up.
 */
#ifndef SHIM_FTS_PANELOFF_MS
#define SHIM_FTS_PANELOFF_MS 0
#endif

/*
 * -DSHIM_FTS_RESET2=N adds N hardware resets immediately after the 0xa6
 * sysinfo read and before any sense command.  This is the exact order that
 * made warm touch work in XNU (0125, reset2=1: power-up reset, sysinfo,
 * reset, sense-on) and the one ios-sweet's driver documents as the fix for
 * the same right/top-edge ghost burst ("a whole row/column of ghost contacts
 * ... 43 05 30").  The shim's default path has no reset there, and
 * SHIM_FTS_TWRPSEQ resets only after a sense on/off cycle, so this order has
 * never been tested from the shim.
 */
#ifndef SHIM_FTS_RESET2
#define SHIM_FTS_RESET2 0
#endif

/*
 * -DSHIM_FTS_NOISEPARAM replays what the driver cepheus ships
 * (drivers/input/touchscreen/st/fts.c 4.2.14, USE_NOISE_PARAM) does on every
 * resume: read the four noise parameters (C8 01 -> event 17 01 xx xx xx xx),
 * reset the controller, write them back (C7 01 <4 bytes> -> event 18 01 00),
 * then sense on.  TWRP's 5.2.4 driver and the XNU driver never send C7/C8.
 *
 * NEGATIVE (2026-11-18): firmware 0060 implements neither command.  `C8 01`
 * times out with scan on and off (2 s poll), and direct TWRP driver_test runs
 * of `C7 01` time out too, while sysinfo still answers.  4.2.14 also ignores
 * readNoiseParameters()' return and writes back its static zero buffer, so
 * this path is inert on Android as well.  Kept for reproducibility only.
 */
#ifndef SHIM_FTS_NOISEPARAM
#define SHIM_FTS_NOISEPARAM 0
#endif

/*
 * -DSHIM_FTS_BLANKCMD sends the three commands TWRP's driver writes when DRM
 * blanks the panel (fts.c fts_mode_handler, FOD path): A2 03 20 00 00 01
 * (long-press + gesture), A0 01 (scan mode low power, 2-byte form) and
 * C0 02 00 00 (single-tap only).  It then waits SHIM_FTS_BLANKCMD_MS and
 * relies on the caller's system reset before sense-on.  TWRP always runs this
 * between its probe and the final reset; no shim run has replayed it.
 */
#ifndef SHIM_FTS_BLANKCMD
#define SHIM_FTS_BLANKCMD 0
#endif
#ifndef SHIM_FTS_BLANKCMD_MS
#define SHIM_FTS_BLANKCMD_MS 250
#endif

/*
 * -DSHIM_FTS_TWRPFULL replays TWRP's exact warm order (fts.c,
 * CONFIG_FTS_FOD_AREA_REPORT), which gets real touch on a warm boot:
 *   sense-on -> ~300 ms -> DRM blank: sense-off, A2 03 20 00 00 01,
 *   A0 01 (low-power scan), C0 02 00 00 (single tap) -> 267 ms ->
 *   panel reinit (SHIM_FTS_TWRPFULL_PANEL=1) -> system reset -> sense-on.
 * The earlier BLANKCMD test ran the commands before the first sense-on, which
 * is not TWRP's order, and TWRPSEQ resets after its own cycle.  The final
 * sense-on is the normal one in fts_test, followed by the polling scan.
 */
#ifndef SHIM_FTS_TWRPFULL
#define SHIM_FTS_TWRPFULL 0
#endif
#ifndef SHIM_FTS_TWRPFULL_ON_MS
#define SHIM_FTS_TWRPFULL_ON_MS 300
#endif
#ifndef SHIM_FTS_TWRPFULL_OFF_MS
#define SHIM_FTS_TWRPFULL_OFF_MS 267
#endif
#ifndef SHIM_FTS_TWRPFULL_PANEL
#define SHIM_FTS_TWRPFULL_PANEL 1
#endif

/*
 * -DSHIM_FTS_CALQUIET_MS=N leaves the I2C bus completely idle for N ms after
 * the late sense-on, before the poll loop starts.  TWRP waits on the IRQ and
 * does not touch the bus while the chip runs its ~3 s calibration; every
 * shim scan that polled the FIFO every 5 ms ghosted or went silent, while
 * the one quiet (IRQ-only) scan came up clean.  This separates "the chip
 * fails by itself" from "the host's bit-bang traffic breaks the calibration".
 */
#ifndef SHIM_FTS_CALQUIET_MS
#define SHIM_FTS_CALQUIET_MS 0
#endif

/*
 * -DSHIM_FTS_IRQDRIVEN=1 makes the late window read the FIFO only when the
 * controller's IRQ line asserts (a latched falling edge, or a line that is
 * still low), exactly like TWRP's threaded handler, instead of reading an
 * empty FIFO every 5 ms.  Every warm shim scan that polled the empty FIFO
 * went silent ~3.6 s after sense-on; this separates the chip's own state from
 * the bit-bang traffic.  The FIFO read errors are logged too, so a silent
 * window cannot hide a failing read.
 */
#ifndef SHIM_FTS_IRQDRIVEN
#define SHIM_FTS_IRQDRIVEN 0
#endif

/*
 * -DSHIM_FTS_RETRY_MS runs a second complete bring-up in the same boot: after
 * the first quiet scan, the rails are dropped, held off for this many ms, then
 * brought up and initialized again and scanned again.  It answers whether the
 * first power-up itself leaves the chip misframed (then run 2 is clean) or
 * whether the poison lives outside the controller (both runs ghost).
 */
#ifndef SHIM_FTS_RETRY_MS
#define SHIM_FTS_RETRY_MS 0
#endif

/*
 * -DSHIM_FTS_PRECYCLE_MS=N brings the touch rails up for N ms and drops them
 * again (SHIM_FTS_PRECYCLE_OFF_MS, default 200) before the normal bring-up.
 * `fts-retry` showed that a second power-up in the same boot calibrates
 * cleanly while the first ghosts, so this tests how little conditioning the
 * controller needs before its real init.
 */
#ifndef SHIM_FTS_PRECYCLE_MS
#define SHIM_FTS_PRECYCLE_MS 0
#endif
#ifndef SHIM_FTS_PRECYCLE_OFF_MS
#define SHIM_FTS_PRECYCLE_OFF_MS 200
#endif

/*
 * -DSHIM_FTS_POWERUP_MS=N waits N ms after the AVDD vote before the first
 * reset (default 200, the Linux driver's only settle time).  A warm entry
 * starts from a fully discharged rail while a re-power starts partly charged,
 * so a longer first settle is the cheap alternative to a second power-up.
 */
#ifndef SHIM_FTS_POWERUP_MS
#define SHIM_FTS_POWERUP_MS 200
#endif

/*
 * -DSHIM_FTS_DUMMYSCAN_MS=N runs one throwaway sense-on for N ms after the
 * first reset, then power-cycles the rails and lets the real init continue.
 * fts-retry's run 2 was clean after run 1 had scanned for 6 s, but a rail
 * cycle alone (SHIM_FTS_PRECYCLE_MS) does not help, so the conditioning is
 * the scan itself (the chip's own failed calibration + force cal).
 */
#ifndef SHIM_FTS_DUMMYSCAN_MS
#define SHIM_FTS_DUMMYSCAN_MS 0
#endif
#ifndef SHIM_FTS_DUMMYSCAN_OFF_MS
#define SHIM_FTS_DUMMYSCAN_OFF_MS 300
#endif

/*
 * -DSHIM_FTS_LATE_TOUCH_MS=N runs a second bring-up after the display console
 * is up and polls the FIFO for N ms.  The early fts_test window starts with
 * the panel dark, so a person cannot see when to touch; this one shows
 * "CEPHEUS / shim probe v1" for the whole window.  The second power-up in the
 * same boot is the conditioned, clean one.
 */
#ifndef SHIM_FTS_LATE_TOUCH_MS
#define SHIM_FTS_LATE_TOUCH_MS 0
#endif

/*
 * -DSHIM_FTS_LATE_RESET_MS=N replays TWRP's second init: N ms after the first
 * sense-on, pulse the reset line *while the chip is scanning* (no sense-off),
 * wait for controller-ready, read sysinfo, then sense-on again.  This is what
 * TWRP does from fts_fw_update_auto: EXP_FN_WORK_DELAY_MS=1000 after probe,
 * fts_fw_update() calls fts_system_reset() and then fts_init_sensing()
 * (sense-on + setScanMode(ACTIVE,0) / 50 ms / (ACTIVE,1)).  XNU's reset2 and
 * the shim's SHIM_FTS_RESET2 both reset *before* the first sense-on, and
 * SHIM_FTS_TWRPSEQ senses off first, so this post-scan reset has never been
 * tested.  The only behavioural difference between TWRP and XNU/shim may be
 * that TWRP's warm chip is reset once more after it has calibrated.
 */
#ifndef SHIM_FTS_LATE_RESET_MS
#define SHIM_FTS_LATE_RESET_MS 0
#endif

/*
 * Panel/DDIC drain before the TWRP-style unblank (with SHIM_FTS_PANELPWR):
 * hold the DDIC in reset and drop BOTH panel supplies (VCIE on GPIO 21 and
 * PM8150 LDO14 vddio) for this many ms, then let panel_reinit() bring the
 * panel back up.  A cold power-on leaves the panel unpowered for minutes; a
 * warm reset never does, and ABL's display init only resets the DDIC without
 * a true supply POR.  If the ghost is charge or latch state left in the panel
 * or its DDIC, this window clears it.
 */
#ifndef SHIM_FTS_PANELDRAIN_MS
#define SHIM_FTS_PANELDRAIN_MS 0
#endif

/*
 * DSI-link quiet test: after the panel is initialized, power the DSI0 PHY
 * down before the FTS rails come up and leave it down for the whole scan.
 * Every panel experiment so far changed the DDIC's state but left the PHY
 * driving the link into the flex that runs next to the touch sensor, and a
 * warm reset never re-initializes it: XNU inherits ABL's live link, while
 * TWRP's dsi_display_prepare() re-programs the PHY.  If the warm ghost is
 * lane activity coupling into the FTS calibration, this removes it.
 */
#ifndef SHIM_FTS_DSIOFF
#define SHIM_FTS_DSIOFF 0
#endif

/*
 * PMIC GPIO test: release PM8150B (SPMI SID 2) GPIO5 at 0xc400 before the FTS
 * rails come up.  cepheus's vdd_boost_vreg is a regulator-fixed on that pin
 * with regulator-always-on and no DT consumer, so Linux/TWRP drives it high
 * at boot and only a full power-off returns it to its POR default; a warm
 * reset can carry it into the next boot.  The entry dump in shim.c prints the
 * pin's state before this runs, so one warm log shows both what ABL left and
 * whether releasing it clears the ghost.  Mode 0x40 = 0 (digital input) and
 * enable 0x46 bit 7 = 0 (master enable off) stop the pin driving anything.
 */
#ifndef SHIM_FTS_GPIO5OFF
#define SHIM_FTS_GPIO5OFF 0
#endif

/*
 * The opposite of the test above: TWRP's live pinctrl dump shows PM8150B GPIO5
 * driven as an output HIGH (mode 0x11, vin-1, pull-down, CMOS strength 3,
 * enable 0x80), because Linux's regulator-always-on vdd_boost_vreg enables it
 * at boot.  A warm entry into the shim has it back at the PMIC default (input,
 * 0x40=0, vin-0, strength 1) with only the master enable still set, so a warm
 * boot never has this rail.  If the touch AFE needs it (Xiaomi's DT gives it
 * no consumer, which is what an always-on board rail looks like), every warm
 * calibration would be bad while TWRP's is good.  Drive it exactly as TWRP
 * does before the FTS rails come up.
 */
#ifndef SHIM_FTS_GPIO5ON
#define SHIM_FTS_GPIO5ON 0
#endif

#define FT_PM8150B_GPIO5_PA 0xc400u

#define FT_DSI_PHY_PA	0xae94400u

#if SHIM_FTS_DSIOFF
/* dsi_phy_hw_v4_0_disable(): LPRX off, lanes off, blocks off. */
static void
ft_dsi_off(void)
{
	uintptr_t phy = FT_DSI_PHY_PA;
	uint32_t ctrl0 = rd32(phy + 0x24);
	uint32_t block = rd32(phy + 0xa0);
	uint32_t lprx = rd32(phy + 0x214);

	log_printf("fts test: dsi off entry ctrl0 %x pll %x rbuf %x vreg %x/%x ctrl3 %x cfg %x/%x st %x\n",
	    ctrl0, rd32(phy + 0x3c), rd32(phy + 0x1c), rd32(phy + 0x20),
	    rd32(phy + 0x110), rd32(phy + 0x30), rd32(phy + 0x34),
	    rd32(phy + 0x38), rd32(phy + 0x140));
	wr32(phy + 0x214, 0);
	wr32(phy + 0x24, ctrl0 & ~0x1fu);
	wr32(phy + 0xa0, 0);
	wr32(phy + 0x24, 0);
	__sync_synchronize();
	delay_ms(1);
	log_printf("fts test: dsi off lprx %x->%x ctrl0 %x->%x block %x->%x st %x\n",
	    lprx, rd32(phy + 0x214), ctrl0, rd32(phy + 0x24), block,
	    rd32(phy + 0xa0), rd32(phy + 0x140));
}
#endif

#if SHIM_FTS_GPIO5OFF
static void
ft_gpio5_off(void)
{
	static const uint16_t regs[] = {
		0x05, 0x10, 0x40, 0x41, 0x42, 0x43, 0x45, 0x46,
	};
	uint8_t mode = 0, en = 0;

	log_printf("fts test: gpio5 before:");
	for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
		uint8_t v = 0;
		int rc = spmi_read(2, FT_PM8150B_GPIO5_PA + regs[i], &v);
		log_printf(" %x=%x%s", regs[i], v, rc ? "!" : "");
	}
	log_printf("\n");
	spmi_read(2, FT_PM8150B_GPIO5_PA + 0x40, &mode);
	spmi_read(2, FT_PM8150B_GPIO5_PA + 0x46, &en);
	int mrc = spmi_write(2, FT_PM8150B_GPIO5_PA + 0x40, 0);
	int erc = spmi_write(2, FT_PM8150B_GPIO5_PA + 0x46, en & 0x7fu);
	delay_ms(1);
	uint8_t m2 = 0, e2 = 0;
	spmi_read(2, FT_PM8150B_GPIO5_PA + 0x40, &m2);
	spmi_read(2, FT_PM8150B_GPIO5_PA + 0x46, &e2);
	log_printf("fts test: gpio5 off mode %x->%x (%d) en %x->%x (%d)\n",
	    mode, m2, mrc, en, e2, erc);
}
#endif

#if SHIM_FTS_GPIO5ON
static void
ft_gpio5_on(void)
{
	static const uint16_t regs[] = {
		0x05, 0x10, 0x40, 0x41, 0x42, 0x43, 0x45, 0x46,
	};
	/* TWRP/Linux state: vin-1, pull-down, CMOS strength high, out high. */
	static const uint8_t want[] = { 0x41, 1, 0x42, 4, 0x45, 3, 0x40, 0x11, 0x46, 0x80 };

	log_printf("fts test: gpio5 before:");
	for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
		uint8_t v = 0;
		int rc = spmi_read(2, FT_PM8150B_GPIO5_PA + regs[i], &v);
		log_printf(" %x=%x%s", regs[i], v, rc ? "!" : "");
	}
	log_printf("\n");
	for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i += 2)
		spmi_write(2, FT_PM8150B_GPIO5_PA + want[i], want[i + 1]);
	delay_ms(5);	/* regulator-fixed startup-delay-us = 4000 */
	log_printf("fts test: gpio5 on:");
	for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
		uint8_t v = 0;
		int rc = spmi_read(2, FT_PM8150B_GPIO5_PA + regs[i], &v);
		log_printf(" %x=%x%s", regs[i], v, rc ? "!" : "");
	}
	log_printf("\n");
}
#endif

static void
ft_udelay(unsigned us)
{
	uint64_t hz = rsr("cntfrq_el0");
	uint64_t end = ticks_now() + (uint64_t)us * (hz / 1000000u);
	while (ticks_now() < end) {
		__asm__ volatile("yield" ::: "memory");
	}
}

/* TLMM: [1:0] pull, [5:2] function, [8:6] drive (mA/2 - 1), [9] output. */
static void
ft_gpio(uintptr_t base, unsigned pin, unsigned mux, unsigned pull,
    unsigned drive, bool out, bool high)
{
	uint32_t off = FT_GPIO_CFG(pin);
	if (out) {
		uint32_t io = (rd32(base + FT_GPIO_IO(pin)) & ~2u) |
		    (high ? 2u : 0u);
		wr32(base + FT_GPIO_IO(pin), io);
	}
	uint32_t cfg = rd32(base + off) & ~0x3ffu;
	cfg |= (mux << 2) | pull | (drive << 6) | (out ? 0x200u : 0u);
	wr32(base + off, cfg);
	__asm__ volatile("dsb sy" ::: "memory");
}

static void
ft_sda_low(void)
{
	ft_gpio(FT_TLMM_SOUTH_PA, FT_PIN_SDA, 0, 3, 0, true, false);
}

static void
ft_sda_release(void)
{
	ft_gpio(FT_TLMM_SOUTH_PA, FT_PIN_SDA, 0, 3, 0, false, false);
}

static void
ft_scl_low(void)
{
	ft_gpio(FT_TLMM_SOUTH_PA, FT_PIN_SCL, 0, 3, 0, true, false);
}

static bool
ft_sda_read(void)
{
	return (rd32(FT_TLMM_SOUTH_PA + FT_GPIO_IO(FT_PIN_SDA)) & 1u) != 0;
}

static bool
ft_scl_read(void)
{
	return (rd32(FT_TLMM_SOUTH_PA + FT_GPIO_IO(FT_PIN_SCL)) & 1u) != 0;
}

/* Release SCL and wait through any slave clock stretching (10 ms max). */
static bool
ft_scl_raise(void)
{
	ft_gpio(FT_TLMM_SOUTH_PA, FT_PIN_SCL, 0, 3, 0, false, false);
	for (unsigned i = 0; i < 10000; ++i) {
		if (ft_scl_read()) {
			return true;
		}
		ft_udelay(1);
	}
	return false;
}

static bool
ft_start(void)
{
	ft_sda_release();
	ft_udelay(1);
	if (!ft_scl_raise()) {
		return false;
	}
	ft_udelay(FT_SETUP_US);
	ft_sda_low();
	ft_udelay(FT_SETUP_US);
	ft_scl_low();
	ft_udelay(FT_LOW_US);
	return true;
}

static bool
ft_stop(void)
{
	ft_sda_low();
	ft_udelay(FT_LOW_US);
	bool ok = ft_scl_raise();
	ft_udelay(FT_SETUP_US);
	ft_sda_release();
	ft_udelay(FT_SETUP_US);
	return ok;
}

static bool
ft_write_bit(bool one)
{
	if (one) {
		ft_sda_release();
	} else {
		ft_sda_low();
	}
	ft_udelay(1);
	if (!ft_scl_raise()) {
		return false;
	}
	ft_udelay(FT_HIGH_US);
	ft_scl_low();
	ft_udelay(FT_LOW_US);
	return true;
}

static bool
ft_read_bit(bool *one)
{
	ft_sda_release();
	ft_udelay(1);
	if (!ft_scl_raise()) {
		return false;
	}
	ft_udelay(FT_HIGH_US);
	*one = ft_sda_read();
	ft_scl_low();
	ft_udelay(FT_LOW_US);
	return true;
}

static int
ft_write_byte(uint8_t byte)
{
	for (unsigned i = 0; i < 8; ++i) {
		if (!ft_write_bit((byte & 0x80u) != 0)) {
			return -1;
		}
		byte <<= 1;
	}
	ft_sda_release();
	ft_udelay(1);
	if (!ft_scl_raise()) {
		return -1;
	}
	ft_udelay(FT_HIGH_US);
	bool ack = !ft_sda_read();
	ft_scl_low();
	ft_udelay(FT_LOW_US);
	return ack ? 0 : -2;	/* -2: NACK */
}

static int
ft_read_byte(uint8_t *out, bool last)
{
	uint8_t byte = 0;
	for (unsigned i = 0; i < 8; ++i) {
		bool one;
		if (!ft_read_bit(&one)) {
			return -1;
		}
		byte = (uint8_t)((byte << 1) | (one ? 1u : 0u));
	}
	if (last) {
		ft_sda_release();
	} else {
		ft_sda_low();
	}
	ft_udelay(1);
	if (!ft_scl_raise()) {
		return -1;
	}
	ft_udelay(FT_HIGH_US);
	ft_scl_low();
	ft_udelay(FT_LOW_US);
	ft_sda_release();
	*out = byte;
	return 0;
}

static bool
ft_recover(void)
{
	ft_sda_release();
	ft_udelay(10);
	if (!ft_scl_raise()) {
		return false;
	}
	for (unsigned i = 0; i < 9 && !ft_sda_read(); ++i) {
		ft_scl_low();
		ft_udelay(FT_LOW_US);
		if (!ft_scl_raise()) {
			return false;
		}
		ft_udelay(FT_HIGH_US);
	}
	ft_scl_low();
	ft_udelay(FT_LOW_US);
	ft_sda_low();
	ft_udelay(FT_SETUP_US);
	bool ok = ft_scl_raise();
	ft_udelay(FT_SETUP_US);
	ft_sda_release();
	ft_udelay(FT_SETUP_US);
	return ok;
}

static int
ft_i2c_read(const uint8_t *cmd, unsigned cmdlen, uint8_t *buf, unsigned len)
{
	if (cmdlen == 0 || len > FT_RX_MAX) {
		return -1;
	}
	if (!ft_start()) {
		return -1;
	}
	int err = ft_write_byte((uint8_t)(FT_ADDR << 1));
	for (unsigned i = 0; !err && i < cmdlen; ++i) {
		err = ft_write_byte(cmd[i]);
	}
	if (!err && !ft_start()) {
		err = -1;
	}
	if (!err) {
		err = ft_write_byte((uint8_t)((FT_ADDR << 1) | 1u));
	}
	for (unsigned i = 0; !err && i < len; ++i) {
		err = ft_read_byte(&buf[i], i + 1u == len);
	}
	if (!ft_stop() && !err) {
		err = -1;
	}
	return err;
}

static int
ft_write_buf(const uint8_t *buf, unsigned len)
{
	if (!ft_start()) {
		return -1;
	}
	int err = ft_write_byte((uint8_t)(FT_ADDR << 1));
	for (unsigned i = 0; !err && i < len; ++i) {
		err = ft_write_byte(buf[i]);
	}
	if (!ft_stop() && !err) {
		err = -1;
	}
	return err;
}

static int
ft_fifo_read(uint8_t *ev)
{
	const uint8_t cmd = 0x86u;
	return ft_i2c_read(&cmd, 1, ev, FT_EVENT_SIZE);
}

/* Poll for the controller-ready event (0x03) like the XNU driver does. */
static int
ft_wait_ready(unsigned ms)
{
	uint8_t ev[FT_EVENT_SIZE];
	for (unsigned t = 0; t < ms; t += 5) {
		if (!ft_fifo_read(ev) && ev[0] == 0x03u) {
			return 0;
		}
		delay_ms(5);
	}
	return -1;
}

static void
ft_log_ev(const char *what, const uint8_t *ev)
{
	log_printf("fts test: %s %x %x %x %x %x %x %x %x\n", what, ev[0],
	    ev[1], ev[2], ev[3], ev[4], ev[5], ev[6], ev[7]);
}

#if SHIM_FTS_NOISEPARAM
/*
 * ftsTool.c readNoiseParameters: C8 01 makes the FW echo event 17 01 with
 * four noise bytes.  writeNoiseParameters: C7 01 + 4 bytes, event 18 01 00.
 * The FW keeps these in volatile state, hence the save/restore around reset.
 */
static int
ft_noise_read(uint8_t noise[4])
{
	static const uint8_t cmd[2] = { 0xc8u, 0x01u };
	uint8_t ev[FT_EVENT_SIZE];

	if (ft_write_buf(cmd, sizeof(cmd))) {
		return -1;
	}
	for (unsigned t = 0; t < 500; t += 5) {
		if (!ft_fifo_read(ev)) {
			if (ev[0] == 0x17u && ev[1] == 0x01u) {
				noise[0] = ev[2];
				noise[1] = ev[3];
				noise[2] = ev[4];
				noise[3] = ev[5];
				return 0;
			}
			if (ev[0] != 0x00u) {
				ft_log_ev("noise pre-read", ev);
			}
		}
		delay_ms(5);
	}
	return -1;
}

static int
ft_noise_write(const uint8_t noise[4])
{
	uint8_t cmd[6] = { 0xc7u, 0x01u, 0, 0, 0, 0 };
	uint8_t ev[FT_EVENT_SIZE];

	cmd[2] = noise[0];
	cmd[3] = noise[1];
	cmd[4] = noise[2];
	cmd[5] = noise[3];
	if (ft_write_buf(cmd, sizeof(cmd))) {
		return -1;
	}
	for (unsigned t = 0; t < 500; t += 5) {
		if (!ft_fifo_read(ev)) {
			if (ev[0] == 0x18u && ev[1] == 0x01u) {
				return ev[2] == 0x00u ? 0 : -1;
			}
			if (ev[0] != 0x00u) {
				ft_log_ev("noise pre-write", ev);
			}
		}
		delay_ms(5);
	}
	return -1;
}
#endif

#if defined(SHIM_FTS_QUIET) || SHIM_FTS_IRQDRIVEN
/*
 * TLMM per-pin interrupt registers (pinctrl-sm8150.c: +0x8 intr_cfg, +0xc
 * intr_status).  Falling edge: detection 2<<2, polarity 1<<1, RAW_STATUS_EN
 * 1<<4, enable 1<<0.  The status bit latches in hardware, so a short IRQ
 * pulse survives until this loop reads it, even with no I2C traffic.
 */
#define FT_GPIO_INTR_CFG(n)	(FT_GPIO_CFG(n) + 0x8u)
#define FT_GPIO_INTR_STATUS(n)	(FT_GPIO_CFG(n) + 0xcu)
#define FT_IRQ_FALL_CFG	((2u << 2) | (1u << 1) | (1u << 4) | (1u << 0))

static uint32_t
ft_irq_status(void)
{
	return rd32(FT_TLMM_SOUTH_PA + FT_GPIO_INTR_STATUS(FT_PIN_IRQ));
}

static uint32_t
ft_irq_level(void)
{
	return rd32(FT_TLMM_SOUTH_PA + FT_GPIO_IO(FT_PIN_IRQ)) & 1u;
}

static void
ft_irq_clear(void)
{
	wr32(FT_TLMM_SOUTH_PA + FT_GPIO_INTR_STATUS(FT_PIN_IRQ), 0);
	__asm__ volatile("dsb sy" ::: "memory");
}

/* Clear any stale latch, then arm falling-edge detection. */
static void
ft_irq_arm(void)
{
	ft_irq_clear();
	wr32(FT_TLMM_SOUTH_PA + FT_GPIO_INTR_CFG(FT_PIN_IRQ), FT_IRQ_FALL_CFG);
	__asm__ volatile("dsb sy" ::: "memory");
	ft_irq_clear();
}

/*
 * Quiet scan: one sense-on write, six seconds with only TLMM status reads,
 * then drain the FIFO.  Every latched edge is logged with its time, so the
 * ghost frames in the drain can be matched against IRQs the chip raised
 * while nothing on the host was touching I2C.
 */
static void
ft_quiet_scan(void)
{
	uint8_t buf[FT_EVENT_SIZE];
	uint8_t sense_on[3] = { 0xa0u, 0x00u, 0x01u };
	uint8_t sense_off[3] = { 0xa0u, 0x00u, 0x00u };
	uint64_t hz = rsr("cntfrq_el0");
	unsigned edges = 0, drained = 0;

	ft_irq_arm();
	log_printf("fts test: quiet arm cfg=%x st=%x io=%x\n",
	    rd32(FT_TLMM_SOUTH_PA + FT_GPIO_INTR_CFG(FT_PIN_IRQ)),
	    ft_irq_status(), ft_irq_level());

	int err = ft_write_buf(sense_on, sizeof(sense_on));
	log_printf("fts test: sense on err=%d\n", err);

	uint64_t t0 = ticks_now();
	for (;;) {
		uint64_t dt = (ticks_now() - t0) * 1000u / hz;
		if (dt >= FT_POLL_MS) {
			break;
		}
		uint32_t st = ft_irq_status();
		if (st & 1u) {
			if (edges < 64u) {
				log_printf("fts test: irq t=%u st=%x io=%x\n",
				    (unsigned)dt, st, ft_irq_level());
			}
			++edges;
			ft_irq_clear();
		}
		ft_udelay(1000);
	}
	log_printf("fts test: quiet done %u irqs, %u ms; st=%x io=%x\n",
	    edges, FT_POLL_MS, ft_irq_status(), ft_irq_level());

	/* Drain what accumulated while the host stayed off the bus. */
	for (unsigned i = 0; i < 64u; ++i) {
		uint64_t dt = (ticks_now() - t0) * 1000u / hz;
		int r = ft_fifo_read(buf);
		if (r) {
			log_printf("fts test: drain t=%u err=%d\n",
			    (unsigned)dt, r);
			break;
		}
		if (buf[0] == 0x00u) {
			log_printf("fts test: drain empty at %u\n", drained);
			break;
		}
		log_printf("fts test: drain t=%u ", (unsigned)dt);
		ft_log_ev("", buf);
		++drained;
	}
	log_printf("fts test: quiet end %u drained; st=%x io=%x\n", drained,
	    ft_irq_status(), ft_irq_level());

	err = ft_write_buf(sense_off, sizeof(sense_off));
	log_printf("fts test: sense off err=%d\n", err);
}
#endif

/* Leave the rails as ABL did (both off, reset asserted). */
static void ft_l1_log(const char *tag);

static void
ft_power_off(void)
{
	ft_gpio(FT_TLMM_NORTH_PA, FT_PIN_RESET, 0, 3, 3, true, false);
	(void)shim_rpmh_vreg("ldoc1", 1800, 0);
	ft_gpio(FT_TLMM_NORTH_PA, FT_PIN_VDDIO, 0, 0, 0, true, false);
	delay_ms(10);
	/* Read back: is the off vote actually in the PMIC? */
	ft_l1_log("off");
}

/* Drop panel VCI and hold the DDIC in reset; it stays off for the FTS test. */
#if SHIM_FTS_PANELOFF_MS > 0
static void
ft_panel_off(unsigned ms)
{
	ft_gpio(FT_TLMM_EAST_PA, FT_PIN_PANEL_VCI, 0, 0, 0, true, false);
	ft_gpio(FT_TLMM_SOUTH_PA, FT_PIN_PANEL_RST, 0, 0, 0, true, false);
	log_printf("fts test: panel off %u ms, vci=%u rst=%u\n", ms,
	    rd32(FT_TLMM_EAST_PA + FT_GPIO_IO(FT_PIN_PANEL_VCI)) & 1u,
	    rd32(FT_TLMM_SOUTH_PA + FT_GPIO_IO(FT_PIN_PANEL_RST)) & 1u);
	delay_ms(ms);
}
#endif

/*
 * True panel POR: reset asserted, VCIE switch off and the PM8150 LDO14
 * vddio vote withdrawn, held for `ms`, then restored.  Callers replay the
 * panel init afterwards (panel_reinit()).
 */
#if SHIM_FTS_PANELDRAIN_MS > 0
static void
ft_panel_drain(unsigned ms)
{
	uintptr_t east = FT_TLMM_EAST_PA;
	int off, on;

	ft_gpio(FT_TLMM_SOUTH_PA, FT_PIN_PANEL_RST, 0, 0, 0, true, false);
	ft_gpio(east, FT_PIN_PANEL_VCI, 0, 0, 0, true, false);
	off = shim_rpmh_vreg("ldoa14", 1800, 0);
	log_printf("fts test: panel drain %u ms, vci=%u rst=%u ldoa14 off=%d\n",
	    ms, rd32(east + FT_GPIO_IO(FT_PIN_PANEL_VCI)) & 1u,
	    rd32(FT_TLMM_SOUTH_PA + FT_GPIO_IO(FT_PIN_PANEL_RST)) & 1u, off);
	delay_ms(ms);
	on = shim_rpmh_vreg("ldoa14", 1800, 1);
	delay_ms(10);
	ft_gpio(east, FT_PIN_PANEL_VCI, 0, 0, 0, true, true);
	delay_ms(10);		/* vcie qcom,supply-post-on-sleep = 10 ms */
	log_printf("fts test: panel drain done, vci=%u ldoa14 on=%d\n",
	    rd32(east + FT_GPIO_IO(FT_PIN_PANEL_VCI)) & 1u, on);
}
#endif

static void
ft_l1_log(const char *tag)
{
	static const uint16_t regs[] = {
		0x04, 0x05, 0x08, 0x09, 0x0a, 0x0b, 0x40, 0x41, 0x42,
		0x43, 0x44, 0x45, 0x46, 0x48, 0x4c, 0x53, 0x60, 0x62,
		0x64, 0x88, 0xa0,
	};
	log_printf("fts test: %s pm8150l l1:", tag);
	for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
		uint8_t v = 0;
		int rc = spmi_read(5, 0x4000 + regs[i], &v);
		log_printf(" %x=%x%s", regs[i], v, rc ? "!" : "");
	}
	log_printf("\n");
}

/*
 * Poke the bus before any rail is enabled: ABL should leave the chip
 * unpowered, so its address must NACK (ft_write_byte -2).  An ACK means the
 * controller is still powered from the previous boot, i.e. a "power cycle"
 * that only toggles GPIO 84/L1 would not really remove its power.
 */
#if SHIM_FTS_PREPROBE
static void
ft_preprobe(void)
{
	bool rec = ft_recover();
	int ack = -1;
	if (rec && ft_start()) {
		ack = ft_write_byte((uint8_t)(FT_ADDR << 1));
		(void)ft_stop();
	}
	log_printf("fts test: preprobe recover=%d ack=%d sda=%u scl=%u\n",
	    rec, ack, ft_sda_read(), ft_scl_read());
}
#endif

void
fts_test(void)
{
	static uint8_t si[FT_SYSINFO_SIZE];
	uint8_t ev[FT_EVENT_SIZE];
	uint8_t sense_on[3] = { 0xa0u, 0x00u, 0x01u };
	uint8_t sense_off[3] = { 0xa0u, 0x00u, 0x00u };
	uint8_t sysinfo[3] = { 0xa6u, 0x00u, 0x00u };

	ft_l1_log("entry");

#if SHIM_FTS_PANELOFF_MS > 0
	ft_panel_off(SHIM_FTS_PANELOFF_MS);
#endif

#if SHIM_FTS_DWELL_MS > 0
	log_printf("fts test: dwell %u ms, panel off\n", SHIM_FTS_DWELL_MS);
	delay_ms(SHIM_FTS_DWELL_MS);
#endif

#ifdef SHIM_FTS_PANELPWR
	/* TWRP's blank/unblank: real DDIC POR (L14 + VCIE), SET_ON, reset. */
	log_printf("fts test: panel por before FTS power-up\n");
#if SHIM_FTS_PANELDRAIN_MS > 0
	ft_panel_drain(SHIM_FTS_PANELDRAIN_MS);
#endif
	panel_reinit();
#endif

#if SHIM_FTS_PREDELAY_MS > 0
	log_printf("fts test: panel-age delay %u ms, panel on\n",
	    SHIM_FTS_PREDELAY_MS);
	delay_ms(SHIM_FTS_PREDELAY_MS);
#endif

#if SHIM_FTS_REPOR
	log_printf("fts test: panel por again before FTS power-up\n");
	panel_reinit();
#endif

#if SHIM_FTS_BLDIM_MS > 0
	/* Panel stays initialized and scanning, but emits nothing. */
	panel_set_backlight(0);
#endif

#if SHIM_FTS_DSIOFF
	ft_dsi_off();
#endif

#if SHIM_FTS_GPIO5OFF
	ft_gpio5_off();
#endif

#if SHIM_FTS_GPIO5ON
	ft_gpio5_on();
#endif

#if SHIM_FTS_PREPROBE
	ft_preprobe();
#endif

#if SHIM_FTS_PRECYCLE_MS > 0
	/*
	 * Condition the rails before the real bring-up.  fts-retry's run 2 was
	 * clean after run 1 had powered and scanned, so this isolates how much
	 * of that (power on, time, power off) the first scan needs.
	 */
	{
		ft_gpio(FT_TLMM_NORTH_PA, FT_PIN_RESET, 0, 3, 3, true, false);
		ft_gpio(FT_TLMM_NORTH_PA, FT_PIN_VDDIO, 0, 0, 0, true, true);
		delay_ms(20);
		int pcerr = shim_rpmh_vreg("ldoc1", 1800, 1);
		delay_ms(SHIM_FTS_PRECYCLE_MS);
		ft_l1_log("precycle");
		ft_power_off();
		delay_ms(SHIM_FTS_PRECYCLE_OFF_MS);
		log_printf("fts test: precycle %u ms on, %u off, ldoc1 err=%d\n",
		    SHIM_FTS_PRECYCLE_MS, SHIM_FTS_PRECYCLE_OFF_MS, pcerr);
	}
#endif

#ifdef SHIM_FTS_TWRP_POWER
	/*
	 * TWRP/Linux power order: fts_enable_reg() enables info->vdd_reg
	 * (DT "avdd" = PM8150L L1) first, then info->avdd_reg (DT "vdd" =
	 * disp_vddio_vreg on GPIO 84, startup-delay-us 4000).  fts_set_gpio()
	 * then requests reset as an output low.  ABL leaves GPIO 12 as an
	 * input, so it floats through the rail ramp exactly as in Linux.
	 */
	int verr = shim_rpmh_vreg("ldoc1", 1800, 1);
	delay_ms(1);
	ft_gpio(FT_TLMM_NORTH_PA, FT_PIN_VDDIO, 0, 0, 0, true, true);
	delay_ms(4);
	log_printf("fts test: rails twrp order ldoc1 err=%d vddio=1\n", verr);
#else
	/* Reset first, then VDDIO (bus), then AVDD, as in the driver. */
	ft_gpio(FT_TLMM_NORTH_PA, FT_PIN_RESET, 0, 3, 3, true, false);
	ft_gpio(FT_TLMM_NORTH_PA, FT_PIN_VDDIO, 0, 0, 0, true, true);
	delay_ms(50);
	int verr = shim_rpmh_vreg("ldoc1", 1800, 1);
	delay_ms(SHIM_FTS_POWERUP_MS);
	log_printf("fts test: rails vddio=1 ldoc1 err=%d settle=%u\n", verr,
	    SHIM_FTS_POWERUP_MS);
	ft_l1_log("powered");
#endif

#ifdef SHIM_FTS_TWRP_POWER
	/* TWRP's fts_set_gpio drives reset immediately after the rails. */
	ft_gpio(FT_TLMM_NORTH_PA, FT_PIN_RESET, 0, 3, 3, true, false);
	delay_ms(10);
	ft_gpio(FT_TLMM_NORTH_PA, FT_PIN_RESET, 0, 3, 3, true, true);
	delay_ms(50);
	ft_l1_log("powered");
#endif

	/* SDA/SCL open-drain inputs with pull-up; IRQ input. */
	ft_gpio(FT_TLMM_SOUTH_PA, FT_PIN_SDA, 0, 3, 0, false, false);
	ft_gpio(FT_TLMM_SOUTH_PA, FT_PIN_SCL, 0, 3, 0, false, false);
	ft_gpio(FT_TLMM_SOUTH_PA, FT_PIN_IRQ, 0, 3, 3, false, false);
	if (!ft_recover()) {
		log_printf("fts test: scl stuck low after power-up\n");
		ft_power_off();
		return;
	}

#ifndef SHIM_FTS_TWRP_POWER
	/* Reset low 10 ms, high 50 ms, then wait for ready. */
	ft_gpio(FT_TLMM_NORTH_PA, FT_PIN_RESET, 0, 3, 3, true, false);
	delay_ms(10);
	ft_gpio(FT_TLMM_NORTH_PA, FT_PIN_RESET, 0, 3, 3, true, true);
	delay_ms(50);
#endif
#if SHIM_FTS_DUMMYSCAN_MS > 0
	{
		static const uint8_t d_on[3] = { 0xa0u, 0x00u, 0x01u };
		static const uint8_t d_off[3] = { 0xa0u, 0x00u, 0x00u };
		int dr = ft_wait_ready(500);
		int d1 = ft_write_buf(d_on, sizeof(d_on));
		delay_ms(SHIM_FTS_DUMMYSCAN_MS);
		int d2 = ft_write_buf(d_off, sizeof(d_off));
		log_printf("fts test: dummy scan ready=%d on=%d off=%d %u ms\n",
		    dr, d1, d2, SHIM_FTS_DUMMYSCAN_MS);
		ft_power_off();
		delay_ms(SHIM_FTS_DUMMYSCAN_OFF_MS);
		ft_gpio(FT_TLMM_NORTH_PA, FT_PIN_RESET, 0, 3, 3, true, false);
		ft_gpio(FT_TLMM_NORTH_PA, FT_PIN_VDDIO, 0, 0, 0, true, true);
		delay_ms(50);
		int dverr = shim_rpmh_vreg("ldoc1", 1800, 1);
		delay_ms(SHIM_FTS_POWERUP_MS);
		log_printf("fts test: dummy rails vddio=1 ldoc1 err=%d\n", dverr);
		ft_l1_log("powered2");
		ft_gpio(FT_TLMM_NORTH_PA, FT_PIN_RESET, 0, 3, 3, true, false);
		delay_ms(10);
		ft_gpio(FT_TLMM_NORTH_PA, FT_PIN_RESET, 0, 3, 3, true, true);
		delay_ms(50);
	}
#endif
	shim_log_pm8150_ldos("powered");
	int rerr = ft_wait_ready(500);
	log_printf("fts test: ready err=%d idle sda=%u scl=%u\n", rerr,
	    ft_sda_read(), ft_scl_read());
	if (rerr) {
		ft_power_off();
		return;
	}

	int serr = ft_i2c_read(sysinfo, sizeof(sysinfo), si, sizeof(si));
	log_printf("fts test: sysinfo err=%d %x %x %x %x %x %x %x %x\n", serr,
	    si[0], si[1], si[2], si[3], si[4], si[5], si[6], si[7]);
	log_printf("fts test: sysinfo afe cfg=%x cx=%x panel=%x cfgVer=%x cxVer=%x fw=%x\n",
	    si[28], si[29], si[30], si[22] | (si[23] << 8),
	    si[26] | (si[27] << 8), si[16] | (si[17] << 8));

#if SHIM_FTS_RESET2 > 0
	/*
	 * XNU 0125's known-good warm order: reset again right after the 0xa6
	 * sysinfo read, before any sense command.  Do not put a sense cycle
	 * between the read and this reset; that is the TWRPSEQ order, which
	 * still ghosts.
	 */
	for (unsigned i = 0; i < SHIM_FTS_RESET2; ++i) {
		ft_gpio(FT_TLMM_NORTH_PA, FT_PIN_RESET, 0, 3, 3, true, false);
		delay_ms(10);
		ft_gpio(FT_TLMM_NORTH_PA, FT_PIN_RESET, 0, 3, 3, true, true);
		delay_ms(50);
		int prerr = ft_wait_ready(500);
		log_printf("fts test: post-sysinfo reset %u ready err=%d\n",
		    i + 1, prerr);
		if (prerr) {
			ft_power_off();
			return;
		}
	}
#endif

#ifdef SHIM_FTS_TWRPSEQ
	/*
	 * TWRP's boot sequence (dmesg 1.135-1.850 s): after the probe resets it
	 * starts scanning, stops it immediately, waits 50 ms and starts again;
	 * the DRM blank later stops that scan and the panel SET_ON path resets
	 * the controller before a third start.  The final start is the one
	 * ft_quiet_scan() sends below.
	 */
	int terr = ft_write_buf(sense_on, sizeof(sense_on));
	log_printf("fts test: twrp seq on1 err=%d\n", terr);
	delay_ms(2);
	terr = ft_write_buf(sense_off, sizeof(sense_off));
	log_printf("fts test: twrp seq off1 err=%d\n", terr);
	delay_ms(50);
	terr = ft_write_buf(sense_on, sizeof(sense_on));
	log_printf("fts test: twrp seq on2 err=%d\n", terr);
	delay_ms(350);
	terr = ft_write_buf(sense_off, sizeof(sense_off));
	log_printf("fts test: twrp seq off2 err=%d\n", terr);
	delay_ms(250);
#ifdef SHIM_FTS_TWRPSEQ_PANEL
	/*
	 * TWRP's DRM blank (sense-off above) then a 249 ms gap, then the
	 * unblank: panel rails drop/return and SET_ON runs.  The controller
	 * reset that follows is TWRP's SET_ON final system reset.
	 */
	log_printf("fts test: twrp seq panel reinit\n");
	panel_reinit();
#endif
	ft_gpio(FT_TLMM_NORTH_PA, FT_PIN_RESET, 0, 3, 3, true, false);
	delay_ms(10);
	ft_gpio(FT_TLMM_NORTH_PA, FT_PIN_RESET, 0, 3, 3, true, true);
	delay_ms(50);
	terr = ft_wait_ready(500);
	log_printf("fts test: twrp seq reset ready err=%d\n", terr);
	if (terr) {
		ft_power_off();
		return;
	}
#endif

#if SHIM_FTS_BLANKCMD
	/*
	 * TWRP's DRM-blank sequence (fts.c fts_mode_handler, FOD branch) and
	 * then its unblank reset.  Order in TWRP: A2 03 20 00 00 01,
	 * A0 01 (low-power scan mode), C0 02 00 00 (single tap only), then
	 * 267 ms later a system reset and the final sense-on.
	 */
	{
		static const uint8_t gesture[6] = {
			0xa2u, 0x03u, 0x20u, 0x00u, 0x00u, 0x01u
		};
		static const uint8_t lp[2] = { 0xa0u, 0x01u };
		static const uint8_t single[4] = { 0xc0u, 0x02u, 0x00u, 0x00u };
		int gr = ft_write_buf(gesture, sizeof(gesture));
		int lr = ft_write_buf(lp, sizeof(lp));
		int sr = ft_write_buf(single, sizeof(single));
		log_printf("fts test: blank cmd err=%d/%d/%d, wait %u ms\n",
		    gr, lr, sr, SHIM_FTS_BLANKCMD_MS);
		delay_ms(SHIM_FTS_BLANKCMD_MS);
	}
	ft_gpio(FT_TLMM_NORTH_PA, FT_PIN_RESET, 0, 3, 3, true, false);
	delay_ms(10);
	ft_gpio(FT_TLMM_NORTH_PA, FT_PIN_RESET, 0, 3, 3, true, true);
	delay_ms(50);
	{
		int brerr = ft_wait_ready(500);
		log_printf("fts test: blank cmd reset ready err=%d\n", brerr);
		if (brerr) {
			ft_power_off();
			return;
		}
	}
#endif

#ifdef SHIM_FTS_QUIET
	(void)sense_on;
	(void)sense_off;
	(void)ev;
	ft_quiet_scan();
#if SHIM_FTS_RETRY_MS > 0
	/*
	 * Second complete bring-up in the same boot.  If run 1 ghosts and run 2
	 * is clean, the first power-up leaves the controller misframed; if both
	 * ghost, the cause is outside the controller.
	 */
	ft_power_off();
	log_printf("fts test: retry drain %u ms\n", SHIM_FTS_RETRY_MS);
	delay_ms(SHIM_FTS_RETRY_MS);
	ft_gpio(FT_TLMM_NORTH_PA, FT_PIN_RESET, 0, 3, 3, true, false);
	ft_gpio(FT_TLMM_NORTH_PA, FT_PIN_VDDIO, 0, 0, 0, true, true);
	delay_ms(50);
	int rverr = shim_rpmh_vreg("ldoc1", 1800, 1);
	delay_ms(200);
	log_printf("fts test: retry rails vddio=1 ldoc1 err=%d\n", rverr);
	ft_l1_log("powered2");
	ft_gpio(FT_TLMM_NORTH_PA, FT_PIN_RESET, 0, 3, 3, true, false);
	delay_ms(10);
	ft_gpio(FT_TLMM_NORTH_PA, FT_PIN_RESET, 0, 3, 3, true, true);
	delay_ms(50);
	int rready = ft_wait_ready(500);
	log_printf("fts test: retry ready err=%d\n", rready);
	if (!rready) {
		int serr = ft_i2c_read(sysinfo, sizeof(sysinfo), si,
		    FT_SYSINFO_SIZE);
		log_printf("fts test: retry sysinfo err=%d sig=%x/%x\n", serr,
		    si[0], si[1]);
		ft_quiet_scan();
	}
	ft_power_off();
#endif
#else
	int err = ft_write_buf(sense_on, sizeof(sense_on));
	log_printf("fts test: sense on err=%d\n", err);

	unsigned logged = 0;
#if SHIM_FTS_BLDIM_MS > 0
	int bl_restored = 0;
#endif
#if SHIM_FTS_SOFT_RECOVER
	unsigned recover_at = 0;
	int recover_armed = 0, recover_done = 0;
#endif
	uint64_t t0 = ticks_now();
	uint64_t hz = rsr("cntfrq_el0");
	for (;;) {
		uint64_t dt = (ticks_now() - t0) * 1000u / hz;
		if (dt >= FT_POLL_MS) {
			break;
		}
#if SHIM_FTS_BLDIM_MS > 0
		if (!bl_restored && dt >= SHIM_FTS_BLDIM_MS) {
			bl_restored = 1;
			panel_set_backlight(SHIM_FTS_BLLEVEL);
			log_printf("fts test: backlight restored at %u ms\n",
			    (unsigned)dt);
			logged = 0;
		}
#endif
		if (!ft_fifo_read(ev) && ev[0] != 0x00u) {
			if (logged < FT_LOG_MAX) {
				log_printf("fts test: ev t=%u ", (unsigned)dt);
				ft_log_ev("", ev);
				++logged;
#if SHIM_FTS_EVDELAY_MS > 0
				delay_ms(SHIM_FTS_EVDELAY_MS);
#endif
			}
#if SHIM_FTS_SOFT_RECOVER
			/* Arm once on the first force-cal/MS-invalid status. */
			if (!recover_armed && ev[0] == 0x43u && ev[1] == 0x05u &&
			    ev[2] == 0x30u) {
				recover_armed = 1;
				recover_at = (unsigned)dt + 600u;
				log_printf("fts test: soft recover armed t=%u\n",
				    (unsigned)dt);
			}
#endif
		}
#if SHIM_FTS_SOFT_RECOVER
		/*
		 * Let the water-mode pair (43 06 01/00) play out, then try to
		 * re-arm the chip from software: sense off, 50 ms, sense on.
		 * A 43 05 01 after this means the controller is sensing again.
		 */
		if (recover_armed && !recover_done && (unsigned)dt >= recover_at) {
			int r;

			recover_done = 1;
			r = ft_write_buf(sense_off, sizeof(sense_off));
			log_printf("fts test: soft recover sense off err=%d\n", r);
			delay_ms(50);
			r = ft_write_buf(sense_on, sizeof(sense_on));
			log_printf("fts test: soft recover sense on err=%d\n", r);
			/* Log post-recovery touches even if the burst used the cap. */
			logged = 0;
		}
#endif
		delay_ms(5);
	}
	log_printf("fts test: done, %u events, %u ms\n", logged, FT_POLL_MS);
	err = ft_write_buf(sense_off, sizeof(sense_off));
	log_printf("fts test: sense off err=%d\n", err);
	ft_power_off();
#endif
}

/*
 * Second bring-up after the display console is up.  A person can see the
 * screen for the whole poll window; the conditioned second power-up should
 * report real touches.  Same rails/reset order as the first fts_test.
 */
void
fts_touch_window(void)
{
#if SHIM_FTS_LATE_TOUCH_MS > 0
	static const uint8_t sense_on[3] = { 0xa0u, 0x00u, 0x01u };
	static const uint8_t sense_off[3] = { 0xa0u, 0x00u, 0x00u };
	static const uint8_t sysinfo[3] = { 0xa6u, 0x00u, 0x00u };
	static uint8_t si[FT_SYSINFO_SIZE];
	uint8_t ev[FT_EVENT_SIZE];
	unsigned edges = 0, logged = 0, total = 0, readerrs = 0;
	uint64_t hz = rsr("cntfrq_el0"), t0, beat = 10000;

	log_printf("fts test: late window %u ms\n", SHIM_FTS_LATE_TOUCH_MS);
	ft_gpio(FT_TLMM_NORTH_PA, FT_PIN_RESET, 0, 3, 3, true, false);
	ft_gpio(FT_TLMM_NORTH_PA, FT_PIN_VDDIO, 0, 0, 0, true, true);
	delay_ms(50);
	int verr = shim_rpmh_vreg("ldoc1", 1800, 1);
	delay_ms(SHIM_FTS_POWERUP_MS);
	log_printf("fts test: late rails vddio=1 ldoc1 err=%d\n", verr);
	ft_l1_log("late");
	ft_gpio(FT_TLMM_NORTH_PA, FT_PIN_RESET, 0, 3, 3, true, false);
	delay_ms(10);
	ft_gpio(FT_TLMM_NORTH_PA, FT_PIN_RESET, 0, 3, 3, true, true);
	delay_ms(50);
	ft_gpio(FT_TLMM_SOUTH_PA, FT_PIN_SDA, 0, 3, 0, false, false);
	ft_gpio(FT_TLMM_SOUTH_PA, FT_PIN_SCL, 0, 3, 0, false, false);
	ft_gpio(FT_TLMM_SOUTH_PA, FT_PIN_IRQ, 0, 3, 3, false, false);
	if (!ft_recover()) {
		log_printf("fts test: late scl stuck low\n");
		ft_power_off();
		return;
	}
	int rerr = ft_wait_ready(500);
	log_printf("fts test: late ready err=%d\n", rerr);
	if (rerr) {
		ft_power_off();
		return;
	}
	int serr = ft_i2c_read(sysinfo, sizeof(sysinfo), si, FT_SYSINFO_SIZE);
	log_printf("fts test: late sysinfo err=%d sig=%x/%x\n", serr,
	    si[0], si[1]);

#if SHIM_FTS_RESET2 > 0
	/* XNU's known-good order: reset again right after the 0xa6 read. */
	for (unsigned i = 0; i < SHIM_FTS_RESET2; ++i) {
		ft_gpio(FT_TLMM_NORTH_PA, FT_PIN_RESET, 0, 3, 3, true, false);
		delay_ms(10);
		ft_gpio(FT_TLMM_NORTH_PA, FT_PIN_RESET, 0, 3, 3, true, true);
		delay_ms(50);
		int rr = ft_wait_ready(500);
		log_printf("fts test: late post-sysinfo reset %u ready err=%d\n",
		    i + 1, rr);
		if (rr) {
			ft_power_off();
			return;
		}
	}
#endif

#ifdef SHIM_FTS_QUIET
	ft_irq_arm();
#endif
#if SHIM_FTS_NOISEPARAM
	{
		uint8_t noise[4] = { 0, 0, 0, 0 };
		int nerr = ft_noise_read(noise);

		log_printf("fts test: late noise read err=%d %02x %02x %02x %02x\n",
		    nerr, noise[0], noise[1], noise[2], noise[3]);
		/* Android's resume resets between the read and the write. */
		ft_gpio(FT_TLMM_NORTH_PA, FT_PIN_RESET, 0, 3, 3, true, false);
		delay_ms(10);
		ft_gpio(FT_TLMM_NORTH_PA, FT_PIN_RESET, 0, 3, 3, true, true);
		delay_ms(50);
		int rerr2 = ft_wait_ready(500);

		log_printf("fts test: late noise reset ready=%d\n", rerr2);
		if (!nerr && !rerr2) {
			int werr = ft_noise_write(noise);

			log_printf("fts test: late noise write err=%d\n", werr);
		}
	}
#endif
#if SHIM_FTS_TWRPFULL
	/*
	 * TWRP's full probe -> DRM blank -> unblank order, in one sequence:
	 *   sense-on, 2 ms, sense-off, 50 ms, sense-on (fts_init_sensing's
	 *   CONFIG_FTS_FOD_AREA_REPORT block), then 300 ms later the DRM blank
	 *   stops the scan and sends gesture + low-power + single-tap, 267 ms,
	 *   then the unblank's panel reinit, system reset and final sense-on
	 *   (the caller's sense_on below).
	 * fts-midpanel replayed this without the three blank commands and
	 * fts-blankcmd ran them before the first sense-on; neither is TWRP's
	 * order.
	 */
	{
		static const uint8_t gesture[6] = {
			0xa2u, 0x03u, 0x20u, 0x00u, 0x00u, 0x01u
		};
		static const uint8_t lp[2] = { 0xa0u, 0x01u };
		static const uint8_t single[4] = { 0xc0u, 0x02u, 0x00u, 0x00u };
		int e1, e2, e3, e4, e5, e6, e7;

		e1 = ft_write_buf(sense_on, sizeof(sense_on));
		delay_ms(2);
		e2 = ft_write_buf(sense_off, sizeof(sense_off));
		delay_ms(50);
		e3 = ft_write_buf(sense_on, sizeof(sense_on));
		delay_ms(SHIM_FTS_TWRPFULL_ON_MS);
		e4 = ft_write_buf(sense_off, sizeof(sense_off));
		e5 = ft_write_buf(gesture, sizeof(gesture));
		e6 = ft_write_buf(lp, sizeof(lp));
		e7 = ft_write_buf(single, sizeof(single));
		log_printf("fts test: twrpfull on/off/on %d/%d/%d blank %d/%d/%d/%d\n",
		    e1, e2, e3, e4, e5, e6, e7);
		delay_ms(SHIM_FTS_TWRPFULL_OFF_MS);
#if SHIM_FTS_TWRPFULL_PANEL
		log_printf("fts test: twrpfull panel reinit\n");
		panel_reinit();
#endif
		ft_gpio(FT_TLMM_NORTH_PA, FT_PIN_RESET, 0, 3, 3, true, false);
		delay_ms(10);
		ft_gpio(FT_TLMM_NORTH_PA, FT_PIN_RESET, 0, 3, 3, true, true);
		delay_ms(50);
		int trerr = ft_wait_ready(500);
		log_printf("fts test: twrpfull reset ready err=%d\n", trerr);
		if (trerr) {
			ft_power_off();
			return;
		}
	}
#endif
#if SHIM_FTS_IRQDRIVEN
	ft_irq_arm();
	log_printf("fts test: late irq arm cfg=%x st=%x io=%x\n",
	    rd32(FT_TLMM_SOUTH_PA + FT_GPIO_INTR_CFG(FT_PIN_IRQ)),
	    ft_irq_status(), ft_irq_level());
#endif
	int e = ft_write_buf(sense_on, sizeof(sense_on));
	log_printf("fts test: late sense on err=%d\n", e);
#if SHIM_FTS_CALQUIET_MS > 0
	log_printf("fts test: late calibration quiet %u ms\n",
	    SHIM_FTS_CALQUIET_MS);
	delay_ms(SHIM_FTS_CALQUIET_MS);
#endif
	t0 = ticks_now();
#if SHIM_FTS_LATE_RESET_MS > 0
	int reset_done = 0;
#endif
	for (;;) {
		uint64_t dt = (ticks_now() - t0) * 1000u / hz;
		if (dt >= SHIM_FTS_LATE_TOUCH_MS)
			break;
#if SHIM_FTS_LATE_RESET_MS > 0
		if (!reset_done && dt >= SHIM_FTS_LATE_RESET_MS) {
			int rr, sr, se;

			reset_done = 1;
			log_printf("fts test: late rescan reset t=%u\n",
			    (unsigned)dt);
			ft_gpio(FT_TLMM_NORTH_PA, FT_PIN_RESET, 0, 3, 3,
			    true, false);
			delay_ms(10);
			ft_gpio(FT_TLMM_NORTH_PA, FT_PIN_RESET, 0, 3, 3,
			    true, true);
			delay_ms(50);
			rr = ft_wait_ready(500);
			log_printf("fts test: late rescan ready err=%d\n", rr);
			if (!rr) {
				sr = ft_i2c_read(sysinfo, sizeof(sysinfo), si,
				    FT_SYSINFO_SIZE);
				log_printf("fts test: late rescan sysinfo err=%d "
				    "sig=%x/%x\n", sr, si[0], si[1]);
				se = ft_write_buf(sense_on, sizeof(sense_on));
				log_printf("fts test: late rescan sense on "
				    "err=%d\n", se);
			}
			/* Count post-reset touch separately from the burst. */
			logged = 0;
		}
#endif
		if (dt >= beat) {
			log_printf("fts test: late beat t=%u irqs=%u ev=%u "
			    "io=%u err=%u\n", (unsigned)dt, edges, total,
			    ft_irq_level(), readerrs);
			beat += 10000;
		}
#if SHIM_FTS_IRQDRIVEN
		{
			bool have = (ft_irq_status() & 1u) != 0;

			if (have)
				ft_irq_clear();
			else if (!ft_irq_level())
				have = true;	/* line still asserted */
			if (!have) {
				delay_ms(1);
				continue;
			}
			++edges;
		}
#elif defined(SHIM_FTS_QUIET)
		if (ft_irq_status() & 1u) {
			++edges;
			ft_irq_clear();
		}
#endif
		for (;;) {
			int r = ft_fifo_read(ev);

			if (r) {
				if (readerrs < 8u) {
					log_printf("fts test: late fifo err t=%u "
					    "err=%d\n", (unsigned)dt, r);
				}
				++readerrs;
				break;
			}
			if (ev[0] == 0x00u)
				break;
			if (logged < FT_LOG_MAX) {
				log_printf("fts test: late ev t=%u ",
				    (unsigned)dt);
				ft_log_ev("", ev);
				++logged;
			}
			++total;
		}
#if SHIM_FTS_IRQDRIVEN
		delay_ms(1);
#else
		delay_ms(5);
#endif
	}
	log_printf("fts test: late done %u events, %u irqs, %u ms\n",
	    total, edges, SHIM_FTS_LATE_TOUCH_MS);
	if (readerrs)
		log_printf("fts test: late fifo errors %u\n", readerrs);
	e = ft_write_buf(sense_off, sizeof(sense_off));
	log_printf("fts test: late sense off err=%d\n", e);
	ft_power_off();
#endif
}
