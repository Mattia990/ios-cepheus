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
 */
#include <stdbool.h>
#include <stdint.h>

#include "ftstest.h"
#include "log.h"
#include "rpmh.h"
#include "util.h"

#define FT_TLMM_SOUTH_PA 0x03d00000u	/* SDA 55 / SCL 56 / IRQ 122 */
#define FT_TLMM_NORTH_PA 0x03900000u	/* reset 12, VDDIO enable 84 */
#define FT_GPIO_CFG(n)  ((uint32_t)(n) * 0x1000u)
#define FT_GPIO_IO(n)   (FT_GPIO_CFG(n) + 4u)

#define FT_PIN_SDA	55
#define FT_PIN_SCL	56
#define FT_PIN_IRQ	122
#define FT_PIN_RESET	12
#define FT_PIN_VDDIO	84

#define FT_ADDR		0x49u
#define FT_SYSINFO_SIZE	208u
#define FT_EVENT_SIZE	8u
#define FT_RX_MAX	256u

#define FT_SETUP_US	4u
#define FT_LOW_US	2u
#define FT_HIGH_US	3u

#define FT_POLL_MS	6000u		/* how long to watch the first scan */

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

/* Leave the rails as ABL did (both off, reset asserted). */
static void
ft_power_off(void)
{
	ft_gpio(FT_TLMM_NORTH_PA, FT_PIN_RESET, 0, 3, 3, true, false);
	(void)shim_rpmh_vreg("ldoc1", 1800, 0);
	ft_gpio(FT_TLMM_NORTH_PA, FT_PIN_VDDIO, 0, 0, 0, true, false);
}

void
fts_test(void)
{
	static uint8_t si[FT_SYSINFO_SIZE];
	uint8_t ev[FT_EVENT_SIZE];
	uint8_t sense_on[3] = { 0xa0u, 0x00u, 0x01u };
	uint8_t sense_off[3] = { 0xa0u, 0x00u, 0x00u };
	uint8_t sysinfo[3] = { 0xa6u, 0x00u, 0x00u };

	/* Reset first, then VDDIO (bus), then AVDD, as in the driver. */
	ft_gpio(FT_TLMM_NORTH_PA, FT_PIN_RESET, 0, 3, 3, true, false);
	ft_gpio(FT_TLMM_NORTH_PA, FT_PIN_VDDIO, 0, 0, 0, true, true);
	delay_ms(50);
	int verr = shim_rpmh_vreg("ldoc1", 1800, 1);
	delay_ms(200);
	log_printf("fts test: rails vddio=1 ldoc1 err=%d\n", verr);

	/* SDA/SCL open-drain inputs with pull-up; IRQ input. */
	ft_gpio(FT_TLMM_SOUTH_PA, FT_PIN_SDA, 0, 3, 0, false, false);
	ft_gpio(FT_TLMM_SOUTH_PA, FT_PIN_SCL, 0, 3, 0, false, false);
	ft_gpio(FT_TLMM_SOUTH_PA, FT_PIN_IRQ, 0, 3, 3, false, false);
	if (!ft_recover()) {
		log_printf("fts test: scl stuck low after power-up\n");
		ft_power_off();
		return;
	}

	/* Reset low 10 ms, high 50 ms, then wait for ready. */
	ft_gpio(FT_TLMM_NORTH_PA, FT_PIN_RESET, 0, 3, 3, true, false);
	delay_ms(10);
	ft_gpio(FT_TLMM_NORTH_PA, FT_PIN_RESET, 0, 3, 3, true, true);
	delay_ms(50);
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

	int err = ft_write_buf(sense_on, sizeof(sense_on));
	log_printf("fts test: sense on err=%d\n", err);

	unsigned logged = 0;
	uint64_t t0 = ticks_now();
	uint64_t hz = rsr("cntfrq_el0");
	for (;;) {
		uint64_t dt = (ticks_now() - t0) * 1000u / hz;
		if (dt >= FT_POLL_MS) {
			break;
		}
		if (!ft_fifo_read(ev) && ev[0] != 0x00u) {
			if (logged < 64u) {
				log_printf("fts test: ev t=%u ", (unsigned)dt);
				ft_log_ev("", ev);
				++logged;
			}
		}
		delay_ms(5);
	}
	log_printf("fts test: done, %u events, %u ms\n", logged, FT_POLL_MS);
	err = ft_write_buf(sense_off, sizeof(sense_off));
	log_printf("fts test: sense off err=%d\n", err);
	ft_power_off();
}
