# Handoff: iOS on Xiaomi Mi 9 (cepheus)

Last updated **2026-11-14** (newest work first under "Start here"). Read this first, then [NOTES.md](NOTES.md)
(hardware facts, test loop, bootloader rules).

## Start here (next session)

### 0139 probe replayed, warm touch still fails; test the sense-on/off/on cycle (2026-11-14)

**0139 warm run (`touch-0139`).** Kernel built with 0139
(`work/boot/kernel-brightness/kernel.development.sweet`, SHA-256 `bfe8dcd0...`), booted
warm from TWRP 3.5.2. The probe matches TWRP exactly: `lockdown sig=a5 id=70 70 cnt=1/2
data=4838320546313200`, `crc reg err=0 val=00`, `crc reset ready=1 events=0 cfg=0 cx=0`,
`sense on err=0 ... probe=1` at 30.443 s. Then the usual failure: ghost 8-finger burst at
34.120-34.199 s, `early status failure` at 34.456 s (+4.0 s), clean power-cycle recovery at
35.082 s, alive through 93.5 s, **no real touch**. Logs: `logs/runs/touch-0139/`.

**Rails-on run (`touch-0139-pwr0`).** Same kernel with `cepheustouchpwr=0` (TWRP resets an
already-powered chip and never power-cycles the rails). Sense-on at 29.922 s, no rail
cycle. Ghost 8-9-finger burst at 33.530-33.615 s (+3.6 s) but **no `43 05 30/31`**; the
driver does not recover (none was requested) and IRQ stays idle through 117 s. No real
touch. Display on at 49.527 s. Logs: `logs/runs/touch-0139-pwr0/`.

**Result.** Neither the lockdown/CRC probe nor the rail power-cycle is the trigger: the
first scan calibrates badly either way. What TWRP still does that we do not is in
`fts_init_sensing` (fts_521, `CONFIG_FTS_FOD_AREA_REPORT`): after the screen-on sense-on
(`a0 00 01`) it immediately sends sense-off (`a0 00 00`), waits `WAIT_AFTER_SENSEOFF`
(50 ms), and sends sense-on again. The first calibration is aborted before it completes.
TWRP's force-cal event then carries sub-status `02`; ours carries `01` (the cold-boot
success also logs `01`, so the byte alone proves nothing).

**Next: 0140.** `xnu/patches/0140-cepheus-fts-sense-cycle.patch` adds `fts_sense_cycle()`
(on, 2 ms, off, 50 ms, on) and uses it at init and recovery; `cepheustouchsensecycle=0`
keeps the old single sense-on, and the sense-on line now ends `cycle=1`. Build on top of
0139, boot warm from TWRP, swipe from ~30 s to ~65 s. If it still fails, the last TWRP
step is the display-notifier: a system reset + sense-on right after the panel's
`DSI_CMD_SET_ON` (TWRP 1.838-1.860 s; XNU display-on was 49.527 s in `touch-0139-pwr0`).

### Panel re-init replay: DSI path works, display stays dark, touch unchanged (2026-11-14)

**Implementation.** `shim/panel.c` replays the live DTB's 34-command EA8076 f1mp on-command
sequence through the DSI0 command FIFO at shim entry (`SHIM_PANEL_INIT`): panel reset GPIO 6
low 1 ms / high 10 ms, then every command from `timing@0` in LP mode, with the FIFO image
format from downstream `dsi_cmd_dma_add()` (`03 00 39 c0` for long writes, `80` last-packet
short writes). `send_image()` mirrors `dsi_ctrl_hw_cmn_reset_cmd_fifo()` +
`kickoff_fifo_command()` (TPG FIFO, embedded mode, SW trigger, poll `CMD_MODE_DMA_DONE`),
resets the FIFO after each command, and restores `COMMAND_MODE_DMA_CTRL` to its entry
value. `sde_restart()` (disptest phase G: autorefresh off, `CTL_FLUSH 0x3|1<<17`, kick,
40 ms, autorefresh on) restarts scanout afterwards.

**Probe-only run.** `work/boot/runs/panel-init-probe/panel-init-probe.img` (no XNU
payload): all 34 commands complete with 0 errors, DSI entry
`hw 20030000 ctrl 1f5 status 5 fifo 1010 int 500 dma 14000000`, exit
`status 0 fifo 11111010` (`logs/runs/panel-init-probe/shim.txt`). The command path is
correct.

**XNU run 1 (`touch-panelinit`).** All 34 commands complete, backlight `0x7ff` sent, DMA
restored. Screen stays dark. FTS: sense-on 30.664 s, `43 05 01` at 30.666 s, then
`43 05 31` at 31.248 s (+0.58 s); recovery power cycle clean at 31.881 s; ghost
six-finger burst at 32.965-33.025 s and `43 05 30` at 33.237 s; second recovery at
33.872 s. No real touch. Logs: `logs/runs/touch-panelinit/`.

**XNU run 2 (`touch-panelinit2`, `sde_restart` + LP backlight).** All 34 commands complete;
tear count advances after the restart (`sde after panel: 30c17/924 df8/5ca ...`), so the
MDP is sending frames again. Screen still dark. FTS: sense-on 30.898 s, `43 05 01`
30.901 s, `43 05 31` 31.514 s (+0.62 s), recovery 32.149 s, ghost burst 33.087-33.174 s,
no real touch. Logs: `logs/runs/touch-panelinit2/`.

**Result.** The shim can talk to DSI0 and restart scanout, but a panel reset + DCS replay is
not enough to bring the EA8076 back: the display stays dark and the FTS failure is unchanged
(same `43 05 31` 0.6 s after sense-on). Panel command state is not the variable the
warm-boot failure depends on (or this partial replay does not reproduce TWRP's full
DSI/PHY/controller init). Do not spend more boots on this path; TWRP's remaining unexplored
difference is its FTS probe sequence (lockdown echo read + CRC register read + system reset
+ checksum-error event poll) and the display-notifier reset/sense-on after panel init.

### Next test: replay TWRP's FTS probe steps (`0139`, 2026-11-14)

**Patch.** `xnu/patches/0139-cepheus-fts-twrp-probe.patch` adds TWRP's two missing probe
steps to `bsd/dev/sweet_fts.c`, on top of 0138. The reference is TWRP's actual driver,
ST fts_521 (`work/fts/cepheus_ftsCore.c`, `cepheus_ftsIO.c`), which is what the TWRP dmesg
logs (`fts_crc_check`, `fts_get_lockdown_info`) match:

- `fts_get_lockdown_info()`: read the 4-byte header with `0xa6/00/00`, request the record
  with `0xa4/06/70`, wait 10 ms, poll for the echo (`0x43 0x01 a4 06 70`), then read the
  28-byte record and require its sequence counter to advance. No dummy byte: fts_521 sets
  `DUMMY_CONFIG=0` for I2C (the st-fts driver's dummy comment does not apply).
- `fts_crc_check()`: read HW register `0x20000078` with `0xfb` + 32-bit address (one byte,
  no dummy on I2C), mask `0x03`; if clean, GPIO system reset and a 500 ms event scan for
  config (`0x20/0x21`) or Cx (`0xa0/0xa1/0xa5/0xa6`) checksum errors.
- Both run after the sysinfo read and before the existing extra reset/sense-on, matching
  TWRP's `fts_init` -> lockdown -> fw-update/CRC -> `fts_init_afterProbe` order.
  `cepheustouchprobe=0` skips them; the sense-on line now logs `probe=1`.
- Expected log lines: `SWEET FTS: lockdown sig=a5 id=70 70 cnt=1/2 data=4838320546313200`,
  `SWEET FTS: crc reg err=0 val=00`, `SWEET FTS: crc reset ready=1 events=0 cfg=0 cx=0`,
  then `sense on ... probe=1`.

**Test.** Build the kernel with 0139, boot warm from TWRP (`fastboot boot`), and swipe at
~30 s. Success means no `43 05 30/31` and real touch events. If it still fails, the only
remaining TWRP difference is the display-notifier system reset + second sense-on after the
panel is initialized (TWRP: display blank/unblank at 1.586/1.838 s, FTS reset 1.838-1.860 s,
second sense-on 1.8604 s); the shim's panel replay could not reproduce that state.

### Warm-boot touch: reset count and panel state ruled out; TWRP recovers the chip (2026-11-13/14)

**Panel held in reset still fails.** `work/boot/runs/panel-off/touch-paneloff.img` (0138
payload + shim built with `SHIM_PANEL_OFF`) drives panel reset GPIO 6 low and holds it
across the jump to XNU, so the display stays dark while FTS initializes. The failure is
unchanged: ghost six-finger enters at 34.03 s and `43 05 30` at 34.311 s
(`logs/runs/panel-off/xnu.txt`). The display panel's state is not the trigger.

**Three resets do not fix it.** `work/boot/runs/touch-reset3/touch-reset3.img` is the 0138
payload with the cmdline patched in place (`cepheustouchreset2=2 cepheusreboot=120`, no
kernel rebuild), booted warm from TWRP at 23:55:11. The log shows `reset 2 ready` and
`reset 3 ready`, sense-on at 30.407 s, then the identical signature: ghost six-finger
enters at 33.918 s, `43 05 30` at 34.173 s, the driver's power-cycle recovery at 34.708 s
restores `43 05 01` at 34.808 s, and **no real touch is ever reported afterwards** (the
user's swipe at ~40-60 s produced none; alive ticks continue to 155.9 s). Springboard
booted normally. Logs: `logs/runs/touch-reset3/shim.txt`, `xnu.txt`. Reset count is not
the variable.

**TWRP recovers the chip after the failed XNU run.** With TWRP 3.5.2 booted afterwards, a
swipe produced 805 `ABS_MT_POSITION_X` samples / 31 `BTN_TOUCH` events
(`logs/runs/twrp-after-xnu/events.txt`). TWRP's init therefore undoes whatever our failed
run leaves behind; the chip is not latched.

**What the failure looks like.** Init and sense-on succeed; ~3.5 s later the chip reports a
burst of ghost multi-finger contacts at the edges and then `43 05 30` (MS invalid mutual
strength). It is a baseline/calibration failure during scanning, not a reset or handshake
failure, and the driver's own power cycle does not recover it. That points at an
environment difference at scan time, not at reset count.

**Init timing is ruled out.** `work/boot/runs/touch-early/touch-early.img` (0138 payload,
cmdline patched to `cepheustouchdelay=1`) starts FTS at 10.645 s, right after XNU's display
init at 9.592 s. It still fails, and sooner: `43 05 31` (MS invalid *self* strength) at
13.299 s, only 1.9 s after sense-on; the recovery power cycle restores `43 05 01` at
13.934 s but no touch is ever reported. So the failure is not boot-time-dependent: it is
relative to the start of scanning, and starting earlier just moves it earlier. Logs:
`logs/runs/touch-early/shim.txt`, `xnu.txt`.

**Panel VCI and AMOLED rails are identical too.** `panel-probe2` (payload-less image
`work/boot/runs/panel-probe2/panel-probe2.img`) adds `disp_vci_vreg` GPIO 21 (east TLMM)
and the PM8150L AMOLED blocks (SID 5: IBB `0xdc00`, AB `0xde00`, OLEDB `0xe000`) to the
shim-entry probe. Warm run `logs/runs/panel-probe2-warm/shim.txt`, cold run
`logs/runs/panel-probe2-cold/shim.txt` (phone fully powered off, Vol- + Power to fastboot).
The whole panel/touch/AMOLED section is identical line for line: VCI is `cfg 3c3` (pull-up,
16 mA, output) `io 3`, and the three AMOLED blocks read the same on both boots. So no
AP-visible rail or pin state at XNU entry explains the warm failure; the remaining
differences are what the driver does after power-on (TWRP's extra resets/probe commands) or
the display panel's internal DDIC state, which the shim cannot see.

**Done: both cmdline tests failed, and the panel-init replay is now ruled out.** `cepheustouchsysinfo=0`
and `cepheustouchreset2=3` both still fail with `43 05 30/31` and no touch; the shim's
34-command panel re-init (with and without `sde_restart`) leaves the display dark and the
FTS failure unchanged. See the two newest sections above. The next differentiator is TWRP's
FTS probe sequence (lockdown echo read, CRC register read, system reset, checksum-error
event poll), not the panel.

### Panel/touch pin state at shim entry is identical cold and warm (`panel-probe`, 2026-11-13)

**Probe.** Payload-less image `work/boot/runs/panel-probe/panel-probe.img` (SHA-256
`cbc4b9f026f0a411...`) logs, at shim entry: the five touch pins, panel reset GPIO 6, TE
GPIO 8, PM8150 LDO13/LDO14 (`0x4c00`/`0x4d00`), and the PM8150L LCDB block (`0xec00`).
Warm run: `logs/runs/panel-probe-warm/shim.txt`. Cold run (phone fully powered off, Vol- +
Power to fastboot, then `fastboot boot`): `logs/runs/panel-probe-cold/shim.txt`.

**Result: identical, line for line.** Touch SDA 55 / SCL 56 / IRQ 122 / reset 12 / VDDIO 84
are all `cfg 1` (pull-down, mux 0, input) and `io 0` on both boots. Panel reset GPIO 6 is
`cfg 2c3` (pull-up, mux 0, 8 mA, output) with `io 3` (high); TE GPIO 8 is `cfg 4` (mux 1 TE,
input) `io 0`. PON and both PM8150L LDO1/LDO2 dumps match byte for byte, including
`a0=80`. **ABL leaves the panel and touch pins in the same state on cold and warm boots, so
"ABL skips panel init on warm" is not supported by the observable state.**

**Two probes are dead ends.** LDO13/LDO14 reads fail on both boots (every register `!`):
their APIDs are not in the apps-EE SPMI map because RPMh/AOP owns them, so the shim cannot
read them. LCDB `0xec00` reads succeed but return zero on both boots; cepheus is an OLED
panel, and the live DTB's OLED rail node is `qcom,amoled` (AB `0xde00`, IBB `0xdc00`, OLEDB
`0xe000`, `status = "disabled"` in the base DTB), not LCDB. The panel VCI switch is
`disp_vci_vreg` (TLMM GPIO 21), not LDO13. Do not spend more boots on LDO13/14 or LCDB from
the shim.

**TWRP 3.5.2 needs its AVB footer stripped.** `twrp-3.5.2_9-0-cepheus.img` is 66,965,504
bytes of Android image plus a 512-byte vbmeta and a 64-byte footer at 67,108,800
(`avbtool info_image`: `Algorithm: NONE`, `Partition Name: recovery`). ABL rejects the
whole file with `Failed to load/authenticate boot image: Load Error`; `avbtool erase_footer`
truncates it back to 66,965,504 bytes and it boots in ~12 s. The bootable copy is
`downloads/twrp/twrp-3.5.2-nofooter.img`; `tools/cepheus-twrp.sh` now creates it
automatically and prefers 3.5.2. The 3.6.1 image also carries a footer and is only a
fallback.

**Warm-booted TWRP touch works.** From a TWRP session, `adb reboot bootloader` (warm reset)
then `fastboot boot downloads/twrp/twrp-3.5.2-nofooter.img`. TWRP came up in 11 s, and a
one-finger swipe produced 682 `ABS_MT_POSITION_X` samples / 27 `BTN_TOUCH` events with clean
enter/leave (`logs/runs/twrp-warm-touch/events.txt`, `dmesg2.txt`). TWRP's dmesg shows only
the normal `Mutual negative detect Force cal = 01 00 00 00 00 02` and no `43 05 30` during
the swipes. **The FTS chip and panel are usable after a warm reset under Linux; the failure
is specific to what our XNU driver does (or does not do), not a permanently latched chip
state.** Note TWRP re-initializes the display panel from scratch, which XNU never does; the
next test holds the panel in reset across the XNU jump to isolate that difference.

**Follow-up: the panel is not involved.** `work/boot/runs/panel-off/touch-paneloff.img`
holds panel reset GPIO 6 low across the XNU jump and FTS still fails with the same
`43 05 30`; see the newest section above.

### Touch warm-boot: PMIC pull-down, reset type and TWRP's init path are ruled out (2026-11-13)

**AVDD pull-down is `0xa0` bit 7, and it is already enabled.** Downstream and mainline
`qcom_spmi-regulator.c` map the PM8150L LDO subtype read from register `0x05` (`0x71`,
`LDO_510`) to the `hfsmps` ops, whose output pull-down is `0xa0` bit 7. `0x48` is the
common LDO pull-down and is not wired for this subtype. `touch-pmic-cold`/`touch-pmic-warm`
dumped LDO1, LDO2 and PON byte-for-byte identically, with `a0=80` in all four LDO dumps, so
the AVDD rail already has its discharge path on both boots. Residual rail charge is ruled
out (also independently by `touch-warm-railsoff`). The image
`work/boot/runs/touch-avdd/touch-avdd.img` was built on the wrong premise and **must not be
booted**; its shim code has been removed.

**Reset type is ruled out.** The failing warm runs already took the PS_HOLD hard-reset path
(`ctl 7 -> 7, ctl2 80 -> 80`), so "PSCI soft reset vs PMIC hard reset" is not the variable.

**TWRP v5.2.4 proves the panel-init-command hypothesis wrong.** Fresh TWRP dmesg
(`logs/runs/twrp-fts512/dmesg-fts.txt`, 126 lines): probe at 1.135 s, `System resetting...`,
sysinfo `FW 0060 / CONFIG 0037 / CX FFFF / AFE 03-03-03`, then **"No Config CRC Error", "No
Cx CRC Error", "No Panel CRC Error"**, so `fts_chip_initialization` chose `NO_INIT` and no
`A4 00 02/03` panel init was sent. The only `A2 03` writes in the OSS driver are the gesture
command `{0xA2,0x03,0x20,0x00,0x00,0x01}`; `production_test_initialization` is
`{0xA4,0x00,type}` and runs only after a CRC error. TWRP then does the same mode-handler
"Screen ON" + interrupt-enable sequence as our driver and gets a normal `43 05 01 ... 02`.
No kernel rebuild for `cepheustouchnative`.

**State note.** TWRP's FTS driver is currently unbound. Rebind fails
(`fts_gpio_setup: Failed to get gpio 122 (code: -16)`, `Probe Failed!`); reboot TWRP to
restore touch.

**Next (in progress).** Compare panel-side state at shim entry cold vs warm: panel reset
GPIO 6 and TE GPIO 8 on TLMM, PM8150 LDO13/LDO14 (VCI/VDDIO), and PM8150L LCDB
(`0xec00`: enable/status/LDO/NCP). If ABL skips a panel reset on warm boots, pulse GPIO 6
from the shim (active low; the screen stays dark because XNU has no panel-init path) and
check whether FTS calibration recovers.

### Full rail power-down before a warm reboot still fails (`touch-warm-railsoff`, 2026-11-13)

**Setup.** In TWRP: panel powered down with a synthetic KEY_POWER (already off, so a second
press confirmed `pm8150_l13/l14` disabled), then `echo 2-0049 >
/sys/bus/i2c/drivers/fts/unbind`, which runs `fts_remove` -> `fts_enable_reg(info, false)` and
drops both touch rails. Readback confirmed `pm8150l_l1` (AVDD) and `disp_vddio_vreg` (VDDIO)
**disabled, users=0**, and both panel rails disabled, before `adb reboot bootloader` ->
`fastboot boot` of the same `0138` image. No physical power cycle.

**Result.** Controller unpowered at XNU entry (30.604 s), rails cycled 200 ms (30.968 s),
sysinfo matched cold, `sense on err=0` (31.168 s), normal `43 05 01` (31.170 s). This time
there was **no `43 05 30` status event at all**. Instead, a ghost-touch burst at
34.674-34.708 s (four contacts, no user input), then only `alive ticks` (irq=1, errors=0)
through 93.564 s and silence. Display on at 50.607 s; the user confirmed no touch.
**Residual chip/panel power is ruled out.** A genuine full power-down of both supplies and
the panel rails before the warm reset does not restore touch; it only changes the failure
from an explicit `0x30` early-failure status to a ghost-touch burst followed by silence.
Whatever survives the warm SoC reset is not on the touch/panel rails: the remaining
candidate is PMIC/always-on (AOP/RPMH) state or another non-AP domain.

### Warm boot with touch init delayed to 90 s: still fails (`touch-warm-delay90`, 2026-11-13)

**Setup.** Same `0138` kernel (SHA-256 `c36667c5...`), repacked as
`work/boot/runs/touch-warm-delay90.img` with `cepheustouchdelay=90 cepheusreboot=240
cepheuswdt=0 cepheustouchlog=1 sweetbl=1`, booted from TWRP via bootloader.

**Result.** The driver started at 9.532 s, then powered the chip at 99.699-100.088 s
(already unpowered at XNU entry; rails cycled 200 ms; `avdd ldoc1 err=0`). Sysinfo matched
cold (`fw=0060 cfg=0037 res=2339x1079`), `sense on err=0` at 100.259 s, the normal force-cal
`43 05 01` at 100.267 s, garbage contacts at 103.889-103.971 s, `43 05 30` at 104.040 s,
recovery power cycle 1, clean `43 05 01` at 104.672 s, then no more FTS events. The user
confirmed no touch, waited, and rebooted to TWRP; the log ends at 104.672 s. SpringBoard's
display-on was at 49.435 s, 50 s before the chip was powered, so init timing relative to
display-on is ruled out.

**Consequence.** The chip fails ~3.6 s after sense-on on every warm boot, regardless of
when it is powered or what the panel is showing. Both follow-ups are now done and are
recorded above: `touch-cold-0138` works, `touch-ablwarm-0138` fails without TWRP.

### ABL-only warm reboot also breaks touch (`touch-ablwarm-0138`, 2026-11-13)

Phone powered off, cold fastboot, then `fastboot reboot-bootloader` followed by `fastboot
boot` of the same `0138` image. **No TWRP and no prior XNU touch driver ran before this
boot.** The controller was unpowered at XNU entry (35.435 s), rails were cycled 200 ms
(35.800 s), sysinfo matched, `sense on err=0` at 35.999 s, and the normal `43 05 01` came at
36.001 s. Then, 3.615 s later, `43 05 30` (39.616 s), the early-failure recovery power cycle
(40.243 s), a clean `43 05 01` (40.251 s), and no further status events; display on at
55.568 s. The user confirmed touch did not work. **TWRP is ruled out.** Cold and warm reach
XNU with the same unpowered chip and run the same init, so the trigger is the SoC warm reset
/ the second ABL pass, not any driver state left by TWRP or XNU.

### Cold boot with the current 0138 kernel: touch works (`touch-cold-0138`, 2026-11-13)

The only earlier cold success (`brightness-dcs4`) used `0136`, before the 0137/0138 rail
cycling, so this closed that gap. Phone powered off, Vol- + Power to fastboot, then
`fastboot boot` of the same `touch-warm-paneloff.img`. The controller was unpowered at XNU
entry (31.489 s), rails were cycled 200 ms (31.880 s), sysinfo matched, `sense on err=0` at
32.052 s, and the only status event was the normal `43 05 01` (32.055 s) - no `0x30` or
`0x31`. Touch worked; `SWEET FB: display on` at 51.515 s. **No 0137/0138 regression.** The
warm failure therefore depends on the boot history, not the driver version or init sequence.

### Warm boot with dimming off: touch still fails, and the DSI state was cold-like (`touch-warm-dimmingoff`, 2026-11-13)

**Pre-warm state.** TWRP's panel was awake (`brightness=1200`, `panel_info` populated), woken with
a synthetic KEY_POWER if needed. Raw DCS through the writable sysfs node
`/sys/devices/platform/soc/ae00000.qcom,mdss_mdp/drm/card0/card0-DSI-1/mipi_reg`
(`/proc/mipi_reg` is read-only): dim `0x51 0x0100`, restore `0x51 0x04b0`, then the panel's own
`dimmingoff` (`0x53 0x20`, packet `390100000100025320`). All three writes returned 0 with no
`panel not ready`; a DCS 0x53 readback is `0x00` both before and after, so the panel does not
expose dimming state to a read. Booted `work/boot/runs/touch-warm-paneloff.img` (0138,
`cepheuswdt=0 cepheustouchlog=1 sweetbl=1`, 200 ms rail-off, `cepheusreboot=300`) with
`fastboot boot`.

**Result.** Identical to `touch-warm-paneloff`: chip unpowered at XNU entry (29.665 s), rails
cycled (30.030 s), sysinfo normal, `sense on err=0` (30.229 s), `43 05 31` (30.864 s), ghost
contacts (33.9 s), `43 05 30` (34.194 s), recovery 2, then only `alive ticks`/`irq=1
errors=0`. The user confirmed SpringBoard booted with no touch. Dimming is ruled out.

**New observation that changes the next test.** The first DCS at 49.850 s read `status
0x00000000 fifo 0x11111010` — the cold-boot DSI signature, not the `status 0x5 fifo 0x1010`
warm signature seen in `touch-warm-paneloff`. So a warm boot can reach the cold DSI state and
touch still fails; the DSI controller state is not the differentiator at the time of the first
DCS. The failure is already decided earlier: the chip reports `43 05 31` 0.6 s after sense-on
(30.229 s), while cold reports the same event only at 177.7 s and then keeps working.

**Timeline clue (unconfirmed).** The only XNU "display on" is `SWEET FB: display on`, which
patch 0079 shows is backboardd's `RequestPowerChange` state, not a DSI/panel power command,
and it arrives at 49.8 s — ~20 s after touch init. The panel is on from ABL in both cases, so
the difference is the panel content/refresh state when the chip first calibrates (boot logo /
TWRP leftovers at 30 s versus a settled fastboot screen at 145 s on cold). Follow-up (done,
failed): `cepheustouchdelay=90` ran with the same 0138 kernel; see `touch-warm-delay90` at
the top. The chip was powered 50 s after display-on and still failed identically, so display
state/timing at init is ruled out.

### Warm boot with the TWRP panel powered off: touch still fails (`touch-warm-paneloff`, 2026-11-13)

**Pre-warm state.** TWRP's screen timeout blanked the panel (`dsi_panel_disable:
DSI_CMD_SET_OFF` at 247.12 s); `pm8150_l13`/`l14`, `disp_vci_vreg`, `lcdb_ldo` and
`lcdb_ncp` were disabled before the reboot (`disp_vddio_vreg` stayed enabled for touch).
Booted the `0138` kernel (SHA-256 `c36667c5...`, `cepheuswdt=0 cepheustouchlog=1 sweetbl=1`,
default 200 ms rail-off) with `cepheusreboot=300`.

**Result.** The chip was already unpowered at XNU entry (`controller before rails-off:
unpowered` at 29.742 s). The driver cycled the rails (`rails cycled, off 200 ms, avdd ldoc1
err=0` at 30.100 s), sysinfo was normal, `sense on err=0` at 30.272 s, and then the same
warm failure: `43 05 31` at 30.864 s, ghost contacts at 32.87 s, `43 05 30` at 33.221 s,
recovery 2, then only `alive ticks` with `irq=1 errors=0`. No touch. The first DCS write at
50.248 s still saw the warm DSI signature (`status 0x00000005 fifo 0x00001010`, `int
0x00000500` pending), i.e. powering the panel off in TWRP did not give ABL/XNU a cold DSI
state. This rules out a simple "panel rail still on in TWRP" explanation, but not the panel
register state itself.

**Follow-up (done, failed).** The `dimmingoff` test was run; see `touch-warm-dimmingoff` above.
TWRP sends `dimmingon` (`0x53 0x28`) on every panel enable and XNU never clears it, so this was
the obvious panel-register candidate. The writable DCS node is the sysfs one
(`.../card0-DSI-1/mipi_reg`), not `/proc/mipi_reg`; format is 2-digit read flag, space,
2-digit read length, space, then the hex packet, e.g. `00 00 390100000100025320`. The panel
ignores a 0x53 readback, so the write cannot be confirmed by reading back. Dimming-off did not
fix touch, and that boot reached the cold DSI signature anyway.

### Warm-boot touch: `0137` failed; `0138` probes the rail-off (2026-11-13)

**`0136` brightness result (`brightness-dcs4`, cold boot).** The user cold-booted to
fastboot and booted `work/boot/kernel-brightness/kernel.development.sweet` with
`cepheuswdt=0 cepheustouchlog=1 sweetbl=1 sweetfb_sel_log=1`. Touch initialized
(`sense on err=0`; the first force-cal was later, at 177.7 s, and recovered) and the
Control Center slider changed the panel brightness.

The DCS path is proven: the display-open write `brightness = 1` / `backlight-level = 0`
sent level 2 (`DCS 0x51 level 0x002 ... int 0x00000501`), and the first slider moves sent
`572 -> 0x013 (19)`, `857 -> 0x01c (28)`, `1428 -> 0x02e (46)`, `3856 -> 0x07a (122)`,
`4427 -> 0x08c (140)`, `4569 -> 0x090 (144)`, `4712 -> 0x095 (149)`. These match
`2 + value * (0x7ff - 2) / 0x10000`; `int` bit 0 (`0x501`) shows command-done, so no
further DCS/FIFO debugging is needed. The first eight sends are logged; later slider
values still update the panel even though they are not printed.

**Warm-boot touch still fails with `0137` (`touch-warm2`).** The rails were released
(`rails cycled, avdd ldoc1 err=0` at 29.713 s), but the chip still reported `43 05 31`
at 30.469 s, then five ghost contacts at 32.273 s, `43 05 30` at 32.595 s, and stayed
silent after recovery 2 (`power cycle 2 err=0`), with `irq=1 errors=0` to 80 s. The
100 ms off window evidently did not produce a POR.

**Cold boot also sees a force-cal, but recovers.** `brightness-dcs4` logged `43 05 31`
at 177.708 s (12 s after display-on) and normal touches from 183.781 s. The panel
turning on can trigger a force-cal even on a working boot; the difference is that a
cold-started chip recovers, while a warm-started chip fails from the first second.

**`0138` touch (written, not yet built).** `cepheustouchpwrms=N` (default 200, clamped
200..30000) sets the rails-off window. The window is a busy-wait with reset asserted,
so neither the RPMh sleep set nor phantom power through the reset ESD diode can
re-power the chip. The driver probes the `0xa6` sysinfo frame once before the rails
drop (positive control) and once at the end after releasing reset at 2 mA:
`controller before rails-off: alive` validates the probe, and `rails off N ms:
controller unpowered` then proves the rail really dropped; `alive` means it did not.
The power-up order is back to the verified `0136` sequence (VDDIO, then AVDD). Build
`0138` and test a **warm** boot with `cepheuswdt=0 cepheustouchlog=1 sweetbl=1
cepheustouchpwrms=5000`; if the probe says `unpowered` but touch still fails, the chip
is no longer the difference and the panel's warm state is next.

### Brightness: `0135` matched the BC control; `0136` drives DCS 0x51 (2026-11-13)

**`0134` result (`brightness-rec1`).** Booted from `0134` with `cepheuswdt=0
cepheustouchlog=1 sweetbl=1 sweetfb_sel_log=1`. All 33 `SWEET FB: rec` lines are
`p 00 b 0000000000 n 0.000 raw 0 0 0 0 0`; there is no `sel 51` and no `SWEET FB:
setProperties`. The UI stayed at 0.2 fps and frames stopped at 380.334 s; the slider
attempt at 410.398-416.177 s produced no swaps at all. `SwapSetBrightness` never took its
cached path, i.e. `[IOMFB+0xb39]` is still 0 because the BC control never matched.

**Why the match fails.** The IOMFB match callback (`0x1af225ddc`) requires
`BCBrtControlGetIsValid` and non-nil capabilities; the embedded-panel path also requires
`capabilities["BuiltIn"]`. `BCAppleBacklightBrtControl initWithService:` (`0x20314a660`)
reads `IODisplayParameters` from the service and returns nil at `0x20314b80c` when the
property is absent, so the fake service is discarded before `BuiltIn`/`MaxNits` can be
built. The accepted shape was recovered from the iPhone X `AppleARMBacklight` in the
`AppleARMPlatform` kext (`__PLK_TEXT_EXEC`, `0xfffffff005616b60`-`0xfffffff005616d40`):

- `brightness`: `min` 0, `max` 0x10000, `value` 0x8000 (16.16 user brightness)
- `rawBrightness`: `min` 0, `max` 0x7ff, `value` 0
- `BrightnessMicroAmps`: `min` 0, `max` 25000, `value` 0 (BC multiplies by 1000)
- `BrightnessMilliNits`: `min` 0, `max` 600000, `value` 0, `uncalMilliNits` 0 (BC
  divides by 1000, so 600 nits)
- `force-brightness-control` = true sets BCAppleBacklight's `_override` (`+0xa2`);
  without it `setNits:error:`/`getNitsWithError:` reject the embedded panel with
  `NotSupported`

BC then reports `_minUser` 0/`_maxUser` 0x10000, `_maxNits` 600/`_minNits` 0 and builds
`capabilities` with `BuiltIn`, `MaxNits`/`MaxNitsPanel` 600, `MinNits` 0 and `RegistryID`.
`BuiltIn` is exactly what the embedded-panel match tests.

**`0135` result (`brightness-params1`, booted 2026-11-13).** The match worked:

- `SWEET BL: IODisplayParameters published (...)` at 171.242 s, immediately before
  `SWEET BL: AppleARMBacklight published (DBV 0x8b..0x7ff)`.
- `BCAppleBacklightBrtControl` matched (`SWEET FB: backboardd sel 50 ... [65536 0 0 0]`),
  and the slider no longer stalls the UI: SpringBoard ran at up to 39 fps while interacting
  and touch stayed responsive.
- The slider reaches the fake backlight as `SWEET BL: setProperties brightness = ...` /
  `backlight-level = ...` in 16.16 units: for example 32197 at 245.315 s, 65536 at
  261.491 s, 0 at 262.428 s. `brightness-nits` is only written once as 0 when the display
  opens; it is not the slider path.
- The `SWEET FB: rec` SwapEnd records stay `p 00 b 0000000000 n 0.000` even while the
  slider moves. Brightness does **not** ride the SwapEnd record; ignore that path.

The panel did not change because `AppleARMBacklight::setProperties` only logged the value.
`0136` now maps it and sends the panel command.

**`0136` (booted 2026-11-13; result above).** In `AppleARMBacklight::setProperties`, `brightness`
(or `backlight-level`) is mapped from 16.16 to the EA8076 DCS range: 0 stays 0, everything
else is `2 + value * (0x7ff - 2) / 0x10000` (live DTB `qcom,mdss-dsi-bl-min-level` 2,
`qcom,mdss-dsi-bl-max-level` 0x7ff). The panel takes DCS `0x51` with a two-byte big-endian
level (`qcom,mdss-dsi-bl-dcs-type-ss`, confirmed by the `qcom,mdss-dsi-on-command` byte
stream, which sends `39 .. 03 51 00 00`), so the command is a DCS long write packaged as
the 8-byte command FIFO image `03 00 39 c0 51 msb lsb ff`. It goes through the DSI0 command
FIFO at `0xae94000` (`DSI_TEST_PATTERN_GEN_CTRL` 0x015c,
`DSI_TEST_PATTERN_GEN_CMD_DMA_INIT_VAL` 0x017c, embedded bit 28 in
`DSI_COMMAND_MODE_DMA_CTRL` 0x003c, `DSI_DMA_CMD_LENGTH` 0x004c, trigger
`DSI_CMD_MODE_DMA_SW_TRIGGER` 0x0090). The ABL left the controller in command mode and
XNU only adds commands, so no IOVA/SMMU or DSI mode setup is done. The first use logs
`SWEET BL: DSI ctrl0 hw ... ctrl ... status ... fifo ... int ... dma ...`, then the first
eight sends as `SWEET BL: DCS 0x51 level 0x... packet c0390003 ... int 0x...`.
Completion is `DSI_INT_CTRL` bit 0 (`CMD_MODE_DMA_DONE`); a command still in flight is not
reset: the next update waits up to 20 ms and defers the new level rather than corrupting
the FIFO (after four failed waits it assumes the status is not visible and takes the FIFO
back). `sweetbl_dcs=0` disables the DCS writes (default 1); `sweetbl=3` is unchanged and
still logging-only.

**Test (passed on `brightness-dcs4`).** Boot `0136` with `cepheuswdt=0
cepheustouchlog=1 sweetbl=1 sweetfb_sel_log=1`. Wait for SpringBoard, then move the
Control Center brightness slider. Expected evidence, in order:

1. On the first slider write, `SWEET BL: DSI ctrl0 hw 0x...` with `ctrl` bit 2 set
   (command engine on), plus the raw status/fifo/int/dma values.
2. `SWEET BL: setProperties brightness = ...` followed by
   `SWEET BL: DCS 0x51 level 0x... packet c0390003 ...51 ...` for the first eight moves.
   The first line may end in `(pending)`; later lines should not, which proves the
   command-done status is visible.
3. The panel brightness follows the slider; 0 turns it off and 0x10000 reaches
   `level 0x7ff`.

If the DCS log appears but the panel does not change, capture the register line and the
first packet lines. The remaining variables are the packet data type/byte order and the
DSI controller mode (`ctrl` bit 2, `status`). If every line says `(pending)`, the
command-done status is not visible and the next step is a different completion check, not
resetting the FIFO blindly.

### Brightness: selector 55 is SetMatrix; brightness rides in the SwapEnd record (2026-11-11)

**Correction to the earlier note.** Selector 55 is `_IOMobileFramebufferSetMatrix`
(`0x1af2237d0`): one scalar input (`9` in the gate3 run) plus a 72-byte 3x3 matrix, no outputs.
Selector 50 is `_IOMobileFramebufferSetBrightnessCorrection` (`0x1af223610`, vtable `+0x7c8`).
Neither one carries the brightness level, so `0133` is correct logging of a different call and
does not answer the brightness question. Do not boot just for selector 55.

**The real brightness stack.** `IOMobileFramebuffer.framework` loads
`BrightnessControl.framework` and resolves `BCBrtControl*` functions.
`_IOMobileFramebufferSetBrightnessControlCallback` (`0x1af225b70`) installs the monitor
callback `0x1af225ddc`: a BC control that is valid, has capabilities, and matches the IOMFB
`BuiltIn`/`DFR`/`DisplayContainerID` becomes this display's control. On match it sets
`[obj+0xb39]=1`, stores the BC object at `[obj+0xb70]`, MaxNits at `[obj+0xb48]`, MinNits at
`[obj+0xb50]`, and calls `BCBrtControlSetDisplayService`. `_IOMobileFramebufferSwapSetBrightness`
(`0x1af227540`, vtable `+0x6f0`) then calls `BCBrtControlSetNits` with the value and caches it
in the `obj+0x18`/`0x46c` swap record that `SwapEnd` sends to the kernel: present flag at
record+`0x2e7` (`obj+0x2ff`) and nits as a double at record+`0x2ec` (`obj+0x304`). With
`[obj+0xb39]` clear it only logs `SwapSetBrightness: Brightness control is not enabled` and
writes nothing. `SwapSetBrightnessLimit` (`0x1af2275dc`, `+0x6f8`) uses record+`0x2e8`/
`+0x2f4`; `+0x700` record+`0x2e9`/`+0x2fc`; `+0x708` record+`0x2ea`/`+0x304`; `+0x710`
(ambient lux) record+`0x2eb`/`+0x363`; `+0x718` (contrast enhancer) record+`0x37b`/`+0x37f`;
`+0x720` (user luminance) record+`0x383`/`+0x387`.

**The 5 s stall.** CoreBrightness commits brightness with a swap-done callback
(`CommitBrightness: %f with swap done callback`, block `0x1af0bd25c`); success logs
`... has been swapped on the display` (`0x1af0bd2ec`) and the observed 5 s spacing matches
`Last update has not been swapped on display in timeout %f.` (`0x1af0bd83c`). The leading
hypothesis is a brightness commit waiting for a display swap-done signal, not a MIG async
completion.

**`0134` (booted as `brightness-rec1`; see the section above).** With `sweetfb_sel_log=1`, `AppleCLCD::swap` now dumps
every `SwapEnd` record: presence bytes `p` (+0x2e2) and `b` (+0x2e7..+0x2eb), the five doubles
as raw hex, the nits double also as milli-nits, and the two floats. Boot `cepheuswdt=0
cepheustouchlog=1 sweetbl=3 sweetfb_sel_log=1`, move the Control Center brightness slider, and
look for `SWEET FB: rec p .. b .. n <x>.<yyy> raw ...`. If `b` stays `0000000000` and `n` stays
`0.000` while the slider moves, the BC control never matched and `SwapSetBrightness` is not
writing the record; the next step is then to make the fake backlight pass BCBrtControl
validation (capabilities/container/max/min nits, `IODisplayParameters`). If `n` becomes
nonzero, the record path works and what remains is mapping it to DCS `0x51` (DTB `bl-min` 2,
`bl-max` 0x7ff) and the 5 s commit wait. The panel is still not touched.

**`0132` is ruled out (static).** XNU's async MIG completion is not the wait: the userland
IOMFB framework does not import `IOConnectCallAsyncScalarMethod`/`AsyncStructMethod`, so the
5 s delay is not an unsent async completion. Do not spend another boot on `0132`.

### Brightness: the fake backlight stalls the UI; `0129` gates publication and logs selectors (2026-11-10)

**`0131` and `0132` were superseded (see the section above).** `0131` showed that nothing ever
registers an IOMFB notification port, and that the stall is real (a swipe produced one swap in
5 s). The async-completion hypothesis behind `0132` was ruled out statically; the current
suspect is the CoreBrightness brightness commit waiting for a swap-done signal. Full reasoning
and results are under "`0131` result" below.

**Phase-1 stall (`brightness-on1`, `0127` only).** The user was actively swiping from
165.689 s to 297.384 s (sessions of 11.5 s, 16.8 s and 30.3 s), and the touch driver logged
5279 enter/motion/leave events, but the framebuffer did exactly **1 swap per 5 s (0.2 fps)**
from 143 s onward. `touch-on6` without the fake backlight ran at 8-26 fps over comparable
interaction. The fake `AppleARMBacklight` causes a real display/UI stall, not idle
throttling.

**Why publishing it changes the path.** `_DisplayOpen` returns **nonzero on success**. Its
success paths set `DisplayBrightnessAuto=false`; with an ARM-conforming backlight service
present it also unconditionally sets `DisplaySyncBrightnessTransactions=true`. So display
`+0x129` is 1, and `_SetIOMFBBrightness` takes the sync branch instead of the registry branch:
`_SendSyncDBVNotification` → block → `display+0x10(display+0x18,
"FrameSynchronizedBrightnessTransaction", message)` (`0x1af035f20`, stored by `_DisplayOpen`)
→ module queue → block `0x1af037d9c` → `-[CBDisplayModuleiOS
handleNotificationForKey:withProperty:]` and `_DisplaySetInternalDictionaryProperty` with
`DisplayBrightness`/`DisplayNitsKey`. That chain is entirely userland, so with the ARM node
published no `IOMFBBrightnessLevel` registry write is expected at all; a silent
`SWEET FB: setProperties` is consistent with the current understanding. The earlier "with the
property absent the registry path is the one we want" note is superseded: the ARM container
sets the sync property itself.

**`0129`/`0130` (gating + instrumentation).** `sweetbl` selects what/when the fake
backlight is published: `0` never, `1` at `AppleCLCD::start` as `AppleARMBacklight` (default,
same as `0127`), `2` as `AppleARMBacklight` on the first display-on requested by backboardd,
`3` at `AppleCLCD::start` as a non-conforming `SweetBacklight` (`0130`). Mode 3 still has
`backlight-control=true`, so CoreBrightness matches it, but `IOObjectConformsTo(...,
"AppleARMBacklight")` is false and `CBDisplayContaineriOS` should take its non-ARM path,
leaving `DisplaySyncBrightnessTransactions` off and using the registry write.
`sweetfb_sel_log=1` logs every IOMFB `externalMethod` call with up to four scalar inputs, not
just the first call per selector. The panel is still not touched.

**Test (one phase at a time; let the user observe each step).** Build from `0130` and
`fastboot boot` with `cepheuswdt=0`:

1. `sweetbl=0` control: expect `SWEET BL: disabled (sweetbl=0)`, no backlight match, and a
   responsive UI while swiping (well above 0.2 fps). This proves the stall comes from the
   fake backlight.
2. `sweetbl=1 sweetfb_sel_log=1`: expect publication at ~93 s, the match, and the stall. Move
   the slider and capture any `SWEET FB: setProperties`, raw `sel 50/51/84` lines, and the
   5 s swap stats.
3. `sweetbl=3 sweetfb_sel_log=1`: non-ARM service. Expect `SweetBacklight (non-ARM)
   published`, the same `backlight-control` match, and — if the non-ARM path works — a
   responsive UI and `SWEET FB: setProperties IOMFBBrightnessLevel = <millinits>` writes when
   the slider moves.
4. `sweetbl=2`: publication waits for the first display-on. If the stall disappears but
   brightness does not move, the ARM container was created too late for CoreBrightness to use
   it.
5. `sweetbl=3 sweetfb_sel_log=1` from `0132`: same as (3), but the IOMFB user client now
   completes MIG async calls. Expect `sel <n> async wake` / `sel <n> async reply` lines and a
   responsive UI; move the brightness slider and note the `sel 50` values.

Open: `0131` ruled out the IOMFB notification port as the wait (see the `brightness-notify1`
result below): `registerNotificationPort` is never called, so the ~5 s stall is a MIG async
completion that our `externalMethod` override swallows. `0132` sends the completion
(`sendAsyncResult64`) and logs the first async call/reply per selector. Phase 2 (DCS `0x51`)
starts only after a runtime write path is confirmed.

**Control result (`brightness-gate0`, 2026-11-10).** Booted from the `0129`+`0130` build with
`cepheuswdt=0 cepheustouchlog=1 sweetbl=0`. `SWEET BL: disabled (sweetbl=0)` at 53.916 s;
backboardd's `backlight-control` match returned `none` at 59.007 s and `0 found` at 94.522 s;
display on at 94.558 s. Touch worked (97 HID down/up events from 109.4 s to 181.9 s) and the
5 s swap buckets ran at **1.6-13.8 fps** throughout the interaction, versus 0.2 fps in
`brightness-on1`. Brightness could not be changed, as expected with no backlight service.
This confirms the stall comes from the fake backlight publication, not from the kernel or
touch path.

**Non-ARM result (`brightness-gate3`, 2026-11-10).** Booted with `sweetbl=3
sweetfb_sel_log=1`. `SweetBacklight (non-ARM) published` at 9.536 s, the `backlight-control`
match returned `SweetBacklight` at 14.701 s and `1 found` at 50.238 s. Display on at 50.290 s;
the 5 s buckets were already 0.2 fps from the first one (59.538 s), and the user reported the
UI unusable. Only 11 HID touch events were logged (all at 33.888 s, before display on), and
the display cycled off/on at 65.3/67.9 s and 95.4/109.9 s. Brightness-related IOMFB calls:
`sel 84` with 4 bytes of zero at 50.292 s, `sel 50` with `0x10000, 0` at 50.292 s and again at
60.306 s. There were **no** `SWEET FB: setProperties` and no `SWEET BL: setProperties` lines.
So non-ARM conformance does not avoid the stall; the trigger is having a matched backlight
service present when the display opens. The next test is `sweetbl=2` (publish only at first
display-on), which keeps `_DisplayOpen` from seeing a backlight service at startup.

**Delayed-publication result (`brightness-gate2`, 2026-11-10).** Booted with `sweetbl=2
sweetfb_sel_log=1`. `SWEET BL: deferred to first display-on (sweetbl=2)` at 9.479 s; the
`backlight-control` match returned `none` at 14.072 s and `0 found` at 49.583 s;
`AppleARMBacklight published` at 49.627 s, immediately followed by display on (off at
50.072 s, on again at 52.817 s). The UI was responsive at first: the 5 s buckets were 38
swaps at 57.571 s, 87 at 72.571 s, 21 at 77.571 s, then 1 at 82.571 s. The 72.571 s bucket
covers a 15 s window (the vsync timer lost ~10 s between 57.5 and 67.5 s), so the real rate
was ~5.8 fps, not the printed 17.4. The IOMFB call timeline shows the stall exactly:
backboardd's last frame before the first stall is at 56.279 s and the next is at 70.587 s
(**14.3 s**), then frames until 73.424 s, then a **5.06 s** gap to 78.483 s, after which
frames are ~5 s apart. A 5 s frame spacing is a timeout, not a slow frame. The service was
published after `_DisplayOpen` had already matched `0 found`, yet CoreBrightness still
picked it up later and switched to the frame-synchronized path. Only 19 HID touch events
were logged, all at 33.45-33.48 s (before display on); the FTS thread stayed alive
(`alive ticks=... irq=1` every ~12.5 s) but no touches were delivered after display on. No
`SWEET FB: setProperties` and no `sel 51` calls; `sel 84` (4 zero bytes) and `sel 50`
(`0x10000, 0`) were called at 49.628 s, with `sel 50` again at 49.655 s. The 5 s timeout
points at the frame-synchronized brightness transaction waiting for a display notification
that the fake IOMFB never sends; `0131` logs `registerNotificationPort` and dropped
`notify` calls to confirm.

**`0131` (notification-port instrumentation).** Logs every
`IOMobileFramebufferUserClient::registerNotificationPort` call (client, type, port, refCon),
the first two dropped `notify` calls per type (with the reason: no callback function or no
port), the first successful send per type, and the client count in the 5 s stats. Instrumentation
only; the panel is still not touched.

**`0131` result (`brightness-notify1`, 2026-11-10).** Booted with `cepheuswdt=0
cepheustouchlog=1 sweetbl=3 sweetfb_sel_log=1`; the user waited 1 min after SpringBoard and
then rebooted to TWRP. `SweetBacklight (non-ARM) published` at 53.243 s; the
`backlight-control` match returned `SweetBacklight` at 57.824 s and `1 found` at 93.339 s;
display on at 93.388 s. There is **no `registerNotificationPort` line at all** in the run, and
no selector 72 (`notifications`) call, so no client ever registered an IOMFB notification
port. The only `notify` lines are `kernel_task notify type 5 dropped: func 0 port 0x0` (one
or two per display-on), i.e. the vsync timer finds no callback; the 5 s stats report `2
clients` (kernel_task + backboardd) but neither has a port. The stall is real and not idle:
the user swiped up repeatedly from 183.03 s to 188.04 s (touch 0, y 1107 -> 1503) and only
one swap happened in that whole window (184.734 s); every 5 s bucket after display on is
`1 swaps (0.2 fps)`. `sel 84` (4 zero bytes) and `sel 50` (`0x10000, 0`) appear at display on
(93.389 s) and `sel 55`/`sel 50` again at 103.403 s, always returning immediately with no
`setProperties` and no `sel 51`.

**Conclusion and `0132`.** Since every IOMFB call is logged on return and the 5 s gap sits
between calls, the wait is in userland. `IOUserClient::callExternalMethod` invokes our
`externalMethod` override directly, so XNU's async machinery never sends the MIG completion
for `IOConnectCallAsyncScalarMethod`/`AsyncStructMethod`; the caller then waits on its wake
port until its own 5 s timeout. `0132` adds `sweet_fb_async_reply()`: when
`args->asyncWakePort` is non-null it calls `IOUserClient::sendAsyncResult64` with the
`asyncReference` array XNU filled in, and logs `sel %u async wake` / `sel %u async reply`
once per selector (`sweetfb_async_log=1` logs all). Build from `0132` and boot with
`cepheuswdt=0 cepheustouchlog=1 sweetbl=3 sweetfb_sel_log=1`. Expected: one `async wake` /
`async reply` line per async selector (the selector number identifies the transaction),
no more 5 s spacing while swiping, and `sel 50` with other values when the brightness slider
moves. If the UI is still 0.2 fps, the async lines will show which selector is left
uncompleted.

### Brightness: phase 1 matched; the write goes to AppleCLCD, not the backlight node (2026-11-10)

**Phase-1 result (`brightness-on1`).** `0127` booted with `cepheuswdt=0 cepheustouchlog=1`.
`AppleARMBacklight` published at 93.036 s, backboardd's `backlight-control` match returned
`AppleARMBacklight` at 97.518 s and `1 found` at 133.043 s, and the display came on at
133.093 s. There were **no `SWEET BL: setProperties` lines**, even while the slider was
moved. The match is solved; the write target was wrong.

**Why no writes were seen.** CoreBrightness's `_DisplayOpen` calls
`IOMobileFramebufferCreateDisplayList`, then `IOMobileFramebufferOpenByName` and
`IOMobileFramebufferGetServiceObject`; it stores that service object at display `+0x6c`.
`_SetIOMFBBrightness` writes `IOMFBBrightnessLevel` to `+0x6c` with
`IORegistryEntrySetCFProperty`, i.e. to the **IOMobileFramebuffer service (AppleCLCD)**,
not to the `AppleARMBacklight` child. `brightness-nits` is written the same way by
`_DisplaySetBLDriverProperty`.

**DBV vs registry.** `_SetIOMFBBrightness` checks `+0x129`, which is set by the display
property `DisplaySyncBrightnessTransactions` (CFString at CoreBrightness data
`0x1d8897148`); `+0x12a` is set by `DisplayFasterEDREngaged`. With `+0x129 == 0` the registry
branch writes `IOMFBBrightnessLevel`. The ARM-backed `_DisplayOpen` success path sets it to
1, so the sync branch runs instead (see the `0129` section above).

**Units.** `_SetBLDriverNitsValue(self, int millinits, bool force)` logs
`Setting BL nits value to %f` after dividing by 1000.0, and passes the same integer to
`_SetIOMFBBrightness`. `IOMFBBrightnessLevel` is therefore an integer in **millinits**
(750000 = 750 nits), not 16.16 fixed-point. The iPhone X max-nit-value is 0x2ee (750).

The iPhone X `backlight` ADT node supplies the properties CoreBrightness expects: `DBVMax`
0x7ff, `DBVMin` 0x8b, `override-boot-brightness` 0x2e4, `backlight-update-policy` 0x4,
`backlight-level-encoding` 0xb, `use-AAP` 1, `use-AAB-architecture` 1, `max-nit-value`
0x2ee. `0127` publishes those on the `AppleARMBacklight` child of `AppleCLCD`.

**`0128` (phase 1b, logs only).** `SweetFramebuffer.cpp` now overrides
`IOMobileFramebuffer::setProperties(OSObject *)` (inherited by `AppleCLCD`) with the same
key/value logging as `AppleARMBacklight`, stores `IOMFBBrightnessLevel`/`brightness-nits`
back as properties, and returns success. The shared helper also keeps the
`SWEET BL: setProperties` logging. `externalMethod` now logs the raw inputs of IOMFB
selectors 50 (`SetBrightnessCorrection`), 51 (ICC curve) and 84 (float setter) on every
call, so we can tell if the slider uses that path too. **It does not change the panel
yet.** The DCS path is still `panel0-backlight` (max 0x7ff, DTB
`qcom,mdss-brightness-max-level`); WLED is not the display backlight.

**`0128` test (superseded).** This build was never run on hardware; the `0129` test above
covers it and adds the `sweetbl` control. If `0128` is booted anyway, expect publication at
~93 s, the match at ~97 s, and
`SWEET FB: setProperties IOMFBBrightnessLevel = <millinits>` only if the registry branch is
reached. Phase 2 maps millinits to the panel DCS `0x51` range (DTB `bl-min` 2, `bl-max`
0x7ff) and writes it through the MDSS/DSI path.

### Touch works: the missing post-sysinfo reset was the fix (2026-11-09)

`touch-on6` booted the user-built kernel (`kernel.development.sweet`, sha1
`739d184dfa30c37b31e332ba85a5c9aad9981c13`, built from patch `0125`) with the standard debug
args plus:

```
cepheuswdt=0 cepheustouch=1 cepheustouchdelay=55 cepheustouchlog=1
```

and `-s 180`. Touch init ran at 74.45 s, sysinfo reported
`fw=0060 chip0=3639 cfg=0037 cx=ffff afe=03/03/03 proto=00`, the extra reset logged
`reset 2 ready err=0`, and sense-on at 74.63 s succeeded. The first real touch was at
88.58 s, i.e. after SpringBoard was up; the screen was fully usable.

The whole run is clean:

- 2378 enter/motion samples, all touch id 0, all type 1 (finger); 59 down events and
  59 matching up events. One-finger tracking was smooth.
- **No `43 05 30` (MS invalid mutual strength) and no `43 06` water enter/exit** for the
  entire run. The pathological edge cross from `touch-on2`..`on5` is gone.
- The only status during use was `43 05 21` at 139.83 s, which the vendor table names
  "SS invalid Self Strength soft Force cal" (a self-cap soft recal), not water and not an
  MS error; touch input continued immediately before and after it.
- The HID edge flag fires: 18 samples were marked `edge`, including y=2339 (bottom clamp)
  and y=55 (top). x stayed within 18..993, so the right edge was not exercised in this run.
- One 4 ms `edge` tap at x=18,y=1000 at 192.69 s while the phone was otherwise idle
  (`touch 0 down`/`up` 0.4 ms apart). It did not repeat; likely a stray/grip contact.
  Worth watching, but it did not affect the session.

**Why it works.** Xiaomi's probe reads sysinfo after reset #1 and then resets the chip
again before its final `a0/00/01`; our old driver went straight from the `0xa6` frame read
to sense-on. `0125` adds one reset after sysinfo (`cepheustouchreset2=1`). The earlier
fallback knobs (`reset2=2`, `poll=0`, `sysinfo=0`) are no longer needed.

**Default changed.** `0126` enables touch by default (`cepheustouch=1`) and leaves the
delay at its existing 20 s default, per the user's choice; `cepheustouch=0` still turns
it off. This timing (init ~20 s after the FTS thread starts, i.e. before display-on) has
not been booted yet. Next test: boot with no touch args and confirm `sysinfo ...
afe=03/03/03`, `reset 2 ready err=0`, and a working first touch; if it fails, retry with
`cepheustouchdelay=55`. The sysinfo line prints `res=2339x1079` (x/y swapped or a
field-layout mistake) even though the coordinate decode is correct, so check `si[..]`
before changing it. Separately, restore `wdt_arm(30)` and drop `cepheuswdt=0` once
bring-up is finished.

### TWRP grip test is clean; XNU fails even with a clean baseline (2026-11-09)

- TWRP grip reproduction (`logs/runs/twrp-touch/events-grip.txt`,
  `dmesg-grip.txt`): the phone was held normally with fingers wrapping the edges
  and the centre was swiped for ~10 s. Contacts reached x=8/1061 and y=94/2274;
  the driver logged only the normal `Mutual negative detect Force cal`, with no
  `43 05 30`, no water mode and no lost events.
- `touch-on5` (same kernel as touch-on4, delay 55): the phone was flat and
  untouched at sense-on (64.64 s); the first touch at 68.30 s was a single
  centre circle. It still produced six contacts -- x=1079 at y=176/1347/2221,
  x=0 at y=1754, x=18 at y=176 and (305,969) -- then `43 05 30` at 68.89 s and
  complete silence until the manual reboot at ~150 s. Baseline contamination at
  `a0/00/01` is ruled out.
- Difference in the vendor probe: TWRP reads sysinfo after reset #1 and then
  performs **two more resets** before its final `a0/00/01`; our driver went
  straight from the `0xa6` frame read to sense-on. `0125` adds
  `cepheustouchreset2=N` (default 1) extra resets after sysinfo, logs
  `cfg/cx/afe/proto/res/tx/rx`, and adds `cepheustouchsysinfo=0` (skip `0xa6`)
  and `cepheustouchpoll=0` (IRQ-only) for no-rebuild comparisons.

**Result.** Built by the user and booted as `touch-on6`; touch works, see the section at
the top. The extra reset after sysinfo is the fix; the fallback knobs were not needed.

### Touch runs touch-on2..on4: chip answers, mutual sensing fails, water mode silences it (2026-11-09)

The bit-bang transport and the `fts_521` command/event layer work: the chip resets, reports
`ready`, returns sysinfo (`fw=0060 chip0=3639`), ACKs `a0/00/01`, and delivers 8-byte events.
But the event data is invalid, and after ~0.3 s of "MS invalid" the chip enters water mode
and stops reporting touches.

- `touch-on2` (delay 30, warm boot, kernel start 440 s): display on 480.2 s, touch init
  470.2 s (before display-on), sense on 470.25 s. First events 473.8 s: **ten** simultaneous
  `enter` contacts on panel edges. `43 05 30` (MS invalid mutual strength) at 474.08,
  `43 06 01` water enter 474.20, `43 06 00` exit 474.37. No further events.
- `touch-on3` (delay 100): the driver was scheduled at 109.5 s but the XNU log ended at
  103 s; the user's swipes happened before init. Inconclusive.
- `touch-on4` (delay 55, cold boot, kernel start 9.5 s): display on 53.06 s, touch init
  64.55 s (**after** display-on), sense on 64.66 s. Burst at 65.52 s: (58,176), (43,1754),
  (40,1479). Burst at 68.60-68.69 s: `x=1079` at `y=0/170/1754/2182` plus `y~=176` at
  `x=0/20/300/436/575/717` and single points elsewhere. `43 05 30` 68.96, water
  69.07-69.29, then **no events at all** until the log ends at 129 s. Zero FIFO read errors.
  Re-initialising after display-on therefore does not fix it.

The contacts sit on complete panel rows and columns, which is not a hand grip: it is what a
failed mutual-capacitance measurement looks like, and the chip's own status event says the
same (`MS invalid Mutual Strength soft Force cal`). TWRP on the same phone is clean: one
finger at (498,1243), a smooth 213 px swipe, and only the normal
`Mutual negative detect Force cal` after touch-down; no water events.

**Ruled out today**

- Touchmode/grip setup is not the difference: `fts_read_touchmode_data` only reads
  (`0xc1 05` / `0xc1 08`) and `fts_init_touchmode_data` only sets host-side defaults.
- The resume path is exactly `setScanMode(SCAN_MODE_ACTIVE, 0x01)` (`a0/00/01`); the real
  driver sends nothing else at screen-on, only logs `0x43` status events, and does not act
  on water mode either.
- Panel/AFE config: TWRP reads `AFE VER: CFG = 03 - CX = 03 - PANEL = 03`; the chip loads
  this from its own flash, so our reset should expose the same values.
- Power: `ldoc1` (AVDD 1.8 V) and the GPIO-84 `touch_vddio_vreg` (VDDIO) are referenced by
  the FTS node only, not shared with the display. Our order matches `fts_chip_powercycle`:
  reset held low, VDDIO, 4 ms, AVDD, 20 ms, reset high. Pre-init dump shows both rails off,
  so touch-on4 was a real cold power-up of the chip.
- Pin config matches `pmx_ts_active` (reset 12 + IRQ 122: gpio, pull-up, 8 mA).
- FIFO stack read (1 event + `event[7]&0x1f` more, 8 bytes each) matches
  `fts_event_handler`; coordinate decode matches `fts_enter_pointer_event_handler` exactly.

**Next test (no rebuild):** reproduce the edge-contact pattern in TWRP while the working
driver runs. Hold the phone the way it is held during use (fingers wrapping the edges) and
swipe the centre. If TWRP also logs `43 05 30`/water and stops reporting, the trigger is
contact handling; if TWRP stays clean, the chip is in a different state under XNU and the
next step is to log `si[28..31]` AFE/protocol, `si[84..85]` TX/RX plus a FIFO/IRQ heartbeat,
and try a full `fts_chip_powercycle` at init (`cepheustouchpc=1`).

### SE17 FIFO is disabled in hardware; touch moved to bit-banged I2C (2026-11-09)

**Phone run `touch-on1` (kernel built from the GENI stack).** SpringBoard booted with the
touch-enabled kernel; touch stayed dead and the log stopped at SE17's FIFO-disable check:

```
[  74.630] SWEET FTS: se17 fw=0x303 proto=3 status=0 tx=0x20102964 rx=0x20104900 ios=0x3 qup=0x10030000 depth=16/16
[  74.630] SWEET FTS: need ABL-loaded I2C firmware and 32-bit FIFOs; stopping
```

All other SE registers were valid. TWRP's debug log
(`/sys/kernel/debug/ipc_logging/i2c-2/log`) says `i2c in GSI ONLY mode`: on cepheus
`GENI_IF_FIFO_DISABLE_RO` (0x64) reads 0x64 and the QUP runs I2C through GPI DMA, not the
legacy FIFO. Porting GSI rings, DMA descriptors and IOMMU/firmware work to XNU for one slow
bus is not practical, so the FIFO path is abandoned. The GENI driver never reached the
chip: the FIFO check stops before the first transfer.

**Decision: bit-bang the same two pins.** `0085` keeps the FTS power/reset code and
replaces the transport with a software I2C master on GPIO 55/56 (qup17, south TLMM at
0x03d00000); the command/event layer follows Xiaomi's cepheus `fts_521` driver (`0x86`
event read, `0xa0/00/01` sense-on, `0xa6/00/00` system info, event `0x03`
controller-ready), not mainline `stmfts`. Details:

- `cepheustouch=1` starts after `cepheustouchdelay` seconds; `cepheustouchlog=1` logs every event.
- 19.2 MHz `mach_absolute_time` ticks, 4 us setup, 2 us low / 3 us high (~150-200 kHz),
  START/repeated START/STOP, ACK/NACK, SCL clock stretching (10 ms) and 9-clock recovery.
- Probes SDA=55/SCL=56 first, then the swapped pair; a wrong order only NACKs and a reset
  between attempts leaves the chip in a known state.
- Reset GPIO 12 is **active low** (Xiaomi's cepheus driver drives it low for 10 ms, then
  high; the live DT's flag 0 is not a polarity). Reset and IRQ 122 use the DT active state:
  function gpio, pull-up, 8 mA. SDA/SCL use the qup17 sleep state: pull-up, 2 mA.
- VDDIO is GPIO 84 (`touch_vddio_vreg`, active high, 4 ms), AVDD is `ldoc1` at 1.8 V.
- IRQ is polled: read the event stack when GPIO 122 is low, plus once every 10 ms; after
  10 consecutive read errors the bus is recovered and a release is sent for held fingers.

**Patch stack.** `0085` was regenerated with the 774-line `bsd/dev/sweet_fts.c` (blob
`8954729db921f60ff4476884435887bc2a5dad9f`); the obsolete `sweet_fts.c` GCC-halt hunk was
removed from `0086`. Host `clang -fsyntax-only` (kernel stubs, `-Wall -Wextra`) passed for
`0085`, and `tools/validate-repo.py` passes for 0001-0126. The stack has since been built
and booted on the phone (`touch-on2`..`touch-on6` above); `0125` made touch work on
`touch-on6`, and `0126` makes it the default.

**Boot setup used.** The successful `touch-on4` and `touch-on6` boots used the standard
debug arguments plus:

```
cepheuswdt=0 cepheustouch=1 cepheustouchdelay=55 cepheustouchlog=1
```

The transport is confirmed in the log: `SWEET FTS: ready try 1 sda=55 scl=56`, then
`sysinfo fw=0060 chip0=3639`, then `sense on err=0`; events arrive and decode into the
expected `enter`/`motion`/`leave` shapes. If the chip never ACKs on a future change, check
the idle levels printed before START (`idle=1/1`), the SDA/SCL order, and whether GPIO 84
really powers `touch_vddio_vreg`.

**Open.** The transport and the `fts_521` command/event layer work; the mutual-sensing
fault was fixed by the post-sysinfo reset in `0125` and verified in `touch-on6` (see the
top section). Still open: decide whether `cepheustouch` should default to 1, and restore
`wdt_arm(30)` in `shim/shim.c` / drop `cepheuswdt=0` after bring-up.

### Superseded: GENI I2C driver (2026-11-09)

The earlier 666-line GENI driver (SE17 at 0xc80000, wrapper 0xcc0000) is kept in patch
history only. Its SE register reads were valid, but `touch-on1` shows the FIFO interface
is hardware-disabled; the wrapper/FIFO-depth work is moot for the current transport.

### Hardware read from live phone (DONE 2026-11-05)

Read from the Xiaomi Mi 9 (cepheus) via adb (root). All key hardware facts are now in NOTES.md:

- **Display**: Samsung EA8076 FHD CMD, 1080x2340 @60 Hz, command mode
- **Splash FB**: cont_splash_region 0x9c000000/0x2400000; the VIG0 source register says the panel scans out 0x9c000000 itself (flame's FB is at 0x9c400000 — copying that number shifted the UI down 4 MB)
- **Touch**: ST FTS on I2C 0xc80000, addr 0x49, IRQ 122, x=1080 y=2340
- **Memory**: banks 0x80000000/0x3cc00000, 0xc0000000/0x80000000, 0x1_4000_0000/0xc0000000 (~6 GB)
- **Most SoC addresses identical to flame** (GIC, PMIC, USB, UFS, WDT, MDP, GPU)
- **Keys**: GPIO keys (vol_up code 115, ai_key code 689) — needs GPIO keys driver

### 1. Update remaining TBD values in code

Files still needing cepheus-specific addresses:
- `xnu/patches/0083-cepheus-display-keys-watchdog.patch` — WDT address confirmed (0x17c10000), but
  keys and touch drivers need rework (GPIO keys vs PMIC keys, I2C touch vs SPI touch)
- `xnu/patches/0085-cepheus-fts-touch.patch` — complete rewrite needed for I2C ST FTS at 0xc80000
- `xnu/patches/0088-cepheus-usb.patch` — USB PHY addresses confirmed same as flame
- `shim/sde.c` — panel comment updated (Samsung EA8076), register offsets same as flame

### 2. Build first test kernel and shim

```sh
python3 tools/mkadt.py work/boot/cepheus-hi.adt
tools/build-xnu.sh work/build-0083.log
cp work/dxb/build/xnu.obj/DEVELOPMENT_ARM64_SWEET/kernel.development.sweet work/boot/kernel-0083
# build shim + payload + boot image
# fastboot boot work/boot/shim-boot-0083.img
```

### 3. Boot and read logs from TWRP

```sh
fastboot boot downloads/twrp/twrp-...-cepheus.img
tools/cepheus-log-pull.sh shim logs/boot-0083.txt
tools/cepheus-log-pull.sh xnu logs/boot-0083-xnu.bin
```

## Where things stand

**Initial skeleton created.** Project structure copied from ios-flame with all flame→cepheus renames.
Hardware facts confirmed from live FDT. Most SoC addresses are identical to flame (same SM8150).
Key remaining work: touch driver (I2C not SPI), keys driver (GPIO not PMIC), and verifying
Xiaomi ABL boot image rules.

## Milestones (to be filled in as work progresses)

Each milestone entry should include:
- Date
- Kernel number and boot args
- Exact results (what worked, what broke)
- What is still open

Format (copy from ios-flame/HANDOFF.md):

```
### Milestone title (YYYY-MM-DD, HH:MM) - kernel NNNN
- **What worked:** ...
- **What broke:** ...
- **Open:** ...
```
