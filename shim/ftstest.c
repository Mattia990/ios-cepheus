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

#ifdef SHIM_FTS_QUIET
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

static void
ft_l1_log(const char *tag)
{
	uint8_t en = 0, set = 0, mode = 0;
	int re = spmi_read(5, 0x4046, &en);
	int rs = spmi_read(5, 0x4041, &set);
	int rm = spmi_read(5, 0x4045, &mode);

	log_printf("fts test: %s pm8150l l1 46=%x%s 41=%x%s 45=%x%s\n", tag,
	    en, re ? "!" : "", set, rs ? "!" : "", mode, rm ? "!" : "");
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

#if SHIM_FTS_PREPROBE
	ft_preprobe();
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
	delay_ms(200);
	log_printf("fts test: rails vddio=1 ldoc1 err=%d\n", verr);
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

#ifdef SHIM_FTS_QUIET
	(void)sense_on;
	(void)sense_off;
	(void)ev;
	ft_quiet_scan();
	ft_power_off();
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
			if (logged < 64u) {
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
