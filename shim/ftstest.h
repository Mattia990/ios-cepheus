#pragma once

/* Pre-XNU FTS bring-up and scan (-DSHIM_FTS_TEST). */
void fts_test(void);

/* Second bring-up after the display is up, poll for -DSHIM_FTS_LATE_TOUCH_MS. */
void fts_touch_window(void);

/* PM8150 LDO1..18 summary (shim.c): entry rail state and post-power-up. */
void shim_log_pm8150_ldos(const char *tag);
