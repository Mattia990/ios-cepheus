#include <stdint.h>
#include "log.h"
#include "util.h"
#include "loader.h"

void exc_handler(uint64_t slot, uint64_t *regs)
{
	(void)slot;
	(void)regs;
	log_printf("EXCEPTION slot=%lu\n", slot);
	for (;;)
		__asm__ volatile("wfe");
}

void secondary_main(uintptr_t idx)
{
	(void)idx;
	for (;;)
		__asm__ volatile("wfe");
}

void wdt_arm(unsigned bite_s)
{
	(void)bite_s;
	log_printf("wdt: arm %u\n", bite_s);
}

void log_gic(void)
{
	log_printf("gcc: GIC probe\n");
}

void log_ram(void)
{
	log_printf("gcc: RAM probe\n");
}

void save_before_jump(void)
{
	log_printf("save_before_jump\n");
}

void reset(void)
{
	log_printf("reset: rebooting\n");
	for (;;)
		__asm__ volatile("wfe");
}

void log_cpu(void)
{
	log_printf("gcc: CPU probe\n");
}

void save_log(const char *why)
{
	log_printf("save_log: %s\n", why);
}

void shim_main(uintptr_t dtb)
{
	(void)dtb;

	/* Disable watchdog */
	wr32(0x017c10000UL + 0x08, 0);

	log_printf("CEPHEUSSHIM probe v1\n");
	log_cpu();

	log_printf("gcc: booting XNU with DTB\n");

	struct loader_fb lfb = {
		.base = 0x9c400000,
		.stride = 4320,
		.width = 1080,
		.height = 2340,
	};
	if (loader_has_payload(dtb))
		wdt_arm(300);

	log_printf("gcc: booting XNU with cmdline: debug=0xc serial=3\n");
	const char *why = loader_boot(dtb, &lfb);
	log_printf("loader: %s\n", why);

	log_gic();
	log_ram();
	log_printf("done, resetting\n");
	save_log("done");
	reset();
}
