#pragma once

/* Replay the live DTB's EA8076 f1mp panel init sequence (SHIM_PANEL_INIT). */
void panel_reinit(void);

/* DCS 0x51 panel brightness (2..0x7ff, 0 = off); does not touch the DSI setup. */
void panel_set_backlight(unsigned level);
