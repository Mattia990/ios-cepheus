/*
 * cepheus (Xiaomi Mi 9, SM8150) boot shim.
 *
 * ABL enters here at EL1 with the MMU and caches off and x0 = DTB. With an
 * XNU payload in the ramdisk (tools/mkpayload.py) the shim boots it;
 * without one it runs the hardware probe: everything it learns goes into
 * a text log that is written to the scratch area at the end of userdata,
 * and the phone is then reset into fastboot, so a run can be read back
 * from TWRP without anyone watching the screen.
 */
#include <stdint.h>
#include "console.h"
#include "fdt.h"
#include "loader.h"
#include "ramlog.h"
#include "log.h"
#include "sde.h"
#include "panel.h"
#include "showlog.h"
#include "diag.h"
#include "disptest.h"
#include "gpuprobe.h"
#include "smmuprobe.h"
#include "spmi.h"
#include "ufs.h"
#include "util.h"

#define IMEM_RESTART_REASON	0x146bf65cUL	/* qcom,msm-imem restart_reason@65c */
#define RESTART_BOOTLOADER	0x77665500U
#define PON_SOFT_RB_SPARE	0x88f		/* PM8150 (SID 0) PON, reason in [7:1] */
#define PON_REASON_BOOTLOADER	0x02
#define PON_PS_HOLD_RESET_CTL	0x85a		/* [3:0] type */
#define PON_PS_HOLD_RESET_CTL2	0x85b		/* [7] enable */
#define PON_RESET_EN		0x80
#define PON_HARD_RESET		0x7
#define WDT_BASE		0x17c10000UL	/* qcom,wdt@17c10000 */
#define WDT_HZ			32765

#define GICD_BASE		0x17a00000UL
#define GICR_BASE		0x17a60000UL

#define PSCI_VERSION		0x84000000UL
#define PSCI_CPU_ON		0xc4000003UL
#define PSCI_SYSTEM_RESET	0x84000009UL
#define PSCI_FEATURES		0x8400000aUL
#define PSCI_SYSTEM_SUSPEND	0xc400000eUL
#define PSCI_SYSTEM_RESET2	0xc4000012UL
#define SMCCC_VERSION		0x80000000UL

static int ufs_ok;
static volatile int in_exception;

/* Header line, then the text; zero-padded to whole blocks. */
static void save_log(const char *why)
{
	if (!ufs_ok)
		return;
	log_printf("--- saved (%s) at %lu ms\n", why, ticks_now() / 19200);
	uint32_t blocks = (log_len + 4095) / 4096;
	if (blocks > UFS_SCRATCH_BLOCKS)
		blocks = UFS_SCRATCH_BLOCKS;
	for (uint32_t i = 0; i < blocks; i += 16) {
		uint32_t n = blocks - i < 16 ? blocks - i : 16;
		int rc = ufs_scratch_write(i, n, log_buf + i * 4096);
		if (rc) {
			log_printf("save: write rc %d\n", rc);
			ufs_ok = 0;
			return;
		}
	}
}

/* Next boot goes to fastboot: PMIC PON reason (what ABL reads) plus the IMEM cookie. */
static void set_reboot_bootloader(void)
{
	uint8_t v = 0xff, after = 0xff;
	int rc = spmi_read(0, PON_SOFT_RB_SPARE, &v);
	if (!rc)
		rc = spmi_write(0, PON_SOFT_RB_SPARE, (v & 1) | PON_REASON_BOOTLOADER << 1);
	spmi_read(0, PON_SOFT_RB_SPARE, &after);
	wr32(IMEM_RESTART_REASON, RESTART_BOOTLOADER);
	log_printf("reboot reason: pon spare %x -> %x (rc %d), imem %x\n", v, after, rc,
		   rd32(IMEM_RESTART_REASON));
}

/*
 * Make PS_HOLD resets (PSCI SYSTEM_RESET, and TZ after a watchdog bite)
 * PMIC hard resets, as Linux does on reboot (qcom,system-reset): ABL leaves
 * WARM_RESET, and state surviving warm resets once left TWRP hanging until
 * a long key press. SOFT_RB_SPARE survives a hard reset. Sequence from
 * qpnp-power-on: disable, wait out 10 sleep-clock cycles, set type, enable.
 */
static void set_hard_reset(void)
{
	uint8_t ctl = 0xff, ctl2 = 0xff, after = 0xff, after2 = 0xff;
	int rc = spmi_read(0, PON_PS_HOLD_RESET_CTL, &ctl);
	rc |= spmi_read(0, PON_PS_HOLD_RESET_CTL2, &ctl2);
	if (!rc) {
		rc |= spmi_write(0, PON_PS_HOLD_RESET_CTL2, ctl2 & ~PON_RESET_EN);
		delay_ms(1);
		rc |= spmi_write(0, PON_PS_HOLD_RESET_CTL, (ctl & ~0xf) | PON_HARD_RESET);
		rc |= spmi_write(0, PON_PS_HOLD_RESET_CTL2, ctl2 | PON_RESET_EN);
	}
	spmi_read(0, PON_PS_HOLD_RESET_CTL, &after);
	spmi_read(0, PON_PS_HOLD_RESET_CTL2, &after2);
	log_printf("ps_hold reset: ctl %x -> %x, ctl2 %x -> %x (rc %d)\n", ctl, after, ctl2, after2,
		   rc);
}

static void __attribute__((noreturn)) reset(void)
{
	__asm__ volatile("dsb sy" ::: "memory");
	smc_call(PSCI_SYSTEM_RESET, 0, 0, 0);
	wr32(0x0c264000UL, 0);	/* PS_HOLD low */
	for (;;)
		__asm__ volatile("wfe");
}

static void wdt_off(void)
{
	log_printf("wdt: en %x sts %x bark %x bite %x -> off\n", rd32(WDT_BASE + 0x08),
		   rd32(WDT_BASE + 0x0c), rd32(WDT_BASE + 0x10), rd32(WDT_BASE + 0x14));
	wr32(WDT_BASE + 0x08, 0);
}

static void wdt_arm(unsigned bite_s)
{
	log_printf("wdt: en %x sts %x bark %x bite %x\n", rd32(WDT_BASE + 0x08),
		   rd32(WDT_BASE + 0x0c), rd32(WDT_BASE + 0x10), rd32(WDT_BASE + 0x14));
	wr32(WDT_BASE + 0x08, 0);
	wr32(WDT_BASE + 0x10, (bite_s - 2) * WDT_HZ);	/* bark: nobody listens */
	wr32(WDT_BASE + 0x14, bite_s * WDT_HZ);
	wr32(WDT_BASE + 0x04, 1);
	wr32(WDT_BASE + 0x08, 1);
	log_printf("wdt: armed, bite in %u s (en %x)\n", bite_s, rd32(WDT_BASE + 0x08));
}

void exc_handler(uint64_t slot, uint64_t *regs)
{
	if (in_exception++)
		reset();
	log_printf("\n*** EXCEPTION slot %lu esr %lx elr %lx far %lx spsr %lx x0 %lx x1 %lx lr %lx\n",
		   slot, rsr("esr_el1"), rsr("elr_el1"), rsr("far_el1"), rsr("spsr_el1"),
		   regs[0], regs[1], regs[31]);
	save_log("exception");
	reset();
}

void secondary_main(uint64_t idx)
{
	(void)idx;
	for (;;)
		__asm__ volatile("wfe");
}

static void log_cpu(uintptr_t dtb)
{
	log_printf("cpu: el %lu midr %lx mpidr %lx\n", rsr("currentel") >> 2, rsr("midr_el1"),
		   rsr("mpidr_el1"));
	log_printf("cpu: mmfr0 %lx mmfr1 %lx mmfr2 %lx\n", rsr("id_aa64mmfr0_el1"),
		   rsr("id_aa64mmfr1_el1"), rsr("id_aa64mmfr2_el1"));
	log_printf("cpu: pfr0 %lx isar0 %lx isar1 %lx\n", rsr("id_aa64pfr0_el1"),
		   rsr("id_aa64isar0_el1"), rsr("id_aa64isar1_el1"));
	log_printf("cpu: sctlr %lx ctr %lx cntfrq %lu vbar %lx\n", rsr("sctlr_el1"),
		   rsr("ctr_el0"), rsr("cntfrq_el0"), rsr("vbar_el1"));
	uint64_t lo = 0, hi = 0;
	fdt_chosen_u64(dtb, "linux,initrd-start", &lo);
	fdt_chosen_u64(dtb, "linux,initrd-end", &hi);
	log_printf("boot: dtb %lx magic %x initrd %lx-%lx shim %p\n", (uint64_t)dtb,
		   dtb ? __builtin_bswap32(rd32(dtb)) : 0, lo, hi, (void *)log_cpu);
	log_printf("psci: version %lx smccc %lx\n", smc_call(PSCI_VERSION, 0, 0, 0),
		   smc_call(SMCCC_VERSION, 0, 0, 0));
	/* No pointer tables: the image runs unrelocated wherever ABL put it. */
	log_printf("psci: features cpu_on %lx reset %lx reset2 %lx suspend %lx\n",
		   smc_call(PSCI_FEATURES, PSCI_CPU_ON, 0, 0),
		   smc_call(PSCI_FEATURES, PSCI_SYSTEM_RESET, 0, 0),
		   smc_call(PSCI_FEATURES, PSCI_SYSTEM_RESET2, 0, 0),
		   smc_call(PSCI_FEATURES, PSCI_SYSTEM_SUSPEND, 0, 0));
}

static void log_pmic(void)
{
	static const uint16_t regs[] = { 0x808, 0x809, 0x80a, 0x80c, 0x810, 0x85a, 0x85b, 0x88c, 0x88f };
	log_printf("pon:");
	for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
		uint8_t v = 0;
		int rc = spmi_read(0, regs[i], &v);
		log_printf(" %x=%x%s", regs[i], v, rc ? "!" : "");
	}
	log_printf("\n");
}

static void log_spmi_regs(uint32_t sid, uint32_t base, const char *name,
			  const uint16_t *regs, unsigned n)
{
	log_printf("%s:", name);
	for (unsigned i = 0; i < n; i++) {
		uint8_t v = 0;
		int rc = spmi_read(sid, base + regs[i], &v);
		log_printf(" %x=%x%s", regs[i], v, rc ? "!" : "");
	}
	log_printf("\n");
}

/*
 * PM8150L LDO1 (touch AVDD, fixed 1.8 V) lives on SPMI SID 5, peripheral
 * 0x40, so its registers are at 0x4000+off.  The RPMh regulator driver
 * controls it through the AOP, but the SPMI observer channel can read the
 * PMIC's actual retained state.  Compare cold and warm boots: 0x40 range,
 * 0x41 set point, 0x45 mode, 0x46 enable, 0x4c soft start.  Subtype 0x71
 * is LDO_510 (hfsmps ops): its output pull-down is 0xa0 bit 7, not 0x48.
 * LDO2 (0x4100) is a control that no touch code touches.
 */
static void log_pm8150l_ldo(uint32_t sid, uint32_t base, const char *name)
{
	static const uint16_t regs[] = {
		0x04, 0x05, 0x08, 0x09, 0x0a, 0x0b,
		0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x48, 0x4c,
		0x53, 0x60, 0x62, 0x64, 0x88, 0xa0,
	};
	log_spmi_regs(sid, base, name, regs, sizeof(regs) / sizeof(regs[0]));
}

/*
 * Touch pins at shim entry, before XNU or its driver touches TLMM.
 *   south TLMM: SDA 55, SCL 56, IRQ 122
 *   north TLMM: reset 12, VDDIO 84 (disp_vddio_vreg enable)
 * Same layout as the XNU driver: cfg [1:0] pull (0 none, 1 down, 2 keeper,
 * 3 up), [5:2] mux, [8:6] drive (mA/2 - 1), [9] output; io bit 1 is the
 * output latch and bit 0 the pad level.
 */
#define TLMM_SOUTH_PA		0x03d00000UL
#define TLMM_NORTH_PA		0x03900000UL
#define TLMM_EAST_PA		0x03500000UL	/* GPIO 21: disp_vci_vreg enable */
#define TLMM_CFG(n)		((uint32_t)(n) * 0x1000u)
#define TLMM_IO(n)		(TLMM_CFG(n) + 4u)

/* N none, D down, K keeper, U up -- avoid string-pointer tables. */
static char pull_char(uint32_t cfg)
{
	uint32_t pull = cfg & 3;
	if (pull == 0)
		return 'N';
	if (pull == 1)
		return 'D';
	if (pull == 2)
		return 'K';
	return 'U';
}

static void log_pin(const char *name, uintptr_t base, unsigned pin)
{
	uint32_t cfg = rd32(base + TLMM_CFG(pin));
	uint32_t io = rd32(base + TLMM_IO(pin));
	log_printf("  %s[%u] cfg %x (pull %c mux %u %umA %s) io %x\n", name, pin,
		   cfg, pull_char(cfg), (cfg >> 2) & 0xf,
		   (((cfg >> 6) & 7) + 1) * 2, (cfg & 0x200) ? "out" : "in", io);
}

static void log_touch_pins(const char *tag)
{
	log_printf("touch tlmm %s:\n", tag);
	log_pin("sda", TLMM_SOUTH_PA, 55);
	log_pin("scl", TLMM_SOUTH_PA, 56);
	log_pin("irq", TLMM_SOUTH_PA, 122);
	log_pin("reset", TLMM_NORTH_PA, 12);
	log_pin("vddio", TLMM_NORTH_PA, 84);
}

/*
 * Panel state at shim entry. ABL owns the panel on both cold and warm boots
 * and XNU never re-initializes it (patch 0079 only tracks the backlight
 * state), so a warm reset that skips ABL's panel init leaves the DDIC in
 * whatever state the previous OS left. Log the pins and rails that decide
 * that state.
 *
 * pinctrl-sm8150: GPIO 6 (panel reset) is on the south tile, GPIO 8 (TE) on
 * the north tile.  VCI/VDDIO are PM8150 LDO13/LDO14 (SID 0, 0x4c00/0x4d00,
 * RPMh-controlled; read-only here).  LAB/IBB are the PM8150L LCDB block
 * (SID 5, 0xec00): 0x46 enable, 0x45 module ready, 0x08/0x0a/0x0b/0x10
 * status, 0x71/0x81 LDO/NCP setpoints, 0x77/0x87 pull-downs, 0x60 touch
 * wake.
 */
static void log_panel_state(int spmi_ok)
{
	static const uint16_t lcdb[] = {
		0x08, 0x0a, 0x0b, 0x10, 0x40, 0x41, 0x45, 0x46, 0x47,
		0x4b, 0x50, 0x53, 0x60, 0x62, 0x66, 0x71, 0x75, 0x77,
		0x79, 0x7b, 0x81, 0x85, 0x87, 0x8b,
	};
	log_printf("panel tlmm entry:\n");
	log_pin("reset", TLMM_SOUTH_PA, 6);
	log_pin("te", TLMM_NORTH_PA, 8);
	log_pin("vci", TLMM_EAST_PA, 21);
	if (!spmi_ok)
		return;
	log_pm8150l_ldo(0, 0x4c00, "pm8150 ldo13 vci");
	log_pm8150l_ldo(0, 0x4d00, "pm8150 ldo14 vddio");
	log_spmi_regs(5, 0xec00, "pm8150l lcdb lab/ibb", lcdb,
		      sizeof(lcdb) / sizeof(lcdb[0]));
	/*
	 * The OLED rails are the other analogue state that survives a warm
	 * reset: the touch sensor sits on the panel, so ELVDD/ELVSS affect its
	 * mutual capacitance.  qpnp-amoled-regulator.c: 0x04 peripheral type
	 * (IBB 0x20, AB 0x24, OLEDB 0x2c), AB_STATUS1 0x08 (VREG_OK bit 6),
	 * AB PD 0x78, IBB PS 0x50 / PD 0x47 / NLIMIT 0x61 / smart PS 0x65.
	 * The block is status = "disabled" in the live DTB, so compare cold
	 * and warm boots before trusting any of these values.
	 */
	static const uint16_t amoled[] = {
		0x04, 0x08, 0x47, 0x50, 0x61, 0x65, 0x72, 0x78,
	};
	log_spmi_regs(5, 0xdc00, "pm8150l amoled ibb", amoled,
		      sizeof(amoled) / sizeof(amoled[0]));
	log_spmi_regs(5, 0xde00, "pm8150l amoled ab", amoled,
		      sizeof(amoled) / sizeof(amoled[0]));
	log_spmi_regs(5, 0xe000, "pm8150l amoled oledb", amoled,
		      sizeof(amoled) / sizeof(amoled[0]));
}

/*
 * Diagnostic for the warm-boot touch failure: hold the display panel in reset
 * across the jump to XNU.  ABL leaves the panel initialized on both cold and
 * warm boots and XNU never re-initializes it, so this isolates whether the
 * display DDIC/sensor environment is what breaks the FTS mutual-strength
 * calibration.  The screen stays dark; read the XNU log from TWRP afterwards.
 */
static void panel_hold_reset(void)
{
	uint32_t io = rd32(TLMM_SOUTH_PA + TLMM_IO(6));
	log_printf("panel: hold reset, cfg %x io %x\n",
		   rd32(TLMM_SOUTH_PA + TLMM_CFG(6)), io);
	wr32(TLMM_SOUTH_PA + TLMM_IO(6), io & ~2u);
	uint32_t cfg = rd32(TLMM_SOUTH_PA + TLMM_CFG(6)) & ~0x3ffu;
	cfg |= (1u << 6) | 0x200u;	/* mux 0, no pull, 4 mA, output low */
	wr32(TLMM_SOUTH_PA + TLMM_CFG(6), cfg);
	delay_ms(5);
	log_printf("panel: reset held, cfg %x io %x\n",
		   rd32(TLMM_SOUTH_PA + TLMM_CFG(6)),
		   rd32(TLMM_SOUTH_PA + TLMM_IO(6)));
}

static void log_gic(void)
{
	log_printf("gic: ctlr %x typer %x iidr %x\n", rd32(GICD_BASE), rd32(GICD_BASE + 4),
		   rd32(GICD_BASE + 8));
	for (int i = 0; i < 8; i++) {
		uintptr_t r = GICR_BASE + 0x20000UL * i;
		log_printf("gicr%d: typer %x:%x waker %x\n", i, rd32(r + 0xc), rd32(r + 8),
			   rd32(r + 0x14));
	}
}

/* Plain RAM where XNU will live: is it there and writable with the MMU off? */
static void log_ram(void)
{
	static const uint64_t probe[] = { 0x100000000ULL, 0x180000000ULL, 0x1fff00000ULL };
	for (unsigned i = 0; i < sizeof(probe) / sizeof(probe[0]); i++) {
		volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)probe[i];
		uint64_t old = p[0];
		p[0] = 0x464c414d45000000ULL | i;
		uint64_t back = p[0];
		p[0] = old;
		log_printf("ram %lx: was %lx readback %lx\n", probe[i], old, back);
	}
}

static void save_before_jump(void)
{
	save_log("jumping to xnu");
}

/*
 * XNU writes a panic header + text into the ADT "pram" node before it
 * resets (pexpert/arm/pe_init.c check_for_panic_log). The next boot is
 * normally a probe shim, so dump it here: the ring flush may be up to
 * ufslogsecs behind the panic, and this is the only copy of the text.
 * Reads physical memory with the MMU off; magic 0x46554E4B = "FUNK".
 */
#define PRAM_PA		0x1fd000000UL
#define PRAM_SIZE	0x80000UL
#define PRAM_MAGIC	0x46554E4BUL

static void
dump_pram(void)
{
	volatile uint32_t *h = (volatile uint32_t *)PRAM_PA;
	if (h[0] != PRAM_MAGIC) {
		log_printf("pram: no panic header (magic %x)\n", h[0]);
		return;
	}
	uint32_t off = h[4], len = h[5];
	log_printf("pram: panic header v%u flags %x log %x+%x\n", h[2], h[3], off, len);
	if (off < 0x30 || off >= PRAM_SIZE || len == 0 || len > PRAM_SIZE - off) {
		log_printf("pram: bad panic log range\n");
		return;
	}
	if (len > 8192)
		len = 8192;
	char buf[129];
	for (uint32_t pos = 0; pos < len; pos += 128) {
		uint32_t n = len - pos < 128 ? len - pos : 128;
		for (uint32_t i = 0; i < n; i++) {
			char c = *(volatile char *)(PRAM_PA + off + pos + i);
			buf[i] = (c == '\n' || c == '\t' || (c >= 32 && c < 127)) ? c : '.';
		}
		buf[n] = 0;
		log_printf("pram: %s\n", buf);
	}
}

void shim_main(uintptr_t dtb)
{
	log_printf("CEPHEUSSHIM probe v1 t=%lu\n", ticks_now());
	log_cpu(dtb);
	dump_pram();
	log_touch_pins("entry");
	int spmi_ok = !spmi_init();
	log_panel_state(spmi_ok);
	if (spmi_ok) {
		log_pmic();
		log_pm8150l_ldo(5, 0x4000, "pm8150l ldo1");
		log_pm8150l_ldo(5, 0x4100, "pm8150l ldo2");
		set_reboot_bootloader();
		set_hard_reset();
	}
	/* Probe runs reset themselves; only an XNU boot arms the watchdog. */
	wdt_off();
#ifdef SHIM_DIAG
	sde_probe();
	uintptr_t dfb = sde.pipe ? rd32(sde.pipe + 0x14) : 0;
	uint32_t dstride = sde.pipe ? rd32(sde.pipe + 0x24) & 0xffff : 0;
	if (!con_init(dfb, dstride)) {
		sde_autorefresh(1);
		diag_run(spmi_ok);
	}
	reset();
#endif
	ufs_ok = !ufs_init();
	if (ufs_ok)
		ramlog_rescue();
#ifdef SHIM_SHOWLOG
	/* Read-only: the shim log on UFS is the previous run's, keep it. */
	sde_probe();
	uintptr_t sfb = sde.pipe ? rd32(sde.pipe + 0x14) : 0;
	uint32_t sstride = sde.pipe ? rd32(sde.pipe + 0x24) & 0xffff : 0;
	if (!con_init(sfb, sstride)) {
		sde_autorefresh(1);
		showlog_run(ufs_ok);
	}
	reset();
#endif
	save_log("ufs up");
#ifdef SHIM_SMMUPROBE
	wdt_arm(20);
	smmuprobe_run();
	save_log("smmuprobe done");
	reset();
#endif
#ifdef SHIM_GPUPROBE
	wdt_arm(20);	/* a hung GPU register access resets the phone */
	gpuprobe_run(save_log);
	save_log("gpuprobe done");
	reset();
#endif
#ifdef SHIM_DISPTEST
	sde_probe();
	save_log("sde probe");
	disptest_run(save_log);
	save_log("disptest done");
	reset();
#endif

	sde_probe();
	save_log("sde probe");
	sde_sample("before", 8, 20);
	/*
	 * The panel scans out from the VIG0 source address ABL programmed; on
	 * cepheus that is the base of cont_splash_region, 0x9c000000 (flame's
	 * sits 4 MB into its block). Read it instead of hardcoding, and fall
	 * back to the known address if the pipe is missing.
	 */
	uintptr_t fb = sde.pipe ? rd32(sde.pipe + 0x14) : 0;
	uint32_t stride = sde.pipe ? rd32(sde.pipe + 0x24) & 0xffff : 0;
	if (!fb)
		fb = FB_SPLASH_BASE;
	if (!stride)
		stride = FB_W * 4;
	log_printf("fb: base %lx stride %u (%s)\n", (uint64_t)fb, stride,
		   sde.pipe ? "live pipe" : "fallback");
	if (!con_init(fb, stride)) {
		con_clear(0xff203040);
		con_str("CEPHEUS", "shim probe v1");
	}
	sde_autorefresh(1);
	sde_sample("autorefresh", 16, 20);
	save_log("display");

#ifdef SHIM_PANEL_OFF
	panel_hold_reset();
	save_log("panel reset held");
#endif
#ifdef SHIM_PANEL_INIT
	panel_reinit();
	sde_restart();
	sde_sample("after panel", 8, 20);
	save_log("panel reinit");
#endif
	/* An XNU payload in the ramdisk means: boot it. */
	struct loader_fb lfb = { fb, stride, FB_W, FB_H, save_before_jump };
	if (loader_has_payload(dtb) && 0)
		wdt_arm(120);
	const char *why = loader_boot(dtb, &lfb);
	log_printf("loader: %s\n", why);

	log_gic();
	log_ram();
	log_printf("done, resetting\n");
	save_log("done");
	reset();
}
