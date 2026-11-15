#pragma once
#include <stdint.h>

/*
 * RPMh VRM write from the shim (SHIM_PANEL_CYCLE).
 *
 * shim_rpmh_probe() maps the RSC, reads the cmd-db locator and resolves the
 * resource without sending anything, so the panel code can skip its power
 * cycle entirely when RPMh is not usable at shim time.
 */
int shim_rpmh_probe(const char *res);

/*
 * One ACTIVE-set request: n 1..4 (offset from the resource address, value)
 * command pairs.  Same transaction as sweet_rpmh_write() in XNU.
 */
int shim_rpmh_write(const char *res, unsigned n, const uint32_t *offsets,
		    const uint32_t *values);

/* PMIC5 LDO on: voltage/mode 7/enable; off: enable=0 (offset 4). */
int shim_rpmh_vreg(const char *res, unsigned mv, int on);
