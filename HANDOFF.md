# Handoff: iOS on Xiaomi Mi 9 (cepheus)

Last updated **2026-11-13** (newest work first under "Start here"). Read this first, then [NOTES.md](NOTES.md)
(hardware facts, test loop, bootloader rules).

## Start here (next session)

### `0136` confirmed end-to-end; warm-boot touch fix in `0137` (2026-11-13)

**`0136` brightness result (`brightness-dcs4`, cold boot).** The user cold-booted to
fastboot and booted `work/boot/kernel-brightness/kernel.development.sweet` with
`cepheuswdt=0 cepheustouchlog=1 sweetbl=1 sweetfb_sel_log=1`. Touch initialized cleanly
(`sense on err=0`, no early `43 05 30/31`) and the Control Center slider changed the panel
brightness.

The DCS path is proven: the display-open write `brightness = 1` / `backlight-level = 0`
sent level 2 (`DCS 0x51 level 0x002 ... int 0x00000501`), and the first slider moves sent
`572 -> 0x013 (19)`, `857 -> 0x01c (28)`, `1428 -> 0x02e (46)`, `3856 -> 0x07a (122)`,
`4427 -> 0x08c (140)`, `4569 -> 0x090 (144)`, `4712 -> 0x095 (149)`. These match
`2 + value * (0x7ff - 2) / 0x10000`; `int` bit 0 (`0x501`) shows command-done, so no
further DCS/FIFO debugging is needed. The first eight sends are logged; later slider
values still update the panel even though they are not printed.

**Warm-boot touch failure.** All three `0136` runs started from TWRP via
`fastboot boot` failed touch: an early `43 05 31` or `43 05 30` force-cal event, a burst of
edge contacts, then silence while the driver stayed alive. A diagnostic run
(`touch-warm1`) delayed FTS to `cepheustouchdelay=50`, so sense-on happened at 59.7 s,
10 s *after* `SWEET FB: display on` at 49.9 s; it still failed (`43 05 30` at 63.5 s, then
water mode). Calibration order is therefore not the cause: the controller starts from a
bad analog/power state on warm boots, and a reset alone does not clear it.

**`0137` touch (written, not yet built).** Both touch rails are dedicated to the
controller: VDDIO is `touch_vddio_vreg` (GPIO 84) and AVDD is PM8150L L1
(`rpmh-regulator-ldoc1`, 1.8 V). At init the driver now asserts reset, disables AVDD through
`sweet_rpmh_write("ldoc1", ...)`, drives VDDIO low, waits 100 ms, then powers AVDD, VDDIO
and reset back up before the first probe. If an early `43 05 30/31` or water-mode enter
arrives within 5 s of sense-on, it repeats the power cycle plus sense-on (at most twice).
`cepheustouchpwr=0` keeps the old power-up and `cepheustouchrecover=0` disables the
recovery, for A/B runs. Build `0137` and test a **warm** boot first (TWRP -> bootloader ->
`fastboot boot`), then a cold boot; the expected new line is
`SWEET FTS: rails cycled, avdd ldoc1 err=0`, followed by a normal `sense on` and no early
force-cal event.

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
