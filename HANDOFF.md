# Handoff: iOS on Xiaomi Mi 9 (cepheus)

Last updated **2026-11-21** (newest work first under "Start here"). Read this first, then [NOTES.md](NOTES.md)
(hardware facts, test loop, bootloader rules).

## Start here (next session)

### Edge force-P: 0158 written, 0154/0156 were dead code (2026-11-21)

The running 0156 kernel selects the **edge** scheduler (`sched_string="edge"`),
not `sched_amp`: `sched_prim.h` picks `sched_edge_dispatch` whenever
`__AMP__ && CONFIG_SCHED_EDGE`, and `proc_reg.h` defines `CONFIG_SCHED_EDGE`
for `XNU_TARGET_OS_OSX`. The force-P block in `recommended_pset_type()`
(0154/0156) is therefore never called, and `pcore_set` is still NULL because
`sched_amp_pset_init()` never ran. That explains the 447 placement samples:
all `pset 0 ptype 1` (E), even with `sweetampforcep=1`.

`0158-cepheus-edge-force-p.patch` puts the policy in
`sched_edge_thread_preferred_cluster()`: with `sweetampforcep=1`, threads of
app types 1 (interactive daemon), 2 (standard daemon) and 5 (app) whose
`base_pri > BASEPRI_UTILITY` prefer the P pset. It finds the P pset by
scanning `pset_array[]` for `PSET_AMP_P`, because `pcore_set` is NULL under
edge. Utility/background priorities still go to E. `0159` adds `pri` and
`app` to the `SMP` sampler lines for verification.

Both patches apply cleanly in sequence after 0157 on the reconstructed tree
(`0154`-`0159`), and `tools/validate-repo.py` passes. Committed as `8cbdf0b`;
not pushed, not built, not booted.

At handoff the phone did not answer ping or SSH on `192.168.7.2`; it is
probably powered off or the USB route was lost. A cold power-on is needed
before the next test.

Next: push `main` (the xnu workflow triggers on `xnu/patches/**`), download
the `kernel.development.sweet` artifact, and `fastboot boot` it with the AMP
ADT and the `apps-0156-amp` cmdline (which already has `sweetampforcep=1`).
On the boot, kill `wifid` before ~165 s, then sample backboardd/SpringBoard:
look for `pset 1 ptype 2` and `pri > 20`. Then test Photos, Notes, Safari,
Calculator and the battery, and ask the user whether the UI feels more
responsive and how the heat is.

### Sandbox extensions: 0157 written for Photos (2026-11-21)

Photos' TCC failure is the sandbox stub, not TCC. With the original `tccd`
(no panic, no patched binary) the log shows tccd granting
`kTCCServicePhotos` via `com.apple.private.tcc.allow`, then
`Failed to issue sandbox extension for class: com.apple.tcc.kTCCServicePhotos`
and `kTCCErrorDomain Code=6`; the client preflight returns 0 and
`PHPhotoLibrary` fails with Code=45002. The SWEET stub returned ENOENT for
sandbox op 5 (extension issue), so the whole grant aborted.

The iOS 16.7.16 ABI came from `libsystem_sandbox.dylib` in the SystemCryptex
dyld shared cache (`dyld_shared_cache_arm64.25`, export trie/linkedit in
`.42.dyldlinkedit`), not from the iOS 26 b2l overlay:

- op 4 container path (still ENOENT);
- op 5 issue: 56 bytes {class, type, resource, flags, token buffer, target
  pid, target version}; the caller allocates 0x800 bytes and expects a
  NUL-terminated token;
- op 6 consume: 24 bytes {token, length including NUL, handle out}; the
  wrapper only rejects the literal string "invalid";
- op 7 release: no-op;
- op 0x43 container-expected: two 64-bit out pointers, interpreted as
  booleans; zero means false.

`0157-cepheus-sandbox-extensions.patch` implements these: issue writes
`sweet-<pid>` into the caller's buffer, consume returns handle 1, release
succeeds, and op 0x43 reports false. No sandbox is enforced. Committed as
`0f13540`; applies cleanly after 0114 on the reconstructed tree; not built or
booted.

Do not overwrite a running binary in place. Both kernel panics followed
`cepheus-files.py put` onto the mapped `tccd` (the second even after restoring
the original). Kill the daemon, wait for it to exit, then replace; the
original `tccd` is back and hash-verified (`53ca5d57...`), running as pid 37.

At handoff the phone is on `apps-0156-amp` with the USB route (`ssh -i
work/userland/ssh/id_ed25519 root@192.168.7.2`), up ~35-40 s after boot;
`wifid` must be killed before ~165 s or the Wi-Fi join deletes the USB route.
`ps` works; `cat`, `head` and `uptime` do not.

Next: build 0157 (push to main triggers the xnu workflow), `fastboot boot` the
new image, cold power-on (a warm PSCI reboot does not restore touch), then test
Photos, Notes and Safari with the panel awake, Calculator, and the battery.
Also check where 0156 force-P puts `backboardd`/SpringBoard and ask the user
whether the UI feels more responsive.

### Apps/battery: SpringBoard queue, Calculator staged, 0155 written (2026-11-21)

A later boot of the 0154 AMP kernel with `cpumask=0xF1` (no
`sweetampforcep=1`) was far laggier than the 0152-series `cpumask=0xF1` boot:
`cpumask` leaves only CPU0 as E, and AMP's default policy puts the interactive
daemons there while CPUs 4-7 idle. Do not combine the AMP kernel with
`cpumask` unless force-P is on. The phone was switched back to the 0152-series
`cpumask=0xF1` image (`work/boot/runs/apps-cpumask-good.img`), which booted
faster and restored the known-good responsiveness. A warm PSCI reboot does not
restore touch (the panel needs a cold power-on), so the cold-run image still
needs the user's power cycle.

The battery sample is real: raw MSOC 150 -> 59%, 3.879 V,
-165 mA, charger state 3. `IsCharging=No` is correct because the source is
weaker than the load. The brief 100% came from the first-probe fallback in
`pollCepheus()` (and the legacy `poll()`), which published 100%/full before the
gauge was ready; a transient SPMI failure also cleared `fHaveSample` and
republished it. `0155-cepheus-battery-no-false-full.patch` makes both paths
publish a neutral 50% estimated state and keep the last sample on a transient
failure. Written, applies cleanly; not built or booted.

SpringBoard's FrontBoard launch queue wedges after a while: every `openapp`
then fails with `BSServiceConnectionErrorDomain Code=3` after 5 s, while
already-running apps still answer. `kill <SpringBoard pid>` fixes it; Clock,
Safari and Photos all launched afterwards. This was the cause of the failed
launches; the black-screen rendering of apps that do launch still needs a
screen-awake retest.

The panel is asleep: `IOMFBBrightnessLevel=0`, `brightness-nits=0`; `/dev/pmem`
dumps are black with stale scanline artifacts. Remote `openapp` does not wake
the display. Wake the phone before judging any rendering.

Calculator: the migrator removed the 4-file placeholder container. The full
bundle is staged at `/private/var/staged_system_apps/Calculator.app` (binary
mode 755) and `/private/var/preferences/com.apple.demo.SADForceInstall.plist`
lists `com.apple.calculator`. The IPSW's own staged bundle has the same ad-hoc
CDHash and a detached signature at
`/System/Library/AppSignatures/com.apple.calculator`; ad-hoc signing is normal
here and is not by itself the blocker. A plain `lsreg` of the bundle at
`/Applications` still returns 0 even with the placeholder gone, so it must be
installed, not just registered. Live `runmigrator` cannot complete the install:
with the MobileInstallation SPI entitlement it aborts in
`-[MIDaemonConfiguration init]` with `NOT_CODESIGNED` (ad-hoc binary, no daemon
container). The boot-time SystemAppMigrator, running as the system
`migrationpluginwrapper`, is the expected path to install it on the next boot.

Safari network is healthy: `nettest google.com https://www.google.com/` gives
DNS, TCP, and HTTP 200; WebKit logs show TLS 1.3/QUIC and resource loads with no
network error. The remaining Safari problem is UI/rendering and needs the
screen awake to reproduce.

`lsreg -r` and `lsreg -u` wedge `lsd`; do not use them. Restarting `lsd`
recovers. Even a plain register can block while `lsd` is wedged.

Next: wake the phone and retest Photos/Safari; reconnect USB in fastboot for a
`cpumask=0xF1` boot (and to let the boot migrator install Calculator). Build
0155 with the next kernel.

### UI: AMP boots all eight CPUs but schedules nothing on P (2026-11-21)

The 0153 kernel booted with the `--amp` ADT (`work/boot/cepheus-amp.adt`,
E on CPUs 0-3, P on 4-7) and `cpus=8`, no `cpumask`. The kernel ring shows
`SWEET CPU: 8/8 CPUs initialized`; P CPUs 4-7 (mpidr 0x400-0x700) all came
alive with PSCI rc=0. No panic; the run ended at t~1687 after 720 s.

Placement: 40 sampler rounds, 1,796 valid `SMP` lines, all `pset 0 ptype 1`
(E/A55); zero `pset 1`/`ptype 2` (P/A76). The `cpu` field never shows
4-7. The A76s are online but idle.

Frame rate: the real scroll at t=1182-1258 (16 buckets) averaged 22.4 fps
(min 10.0, max 41.6); an earlier t=897-902 burst read 46-51 fps but was
transient. `SWEET GPU: 5s:` still shows ~4% GPU busy with backboardd
taking the CPU time, so the frame is still CPU-bound in backboardd.
`cpumask=0xF1` remains better (36-38 steady, 50+ bursts).

Cause: AMP's perfcontrol recommendation marks interactive daemons as E,
so backboardd (an interactive daemon) never leaves the A55s. The P cluster
is healthy; the policy is the problem. `0154-cepheus-amp-force-p.patch`
(now in `xnu/patches/`) adds `sweetampforcep=1`: it forces
`TASK_APPTYPE_DAEMON_INTERACTIVE` and `TASK_APPTYPE_APP_DEFAULT` threads
to P after the utility/background checks, so backboardd/SpringBoard can
use the A76s while background/utility work stays on E. The arg defaults
off. Next: push `main`, dispatch the `xnu` workflow, download the kernel
artifact, and boot with `-A work/boot/cepheus-amp.adt` plus
`sweetampforcep=1` (no `-m`). If that still does not use P, revert to
`cpumask=0xF1`.

Logs: `logs/runs/place-amp/` (`live.txt`, `xnu.txt` 13,409 lines,
`shim.txt`, `rescue.txt` empty). The run script died at the 720 s mark
before the automatic rescue; the probe shim + TWRP were run manually.

### UI: cpumask placement run gained ~50%; shim thread still on A55 (2026-11-21)

`place-smp` booted the 0152-series kernel with `cpus=8` and the 20/20/60
sampler; the phone was rebooted manually at t~600, so samples 0-25 were
captured. The scroll buckets at t=494-594 read 17-40 fps, mostly 22-25,
consistent with the known ~28 fps lag.

Placement during the scroll: `backboardd` had 23-31 threads. The worker
group with backboardd stacks (`t7`-`t12`) sat mostly on logical CPUs 0-3
(A55) in every scroll sample; `t11` stayed on cpu6 (P). The only thread
sampled with a non-shared-cache PC was `t5`, whose `lr` is in
`libvulkan_freedreno`, on cpu2 (A55) in sample 22. `SpringBoard` was also
E-heavy. All lines read `pset 0 ptype 0` (no AMP). This is consistent
with, but not proof of, the placement hypothesis: the sampler may mostly
return each thread's saved syscall state, so the sampled CPU is the last
CPU, not necessarily the frame-critical one.

Result: `cpumask=0xF1` booted the same 0152-series kernel with five
logical CPUs (boot A55 + four A76). The phone returned to TWRP cleanly
after ~550 s of uptime; no panic. Clean scroll buckets at t=408-423 read
36.8, 37.4, 37.8, 38.2 fps; t=448-468 read 51.6, 32.2, 33.2, 50.8, 33.4
fps (the 50+ buckets have more swaps than panel copies, so the presented
rate is ~46 fps). Baseline was 22-25 fps, so placement is confirmed as
part of the cost, but not the whole gap. `backboardd` moved from 15-21
threads on A55 to 19-25 on A76 (2-8 left on the boot A55). The only
thread caught executing SweetMetal2 code during the scroll (`t5`, PC
`SM+0xade4b4`) was on cpu0, the remaining A55; if that is the shim's
encode/submit thread it is the next serial cost.

### UI: scheduler-placement test is staged, not run (2026-11-21)

The next boot experiment is CPU placement, not another frequency or
vsync test. Three kernel patches are written and apply cleanly on top of
the series (all three are untested):

- `0151-cepheus-cpumask-without-cpus.patch` lets SWEET use `cpumask=`
  without `cpus=` (they cannot be combined). `cpumask=0xF1` selects the
  boot A55 plus all four A76/prime as five logical CPUs.
- `0152-cepheus-sampler-cpu-pset.patch` adds `cpu N pset N ptype N` to
  every `sweetsample` `SMP` line, from `thread->last_processor`, and
  adds `sweetsamplestart`/`sweetsampleperiod`/`sweetsamplecount`
  (defaults 90/120/40 s) so a short test can sample every 20 s.
- `0153-cepheus-amp-e-p-clusters.patch` turns on `__ARM_AMP__` and
  `MAX_CPU_CLUSTERS=2` for SWEET and panics early if the ADT lacks an E
  or P cluster. It is the intervention, not part of the diagnostic, so
  it is held in `xnu/pending/` and the CI series stops at 0152 until the
  cpumask result is in. Move it to `xnu/patches/` for the AMP build.

`tools/mkadt.py --amp` now emits `cluster-type 'E'` for CPUs 0-3 and
`'P'` for 4-7 (values 69/80); without it every CPU stays type 0. The
default tree is byte-identical to before apart from the random seed.
`tools/cepheus-run.sh` takes `-m CPUMASK` (replaces `cpus=8`) and
`-A ADT` (default `work/boot/cepheus-hi.adt`).

Order: first boot the staged 0151+0152 SMP kernel with the usual
`cpus=8` and `sweetsamplestart=20 sweetsampleperiod=20
sweetsamplecount=60`, then run the full-frame scroll. If backboardd's
render threads are already on logical CPUs 4-7 (P), placement is not
the cause: skip the cpumask and AMP boots. If they run on 0-3 (A55),
boot the same kernel with `cpumask=0xF1` (one A55 + four P) and compare
the fps. Only if that changes the frame rate, build 0153 and boot the
`--amp` ADT. Bit 0 of the cpumask must stay set; disabling the boot CPU
still sets `cpu_config_correct=false` and panics later.

Commands (the CI series must end at 0152 for the first two):

```
tools/cepheus-run.sh place-smp -G -s 900 \
  -a "sweetmtl=2 sweetusb=2 cepheustouchpanel=0 cepheustouchpanelpor=0 \
      sweetsamplestart=20 sweetsampleperiod=20 sweetsamplecount=60"
tools/cepheus-run.sh place-cpumask -G -s 900 -m 0xF1 \
  -a "sweetmtl=2 sweetusb=2 cepheustouchpanel=0 cepheustouchpanelpor=0 \
      sweetsamplestart=20 sweetsampleperiod=20 sweetsamplecount=60"
python3 tools/mkadt.py work/boot/cepheus-amp.adt --highmem --amp
tools/cepheus-run.sh place-amp -G -s 900 -A work/boot/cepheus-amp.adt \
  -a "sweetmtl=2 sweetusb=2 cepheustouchpanel=0 cepheustouchpanelpor=0 \
      sweetsamplestart=20 sweetsampleperiod=20 sweetsamplecount=60"
```

The sampler prints `SMP name[pid] tN cpu C pset P ptype T ...`; on the
SMP boots `pset` is 0 and `cpu` is the logical ID (0-3 A55, 4-7 A76),
while AMP should put user-interactive threads in `pset` 1 (`ptype` 2,
P) and kernel/background work in `pset` 0 (`ptype` 1, E).

The existing `metal2` telemetry already argues against a frontend stall:
`J scheduled wait` 54 ms over 4370 commits, `J completed wait` 26 ms
over 2, `J resource wait` 0. The GPU was 31-37% busy while backboardd
used >100% CPU, so the 27-28 ms frame is CPU work in backboardd, not the
shim waiting on the GPU. Core type/placement is therefore the remaining
non-Rust lever, but it is still a hypothesis until the sampler shows
where those threads actually run.

Open: the phone is charged to 100% and the 0150 battery backend reports
real FG percentage/amperage and the charging flag on the PC port. The
0152-series SMP kernel is booted; the cpumask run is in flight. There is
still no local kernel build environment. Pending from earlier:
`sweetgpufreq=5`, Reduce Motion, a longer pstack, and the clock/timezone
and Wi-Fi status-bar checks.

### UI: 0056 is overridden; frame cost is shim throughput (2026-11-21)

The exact image boots with `sweetfb_hz=60`, so 0056's throttle is
disabled: `fFlushTicks = 1` and the latest pending frame is presented at
every vsync tick. Panel
copies track swaps (233 copies/247 swaps in the 49.4 fps bucket;
137/141 at 28.2 fps), and the kernel's `avg copy` is 0.52-0.65 ms,
about 1.7% of one CPU at 28 fps. 0056 is not the frame-pacing cause; do
not spend a boot on it.

Full-frame cadence is not vsync-locked. At t=2168.752-2169.058 eleven
`rp12 dr56` frames commit every 27-28 ms (about 36 fps instantaneous),
with ctc 10.6-11.2 ms; the 5 s buckets over the same scroll read 29-34
fps. The 49.4 fps bucket is not evidence of a cool full frame: the
nearby t=142 rows are a light `rp1 dr13-15` scene at 17-19 ms, and the
bucket that follows is diluted by idle. CPU clusters are at their top
table entries (A55 1785, A76 2419, prime 2841 MHz) and `sweetddr` votes
are on by default, so frequency is not the missing lever.

At 27-28 ms per full frame, the 12.6 ms age and 10.6 ms ctc are close
to additive, but age includes caller idle; that is consistent with
serialization, not proof. The cheapest boot-arg experiment left is
`sweetgpufreq=5` (585 vs 499 MHz), estimated at ~1-1.5 ms of ctc; it
needs a cold boot. Shaving the CPU side means changing the shim.

Replay logging is structurally on: `Rebinds::any()` is `live > 0` (any
buffer over host pages), and `open_render` creates a `RenderLog` for
every pass while it is true. In the installed binary, `set_viewport` at
+0x267d70 tests the log Vec at encoder+0xd58; when present it allocates
a 48-byte box and appends a 40-byte call per viewport. That is a real
per-call cost, but the 90 SweetMetal2 pstack samples are spread over
~70 functions and do not concentrate in this block, so its share is
unmeasured. `phone/tools/pstack2.c` reads the Vec fields at runtime; it
is written but not built (`work/userland/bin/ios-cc` is absent) and has
not run.

### UI: exact metal2 image is still ~28 fps; the lag is longstanding (2026-11-20)

Re-booted `work/boot/runs/metal2.img` (the exact 2026-11-19 image whose
log showed 58 fps; SHA-256 starts `6d70ec73`) on the current rootfs. A
1200-step 12 s vertical swipe at t=971 held **28.0-28.8 fps** for the
first 10 s (the page then stopped moving), 136-144 presents per 5 s. In
Preferences, launch plus a swipe gave **29.2-34.0 fps**. Backboardd
telemetry showed about 3 submits per presented frame, commits 23-29 ms
apart, commit-to-complete 10-11 ms, and the GPU 31-37% busy. `psx` during
a long swipe read backboardd 115.6%, Preferences 8.8%, SpringBoard 5.2%.
This rules out 0148-vs-0150, the 0149/0150 battery backend, `sweetbl=0`,
and `sweetrtcoff` as the frame-pacing cause. The user confirms it is not
a regression: "It isn't a regression, i always saw it and feel it
laggy". The old 58 fps buckets alternate with ~23 fps and contain no
`SWEET CMD` swipe records, so they were an idle animation, not touch
interaction. Treat ~28 fps as the longstanding baseline.

Also verified: `/System/Library/Extensions/SweetMetal2.bundle/{SweetMetal2,
libvulkan_freedreno.dylib}` are byte-identical to `phone/metal2/out`
(SHA-256 `74df7865...` and `441a16d4...`), so the `nm`/pstack symbols
match the running build.

The early **49.4 fps** bucket (t=84.7) is real interaction: 247 swaps
during 3.7 injected home-screen swipes. The next bucket's 33.6 fps is
diluted by ~2 s with no swipes, so it is not a 5 s collapse. GPU work
was 1.07 ms/submit early versus 3.5-3.9 ms later, which fits a lighter,
still-loading home screen as well as throttling. Do not treat 49 fps as
the baseline until the same scene is measured early and late.

Next: charge the pack and do a cold boot. The 0056 question is closed
(above). For one free test, add `sweetgpufreq=5` to the boot args and
compare the same full-frame scroll at ~15 s and ~10 min; Reduce Motion
is the only other no-rebuild setting worth a try. The thermal A/B and
the Rust install stay deferred. No non-Rust switch was found for the
shim's per-pass/per-call encode; the only substantial fix is in the
shim (Rust), which is not buildable here yet.

### UI: transparency is not the cost; per-pass encode is the suspect (2026-11-20)

Set `com.apple.Accessibility ReduceTransparencyEnabled=1`, respringed
(backboardd 1261), and repeated the Preferences scroll: **25.8 fps**
versus 29-34 fps before, so blur/backdrop is not the frame-pacing cause.
With transparency off the draws per commit grew from 56 to 140-147 while
the pass count stayed 12, and GPU time per submit rose to 4.4 ms. The
setting was put back to 0 and backboardd respringed again (1350).

Backboardd's own telemetry shows one command buffer per frame. A full
frame is `rp12 dr56 kpx8182` (12 passes, 56 draws, 8.18 Mpixels) and a
lighter frame is `rp1 dr37 kpx2676`. Commits are 23-29 ms apart, one per
frame. Creation-to-commit age is 12.6 ms for the full frame and 5.2 ms
for the light one; commit-to-complete is ~10.6 ms and ~8.4 ms. The 7.4 ms
age difference over 11 more passes and 19 more draws was fitted as
roughly **0.5 ms per render pass plus 0.1 ms per draw**, but the fit is
not unique and age includes caller idle. The K rows qualify it: a
`rp1 dr37` frame's ctc is 8.4 ms versus 10.6 ms for `rp12 dr56`, so the
GPU-side cost of the extra 11 passes and 19 draws is only 2.2 ms. The
per-pass CPU cost is still a hypothesis; measure the encode directly
before treating it as the lever.

The J row in the 3202-commit block logs 8800 flushes, 72.3 GB, 3.89 s of
CPU (8.2 MB and 1.2 ms per commit, 2.75 per commit): `flushSurfaces`
runs at commit and again at finish, and flushes every IOSurface render
target whether the CPU wrote it or not.

Blocked on driver work: this Mac has no `rustc`/`cargo`/`rustup`, and
`work/m2v-bridge/` and `work/turnip/build-ios/` are gone, so neither
SweetMetal2 nor Turnip can be rebuilt as-is. Profiling further or
testing a fix needs the Rust 1.98 toolchain + `aarch64-apple-ios` target
(both the bridge and Turnip builds also need the local iOS clang setup).

### Battery: 0150 booted; vmd + dasd/coreduetd cause the heat (2026-11-20)

Booted `work/boot/runs/battery-0150/battery-0150.img` (kernel SHA-256
`bc6bc2b9...`; disassembly confirms it contains 0150's `ma > 0` and
`percent >= 99` gates). 0150 works: on the PC port the FG reads
`RawIBATT` +308...+1260 (`InstantAmperage` -150...-615 mA) and
`IsCharging=No` with `ExternalConnected=Yes`. `FullyCharged=No`.

Heat/drain: iOS was running ~4.1 cores and drawing ~590 mA (2.2 W) at
boot. Two loops:

- `vmd` (VisualVoicemail.framework, no modem) at 51-90% CPU, repeatedly
  hitting its 90 s/180 s CPU limit (`vmd.cpu_resource-*.ips`).
- `dasd` asks `coreduetd` for `_DKPredictionQuery` predictions in a
  tight loop (oslog: `[Storage] Starting executeQuery ... 'dasd'`);
  `coreduetd` used 80-100% of a core, `dasd` ~7%, and ReportCrash wrote
  the vmd reports.
- `aggregated` also peaked at ~100% early; it settled.

Runtime test: `kill -9 <vmd>` dropped total CPU from ~410% to ~127%
(current 586 mA -> ~150 mA); `kill -9 <dasd>` dropped it to ~10% and
~110 mA at 3.86 V. vmd stayed dead, but `dasd` is re-launched on demand
(PID changed, coreduetd resumed at 100%), so a one-shot kill is not
enough.

Persistent fix installed (user approved): both entries were removed
from `/System/Library/xpc/launchd.plist` (532 -> 530 jobs) via
`tools/cepheus-files.py`; the pre-change copy is
`work/daemons/launchd.plist.before` (gitignored). This disables Visual
Voicemail and background task scheduling; both are useless without the
modem. If launchd rescans `/System/Library/LaunchDaemons` and starts
them again, the two per-job plists need the same treatment.

Next image is ready: `work/boot/runs/daemons-off/daemons-off.img`
(sha256 `649b5568...`), cmdline includes `sweetrtcoff=1554650334`.
Boot it after the battery has charged and check `psx` for vmd/dasd,
`coreduetd` CPU, current, and that the clock starts at 2026 rather than
1977.

RTC: the boot still starts in 1977 and jumps when userspace sets the
clock. The kernel logged offset `1554650334` (RTC 240555155 vs time of
day 1795205489); saved in `work/rtc-offset` (gitignored) for the next
cmdline. It did not stop coreduetd's loop, but it avoids the jump.

Battery was down to 27% (raw MSOC 69, 3.1 V loaded) on the PC port; the
port cannot charge this phone. Watchdog ended the boot at ~21:27.

### UI: daemons-off boot is cool, but frames still present slowly (2026-11-20)

Booted `work/boot/runs/daemons-off/daemons-off.img` (kernel 0150,
`sweetrtcoff=1554650334`). The daemon fix held: no `vmd` or `dasd`, and
`coreduetd`/`aggregated` stayed near idle. The clock started in 2026.
Battery read 37% at 3.146 V, `ExternalConnected=Yes`,
`IsCharging=No`, about 149 mA on the PC port.

The UI is still sluggish. During the boot animation the 5 s buckets
peaked at **14.4 fps**; opening Preferences gave **7.8 fps** (39 swaps,
38 panel copies, 697 us average copy). The GPU was not starved: at
93 s it submitted 281 frames in 5 s (56/s), 20% busy, longest 13.5 ms,
while backboardd presented only 87 times (17/s). So rendering is fast
but presentation is paced/blocked.

The fake `AppleARMBacklight` is active on this boot (`sweetbl` default
1): `SWEET BL: setProperties backlight-level = 8128`, then DCS `0x51`
at display-on. The earlier `metal2` run also published
`AppleARMBacklight` (at 15.5 s), but its **58 fps** buckets were an idle
animation (no `SWEET CMD` swipe records), not touch interaction; see the
exact-image recheck above. `sweetbl=0`, the 0149/0150 battery backend,
and `sweetrtcoff` are therefore all ruled out as the pacing cause.

The `sweetbl=0` control (`work/boot/runs/daemons-off-bl0/daemons-off-bl0.img`,
SHA-256 starts `a16280078f`) booted at 22:06 with **no `SWEET BL` lines
at all**, so the fake backlight was completely absent. It is still slow:
the boot buckets ran 14-26 fps, Safari launched at 21.6 fps, and twelve
injected short swipes plus three 1.5 s vertical swipes held **25-26 fps**.
The GPU was not the limit: it presented 125 frames in 5 s while the
renderer submitted 250-340, present calls took ~1 ms, and the GPU was
3-6% busy. Total CPU was ~15% (`kernel_task` 11%); `vmd`/`dasd` stayed
gone. So the fake backlight is **not** the frame-pacing cause.

The exact `metal2` image that reached 58 fps on 2026-11-19 was booted
again at 22:13 on the current rootfs (`logs/runs/metal2-recheck/`) to
separate a 0149/0150 regression from a phone-state change. A 0148
kernel with the current cmdline is also staged at
`work/boot/runs/daemons-off-bl0-0148/daemons-off-bl0-0148.img`
(SHA-256 starts `cdb7393e`). The pack was last seen at 37%; the PC port
does not charge it.

### Battery: 0149 booted; 0150 fixes the charging flag (2026-11-20)

Booted `work/boot/runs/battery-0149.img` with 0123+0148+0149. The
PM8150B backend probed successfully: `SweetBatteryFallback=No`,
`SocReady=1`, `MaxCapacity=3300`, and the FG reported a real, falling
SOC (95 -> 87, 37% -> 34%) with `VBATT` 3.13 -> 3.10 V and net
`InstantAmperage` about -370 mA while on the PC USB port. TWRP's 99%
was a stale Nov 18 reading; 3.79 V at 35% is plausible.

The bug: `IsCharging` came from SMB5 state 3 (CC) alone, so SpringBoard
showed CHARGING while the pack drained. A PC port supplies less than the
phone draws. Patch `0150-cepheus-battery-charging-direction.patch`
requires `currentValid && ma > 0` before setting `IsCharging`, stops
forcing 100% when `BATTERY_CHARGER_STATUS_1` is 5 (gated on
`percent >= 99`), and applies the same current-direction rule to the
PM6150 path.

Also confirmed: `BATT_INFO` temp registers 0x4150/0x4151 read 0, so
`Temperature` is the estimated 25.0 C (`SweetBatteryTemperatureEstimated=Yes`);
TWRP's gen4 path read 18.5 C from SRAM. Real temperature still needs the
FG SRAM access path.

Boot ended at 1121 s when the watchdog (`cepheusreboot=1200`) took the
phone down; no panic. Battery was ~34% and discharging, so wall-charge
before the next cold boot.

### Battery: cepheus PM8150B FG-gen4 backend written as patch 0149 (2026-11-20)

The live battery is still the static fallback (`SweetBatteryFallback=Yes`,
`CurrentCapacity/MaxCapacity=5020/5020`, external, not charging). The
boot cmdline has no `sweetbattery=1`, but that is not the root cause:
`SweetBattery` only implements the PM6150 QG path, and cepheus is a
PM8150B with FG-gen4. Patch `0149-cepheus-fg-gen4-battery.patch` adds a
read-only cepheus backend:

- `SweetBattery::start()` probes SPMI USID 2 for the FG-gen4 subtypes
  (`BATT_SOC` 0x10 at 0x4005, `BATT_INFO` 0x11 at 0x4105). On cepheus it
  selects the new backend automatically; no boot arg is needed.
  `cepheusbattery=0` disables it and keeps the static fallback for A/B.
- SOC is the fuel gauge's own monotonic value (0x4009, matching copy at
  0x400a), scaled 0/255 to 0/100 with the vendor formula, not a voltage
  curve. Voltage is 0x41a0/0x41a1 (little-endian, x122070/1000 uV) and
  current 0x41a2/0x41a3 (little-endian signed, x488281/1000 uA, sign
  flipped to the `IOPMPowerSource` positive-while-charging convention).
  Both use the vendor `_CP` shadow copies for the torn-read check.
- Charger state comes from SMB5: `BATTERY_CHARGER_STATUS_1` 0x1006
  (1-4 charging, 5 terminated/full), power path 0x110b, USB plugin
  0x1310, and the error/JEITA gates at 0x1007/0x100d.
- `sweet_spmi_read()` now accepts USID 2 peripherals 0x10-0x13 and
  0x40/0x41; every access is still an observer read, no PMIC writes.
- Cepheus' cell is 3300 mAh (`batterydata-F1-*-3300mah`), so the backend
  publishes that as the design capacity instead of sweet's 5020.
- Temperature comes from `BATT_INFO` 0x4150/0x4151. The gen3 driver
  documents this block as 0.25 K/LSB while gen4's SRAM copy is Celsius,
  so the decoder accepts a plausible reading from either encoding and
  falls back to 25.0 C marked estimated.
- Bring-up properties: `SweetBatteryRawMSOC`, `RawVBATT`, `RawIBATT`,
  `RawTemp`, `SocReady`, `ChargerState`.

Patch applies cleanly after 0062+0099 (the only earlier patches that
touch these files) and was round-tripped against a reconstructed
post-0099 tree. It was subsequently built into
`work/boot/runs/battery-0149.img` and booted; see the 0150 section above
for the result. The SPMI observer does expose USID 2's FG/charger APIDs.

### WiFi: keepalive and fallback both verified on a cold boot (2026-11-20)

Cold-booted `work/boot/runs/wifi12-cold.img` with the new `wland`
(231,072 bytes, commit `d527dd1`, keepalive + fallback). There is no
`/var/local/wlan.conf`, so the network was joined manually from Settings.
The join completed 33.7 s after `wland` started and the radio reached
`state 5`.

Keepalive: `radio: keepalive: 1 sent` at 93.7 s, exactly 60 s after the
join. The counter logs only every fifth frame, so one line is expected.
The link stayed `state 5` through 300.9 s associated, past the 231 s point
where the AP had previously sent its inactivity deauth; there was no
deauth or link-loss event and RX/TX kept moving. That is the keepalive
fix working.

Fallback: sent `SIGUSR1` to `wland` at 338.6 s. It logged
`join: link lost (asked)` and `bridge: link down`, and `wifid` did not
rejoin. At 358.6 s `wland` logged
`bridge: wifid has not rejoined in 20 s: joining again`, rejoined at
359.0 s, and logged `bridge: fallback join joined`. The radio returned to
`state 5`, `en0` kept its DHCP lease, and `nettest` got an HTTPS `200`
from apple.com. So the fallback path works and does not fight `wifid`
when `wifid` acts first.

Status-bar icon: the fallback link-down/up made it white (the initial
join alone left it grey), so the colour was a stale status update, not
DHCP or the `IEEE80211` service type. A respring then showed one of the
three signal arcs, so the remaining problem is the signal level. The
Apple80211 link event carries the right dBm (`rssi=-54`) but leaves
`snr`/`noise` at zero, and `wifid`'s `GET NOISE` (type 17) returns
`EOPNOTSUPP`; `phone/wifi/radio.c` derives RSSI as firmware SNR over a
`-95` dBm floor, so patch 0148 fills `snr = rssi + 95`, sets
`noise = -95` in the link event, and implements `GET NOISE` with the
same 52-byte layout as `GET RSSI`; `sta_state()` also refreshes the RSSI from the
BSS wland is hearing, instead of the join-time copy. Not built yet. SpringBoard still
lacks `com.apple.private.corewifi.bssid`/`...wifi-info` and its
`GET BSSID` is refused, but that is not what greys the icon. Next:
build/boot with 0148, join manually, and check for three arcs; in the
live `wifid` log, expect `LinkChangeStatus ... rssi=-54 noise=-95` and
`GET NOISE ... return 0`, not `-3903`. Also open: auto-join (a manual
Settings join is still needed after each boot), the `wland` prefetch
fix, and the toybox `<sys/disk.h>` build failure.

### WiFi: killing configd took L3 down; cold boot recovered it (2026-11-20)

To test whether the grey status-bar icon was caused by the active Wi-Fi
service being typed `Ethernet`, `CB3347B7.../Interface/Type` in
`preferences.plist` was changed to `IEEE80211` and configd (`pid 35`) was
killed with `SIGTERM` to apply it. That was a mistake: the USB gadget stayed
enumerated, but the phone sent no packets at all, lost its USB and LAN
addresses, and answered neither IPv6 link-local pings nor the kernel
command server on UDP 5141. configd did not come back on its own.

A cold boot of `work/boot/runs/wifi12-cold.img` restored it. `configd` is
`pid 35` again, USB `ue0` has its address back (service
`5EE7C0DE...`), and configd rewrote the plist
(1752 bytes) keeping `CB3347B7.../Interface/Type = IEEE80211`, so the new
service definition parses.

After a manual join from Settings, configd selected a DHCP service
(`4120C2A2...`), and `en0` got a `/24` lease from
the router, with the router's name servers. SSH to that address works, and
`nettest` got an HTTPS `HTTP 200` from apple.com. So the
`Type=IEEE80211` service change fixed the automatic DHCP/primary-service
selection; if only the manual service was keeping the address before, that
is now gone.

The status-bar icon was still grey at this point; the newer section above
has the follow-up. `wifid` does send SpringBoard the network-name,
access-point, link-change and join-status events. SpringBoard's request
for the access point's address is refused with `PROCESS NOT ALLOWED FOR
REQUEST TYPE` (`Preferences` is allowed), but that denial is not what
keeps the icon grey.

Do not kill configd to apply a preferences change. Reboot, or use
`scutil`/`prefs` so configd owns the update. The new `wland`
(231,072 bytes, SHA-256 `0c7cfa6f...`, with `radio: keepalive` and
`bridge: fallback join`) is installed; it takes effect on the next boot.

Open: the signal-level/icon work above and whether the DHCP service
persists across a link drop/rejoin. Keepalive and fallback are verified.

### SSH: restored; dash had a weak strchrnul import (2026-11-20)

SSH is back on the phone over USB. `.github/workflows/ssh.yml` builds dropbear
2024.86, dash 0.5.12 (toybox still best-effort) and `phone/tools` for iOS 16
arm64; `tools/cepheus-install-ssh.sh` installs the artifact over the file server.

The first working dropbear exposed two bugs:

- dropbear 2024.86 has no `-E` (that is OpenSSH); with syslog off it already logs
  to stderr. The job is now `dropbear -F -e -R -p 22`; `-e` passes the job's PATH
  (including `/var/local/bin`) to the login shell.
- dash's configure found `strchrnul` in the iOS SDK, but iOS 16 only has it as a
  weak import, so the first call jumped to address 0 (`CRASH dash ... pc 0x0`).
  The workflow now configures `ac_cv_func_strchrnul=no` (dash's own fallback in
  `system.c`) and fails the build if any weak imports remain. Rebuilt dash:
  SHA-256 `80348063...`, no weak imports, `_strchrnul` defined locally.

Installed over the phone's USB file-server address; `/bin/sh` is a symlink to
the new dash, and the installer now removes it before relinking so upgrades
work. Verified with `ssh -i work/userland/ssh/id_ed25519` to the phone's USB
address.

The root image is very minimal: `/bin/ps` and `/bin/df` exist, but there is no
`ls`, `cat`, `kill`, `sleep`, `date`, `launchctl` or `chmod`; toybox would fill
that gap but still fails on `<sys/disk.h>`. Guard it with `__APPLE__` and
`!defined(__ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__)` to use the generic
stub.

Open: SSH is over USB only for now; Wi-Fi is up but not joined because
`/var/local/wlan.conf` is missing (see the Wi-Fi section).

### WiFi: Settings join works; internet and SSH over Wi-Fi (2026-11-20)

Cold boot `wifi12-cold.img` (`cepheusreboot=1200 sweetmtl=2 sweetusb=2
cepheustouchpanel=0 cepheustouchpanelpor=0`) with SSH installed. `wland`
brought up the modem/WLAN firmware (`FW READY`, `mission mode on`, HTC/WMI)
and scanned every ~15 s. Joining `<ssid>` from the Settings app then
worked through the Apple80211 path:

```
bridge: join '<ssid>' (PMK) -> 0
radio: join: auth reply status 0
radio: join: assoc reply status 0 aid 19
radio: *** joined '<ssid>' ***
bridge: link up '<ssid>'
radio: stats: state 5, tx 2995 ... rx 3437 ...
```

`en0` became the primary interface with the manual service `CB3347B7...`
(a `/24` address, the router, and the configured name servers).
`nettest` on the phone resolved `apple.com`, connected to TCP 80 and got
`HTTP 200`; `ssh -i work/userland/ssh/id_ed25519` to the phone's LAN
address also works. So Wi-Fi is fully usable at L3, and no
`wlan.conf` is needed when Settings supplies the credentials.

At the time, open: automatic DHCP. `preferences.plist` still had two DHCP
services (`E2805781...`, `65C6796F...`) plus the active manual
`CB3347B7...`, and no `PrimaryService`/`ServiceOrder`; configd kept the
manual service. This was later fixed by the `Type=IEEE80211` change in the
newer section. The status bar icon was still grey then; the newer section
has the signal-level follow-up. The `Preferences` crash/16.7.16 Apple80211
layouts are unchanged.

### WiFi: L2 join works; DHCP waits on configd; cold boot hit a TFTP stall (2026-11-20)

`wifi7` (cold boot, race-uploaded `wland`) got the modem and WLAN firmware up: Settings
scanned 8-17 networks and a join to `<ssid>` completed at L2:

```
radio: join: auth reply status 0
radio: join: assoc reply status 0 aid 19
radio: *** joined '<ssid>' ***
bridge: link up
```

The kernel answered `wifid`'s `ASSOCIATE`/`ASSOCIATE_RESULT`/`CURRENT_NETWORK` gets, but
`radio: stats: state 5, tx 4` showed only the four EAPOL frames: no DHCP discover/request
and no ARP reached the radio. The IP side is configd's job on `en0` (bridge.c does not use
`ipnet.c` when `/dev/wlan` is present).

`preferences.plist` was the 192-byte host-names-only file with no `NetworkServices`, and
removing it plus rebooting (`wifi8`/`wifi9`) only recreated it before `en0` existed. `wifi10`
wrote a crafted service instead (`en0`, `Hardware=AirPort`, `Type=IEEE80211`, DHCP) and
configd accepted it: it added its own `en0` service (`Type=Ethernet`, DHCP) and
`Interface/en0/AirPort/JoinMode=Automatic`. `wifid` still did not auto-join (the 5 GHz
network and `Enable5GhzAutoJoinScan=false`), so the next test needs a tap in Settings.

`wifi11` was the first full power cycle after that. It never reached WLFW: the 4 MB
`wlanmdsp.mbn` TFTP read stalled after block ~20, and at 42.6 s the modem logged
`smp2p: modem state 7 ERR-FATAL ready handover`; the radio stayed at `state 0`, so Settings
had no Wi-Fi at all. `wlan7` shows the same transfer succeeding on another cold boot
(`read done (3960 blocks ... 6482 ms waiting)`), so this is a cold-storage timing failure,
not a config error. Fix in the tree: `tftp_init()` now reads `wlanmdsp.mbn` once before
registering the TFTP service (`phone/wifi/tftp.c`, commit `c5b40f6`); for the immediate
boot `work/wifi12-watch.py` also fetches the file over the USB file server as soon as it
answers (4.3 s for 4 MB), before `wland` starts.

Open after DHCP works: the `Preferences` crash (`wifid sets 383 (?): REFUSED`,
`CURRENT_NETWORK` 1164 bytes), the 16.7.16 Apple80211 layouts (`ASSOCIATE_RESULT` currently
returns `0 bytes`), and whether configd receives the `en0` link event.

### WiFi: modem ERR-FATAL was the RMTFS buffer size (2026-11-20)

`wifi5` (patched MPSS constants) got past PAS: `auth and reset -> 0/0`, `modem ready
after 100 ms`, GLINK/QRTR up, services `0x42/0xb401` and `0x2b/0x1202`, RMTFS opened
`modem_fs1`, `modem_fs2`, `modem_fsg`, pd-mapper answered `tms/pdr_enabled` and
`tms/pddump_disabled` with 0 domains, then
`rmtfs: alloc 3145728 bytes for 1 -> 107aa5000` at [4168] and
`[4211] smp2p: modem state 7 ERR-FATAL ready handover` 43 ms later.

Root cause: `phone/wifi/rmtfs.c` had flame's 2 MB `BUF_SIZE`; the cepheus modem asks
for 3 MB. The stock DT confirms it: `/soc/qcom,rmtfs_sharedmem@0` has
`reg = <0 0x300000>` (3 MB). The daemon's `size <= BUF_SIZE` check failed, so the
modem got QMI error 3 for its allocation and asserted. The pd-mapper 0-domain replies
are not the cause: flame's table is identical and its modem stayed up, and those
services are not in either `.jsn` file.

Fix (in the working tree): `BUF_SIZE 0x300000u` in `phone/wifi/rmtfs.c`; the
`/var/local/fw/rmtfs/.buffer` record now includes the size, so a stale 2 MB mapping is
not reused. For the immediate test the constants were binary-patched in
`phone/wifi/out/wland` (`0x200000 -> 0x300000` at file offsets `0xa0c8`, `0xa0f4`,
`0xa380`, `0xa578`) and the binary re-signed ad hoc; `wland.pre-rmtfs` keeps the
pre-fix copy. The patched binary is installed at `/var/local/bin/wland` (SHA-256
`a74209b16168da0183dc6daab2dcc4f8d0884b398ada74cbd635531b34ae7075`), `.buffer` was
removed, and `wifi6` booted with
`-G -k logs/runs/touch-0148-quiet/kernel -s 900 -a "sweetmtl=2 sweetusb=2 cepheustouchpanel=0 cepheustouchpanelpor=0"`.

`wifi6` result: the 3 MB alloc now succeeds (`hyp_assign 107a45000+300000`), the modem
reads `modemst1.img`/`modemst2.img` sector 1+1, then asks for a **single 3 MB iovec**
(`2 7a45400 17fe`, i.e. `fsg` sector 2 + 6142 sectors) and fails at
`rmtfs: iovec out of the buffer (2 7a45400 17fe)`: the source's `tmp[BUF_SIZE]` bounce
buffer is still 2 MB in the patched binary, so the `bytes > sizeof(tmp)` check rejects
it. The modem err-fatals 35 ms later. This is the last missing piece: a real rebuild
with `BUF_SIZE=3 MB` grows `tmp` and makes the check pass. The binary patch cannot do
that safely (`_iovec.tmp` is exactly 2 MB and `_xf` follows it in BSS).

Next: dispatch the `wlan` workflow with the `rmtfs.c` change, put the artifact at
`phone/wifi/out/wland`, reinstall, and boot `wifi7`. Then the modem should finish the
EFS reads, start the WLAN PD, publish WLFW `0x45`, and load `wlanmdsp.mbn`.

The earlier `boot: mem setup 8dc00000+9600000` line is only the printf/compile-time
copy; the SMC literal is patched to `0x8e000000` and auth passed. A real rebuild will
make all copies consistent.

### WiFi: PAS auth failed on flame's modem region; stock cepheus DT has the real one (2026-11-20)

`wifi3` and `wifi4` proved the launchd job works: `exec wland` at 20.0 s, SMMU
`0x640/0x1 -> CB5 ... ok`, all WLAN rails up, QMP clock/image acks, the 77,508,765
bytes of modem segments loaded, then
`boot: auth and reset -> 4291821497/0` (`0xFFCFFFB9`) and wland exits. `sweetusblog=0`
in `wifi4` changed nothing, so the failure is not SMC interruption noise.

Root cause: `phone/wifi/wland.c` carried flame's `MPSS_PA=0x8dc00000` /
`MPSS_SIZE=0x0b400000`, but the stock cepheus DTB (the four appended DTBs in
`downloads/cepheus/boot_unpacked/kernel`) says
`/reserved-memory/modem_region = 0x8e000000 + 0x9600000`, with
`mss@4080000: qcom,pas-id = <4>`, `qcom,firmware-name = "modem"` and
`memory-region` pointing at it. The image is relocatable and `wland` writes the
segments at their absolute `p_paddr` (0x8e000000…); the last segment ends exactly at
0x97600000 and the segment sum is exactly the 77,508,765 bytes in the log. TrustZone
was therefore told the image starts 2 MB lower than where it was written, so its hash
check failed.

Fix (in the working tree): `MPSS_PA=0x8e000000`, `MPSS_SIZE=0x09600000` in
`phone/wifi/wland.c`. For the immediate test the two constants were also binary-patched
in `phone/wifi/out/wland` (literal at file offset 0x143d8, `mov w19,#0xb400000` at
0x5da4) and the binary re-signed ad hoc; `wland.flame-base` keeps the old copy.
`wifi5` race-uploads this binary as soon as the kernel file server listens (~14 s,
before the 20 s launchd job). The result is in the `wifi5`/`wifi6` section above: PAS
now succeeds; the next blocker was RMTFS. The stock DT also confirms `icnss`'s
`iommus = <0x26 0x640 0x1>`, so `radio.c`'s hardcoded WLAN SMMU stream 0x640 is right
for cepheus.

### WiFi: the port is staged, wland is the last piece (2026-11-19)

The kernel side of ios-flame's WiFi (patch 0123, from ios-topaz/darwin-redwood) is
already in the test kernel and in the ADT: `logs/runs/metal3/xnu.txt` has
`cepheus wlan: /dev/wlan ready (major 20)` at 29.9 s and `exec wifid` at 32.3 s, and
`work/boot/cepheus-hi.adt` contains `cepheus,wcn3990`. `tools/mkadt.py` publishes that
`wlan` node because com.apple.wifid's `_LimitLoadToDeviceTree` needs it; the kernel's
`bsd/dev/cepheus_wlan.c` + `iokit/Kernel/arm/RedwoodWLAN.cpp` provide en0 and the
Apple80211 requests.

What is missing is userland: `phone/wifi` (wland, the WCN3990 driver from flame, via
`bsd/dev/cepheus_wlan.c`'s `/dev/wlan`) is not built or installed. The phone's own
modem partition confirms the same chip family as flame: `wlanmdsp.mbn` reports
`WLAN.HL.3.0.c2` (WCN3990) and ships 33 `bdwlan.*` variants plus `bdwlan.bin`. The
modem image (`modem.mdt` + bNN), `modemr.jsn`, `modemuw.jsn`, `wlanmdsp.mbn` and
`bdwlan-cepheus.bin` (= stock `bdwlan.bin`; `wlfw.c` looks for the cepheus name) are
extracted into `work/wifi/fw/modem/`, and the rmtfs partition copies (`modemst1`,
`modemst2`, `fsg`, `fsc`) into `work/wifi/fw/rmtfs/` — both gitignored, the rmtfs
images carry device identifiers and stay phone-only. `tools/cepheus-wifi-fw-extract.sh`
reproduces the extraction from TWRP.

New tooling (2026-11-19):
- `.github/workflows/wlan.yml` builds `phone/wifi` on macos-15 with the iPhoneOS SDK
  (`workflow_dispatch`, artifact `wland-ios16`); this Mac has no iOS SDK.
- `tools/cepheus-wifi-install.sh` uploads `wland` to `/var/local/bin`, the firmware to
  `/var/local/fw/modem`, the rmtfs images to `/var/local/fw/rmtfs`, an optional
  `--conf` file to `/var/local/wlan.conf` (mode 600, never in the repo), and adds the
  `org.iosonpixel.wland` launchd job (`wland auto`, once per boot, log
  `/var/local/wland.log`, off with `/var/local/wland.off` or `sweetwifi=0`).

Next: dispatch the wlan workflow, put the artifact at `phone/wifi/out/wland`, boot
iOS (the file server must be up), run the installer, reboot, and join the network from
Settings (cold boot has touch). Then watch `/var/local/wland.log` and record the result.
Open: the BDF choice (`bdwlan.bin` first; the bXX variants are untested), and the WLAN
SMMU stream 0x640 is flame's value — cepheus' stock DT was not checked.

### GPU: cold Metal boot, and real apps get A640 devices (2026-11-19)

Run `metal3` is the same command as `metal2` with `-s 240`. The phone had been
power-cycled in fastboot, so this was a cold boot and **touch works**: the
`cepheustouchpanel=0 cepheustouchpanelpor=0` args only skip the panel SET_ON replay and
keep VCI/reset powered (patch 0144); they do not disable the touchscreen. `SWEET FTS`
read `sysinfo fw=0060 chip0=3639 ... res=2339x1079` at 50.5 s and the UI accepted input,
which is the expected cold-boot behaviour from the touch work.

Devices this boot: backboardd 1062 ms, SpringBoard 2793 ms,
`com.apple.WebKit.GPU` 418 ms, MobileSafari 2163 ms, MobileSMS 993 ms, each with its own
`/var/local/m2v-cache/<process>`; chronod still excluded. GPU 206–300 submits per 5 s,
`0 missed`, busy 25–30 %, pool peak 306 MB, 2100 frames presented; `SWEET FB` drops to
0 swaps once the screen is idle (4 clients), so present is event-driven, not a fixed
60 fps loop. Safari and Messages were actually browsed on the GPU.

One bridge gap surfaced: `MTL2[342 MobileSMS]: MISSING NBCommandBuffer
-presentDrawable:`. Nothing else failed in this log.

The cold/warm shader-cache comparison was not made: the telemetry marker was removed to
cut the heat after `metal2`, so there are no `vkpipe` counters here, and the device-init
times above do not include Turnip's lazy pipeline compiles. Logs: `logs/runs/metal3/`
(`xnu.txt`, `shim.txt`, `mtl2-latest.log`).

### GPU: Metal draws SpringBoard on the A640 (2026-11-19)

**The CI-built Metal stack works on the phone.** Run `metal2`
(`tools/cepheus-run.sh metal2 -G -k logs/runs/touch-0148-quiet/kernel -s 900 -a
"sweetmtl=2 sweetusb=2 cepheustouchpanel=0 cepheustouchpanelpor=0"`; the existing
ABI v1 test kernel, no rebuild needed). `SweetMetal2.bundle` was installed in
`/System/Library/Extensions/` over the kernel file server (`tools/cepheus-files.py`,
TCP 2223; keep the on-phone copy and the local one in `phone/metal2/out/` in sync) and
the four GPU firmware files came from `research/gpu/fw/` (`-G` → `sweetgpu=1`).

Two on-phone files are required before the first boot:

- `/var/local/mtl2-clients` = `* -chronod` (the plugin gives backboardd a device
  always; SpringBoard must be listed or the unlock swipe does nothing; chronod stays
  on the CPU path, as on ios-flame);
- `/var/local/m2v-cache/` mode 777 (Turnip's shader cache; on this ticket `/var/local`
  itself was missing on the first boot, and backboardd's `mkdir` then failed with
  `PermissionDenied`, so the frontend refused the device and the UI silently stayed
  on the CPU).

`logs/runs/metal2/mtl2-latest.log` and `xnu.txt`:

- `MTL2[57 backboardd]: device Turnip Adreno (TM) 640 in 1142 ms
  (cache /var/local/m2v-cache/backboardd)`; SpringBoard's device in 2124 ms;
  both loaded `libvulkan_freedreno` (non-zero base) and adopted
  `MTLTextureImplementation`; `_MTLDevice is 528 bytes`;
- `chronod: not listed` as intended;
- kernel side: ~550 submits per 5 s, `0 missed`, GPU busy 19 % (7–41 %), mean submit
  1.7–4.2 ms, `present: 4500 frames, avg cache clean ~515 us, GPU blit ~13 us` —
  300 frames per 5 s = 60 fps, pool 85–97 MB;
- no `vkpipe` failure (`failed=0`), only one
  `VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT` warning; nothing in the
  plugin's problem list beyond the "no device answer" hints (gpuCoreCount,
  maxVertexAmplificationCount, …) that the frontend logs and falls back on.

Icon artwork and text render through Turnip (different from the CPU path in several
places); the phone warms up during the first boot because 71 pipelines ≈ 5.3 s of
on-phone compilation plus continuous 60 fps presenting. With `/var/local/m2v-cache`
populated the next boot is expected to be cooler; this was not measured yet.

The first Metal boot (`metal1`) is the negative control: backboardd was refused
(`ShaderCache ... PermissionDenied`) and SpringBoard/PosterBoard/chronod were all
"not listed", so the UI stayed on the CPU even though the plugin had loaded.

**Open:** (1) a USB-side `SWEET USB: busERR ... SMMU cb fsr 400 far 0x154fe6f0aaaba`
line appears every ~5 s in `metal2`; USB and the file server keep working and the GPU
path reports no fault, so this is tracked separately. (2) dropbear did not start this
boot (port 22 closed, `kex_exchange_identification` reset through FlameBridge); the
on-phone key was updated over the file server instead. Check
`/var/local/dropbear.log` on the next SSH-capable boot. (3) `FlameBridge` did not
pick up `tools/flamebridge/ports.txt` (only the 2222 default forward was listening);
set `CEPH_FILES_HOST` to the phone's USB address to bypass it meanwhile. The run was ended early
(`tools/cepheus-cmd.py reboot`) to stop the heat; the runner then does its rescue
boot and leaves the phone in TWRP.

### GPU: the Adreno 640 comes up and presents the panel (2026-11-19)

**First GPU run on cepheus, and it works.** `tools/cepheus-run.sh gpu1 -G
-k logs/runs/touch-0148-quiet/kernel -s 180 -a "cepheustouchpanel=0 cepheustouchpanelpor=0"`
booted the A640 path for the first time. The four firmware files came read-only from the
stock `/vendor/firmware` (TWRP) into `research/gpu/fw/` (gitignored): `a640_gmu.bin`,
`a630_sqe.fw`, `a640_zap.mdt`, `a640_zap.b02`.

`logs/runs/gpu1/xnu.txt`:

- power was off at entry (`cx_gdsc 222001 gx_gdsc 222001`); `cx gdsc -> f8222000 on`,
  GMU RCG `80000000 cfg 1`, `gpucc cx_gmu = 4221`;
- `gmu: 7 firmware blocks loaded`, `init result babeface BOOTED`; HFI start + perf vote
  499 MHz, OOB `gpu_set` acked, `gx gdsc f8222800`;
- `zap: auth and reset rc 0 r1 0`; both rings idle, `hwfault 0`;
- `bring-up done, GPU ready in 294 ms (pool 0x104a00000)`;
- the panel switched to 4352-byte rows and two 686-page surfaces were mapped;
- `5s: 300 submits (0 missed), GPU busy 321 ms (6%) ...; present 300/321ms` and
  `present: 600 frames, avg cache clean 617 us, GPU blit 16 us`;
- no SMMU fault, no GPU timeout, no fallback to the CPU copy.

SpringBoard booted with the image scaled correctly; the UI stays laggy because
`sweetgpu=1` only moves present/scale to the GPU while rendering is still on the CPU.
That run had touch disabled. The `AegirPoster` SIGABRT at 78.5 s is not new: the same
crash is in `brightness-on1` at 183.2 s.

**Still open: Metal.** `sweetmtl=2` needs SweetMetal2.bundle = `phone/metal2` + the
metal2vulkan Rust bridge + Turnip. None of them is built, and this Mac has no
`work/turnip`, `work/metal2vulkan` or `work/m2v-bridge`; it also has no Xcode/iPhoneOS
SDK (`xcode-select` is the Command Line Tools), no meson/ninja/cmake and no Rust/cargo.
Building the stack therefore needs the toolchain first: Xcode, Homebrew meson/ninja/cmake,
rustup 1.98 + the `aarch64-apple-ios` target, Mesa 85fda3b3 + UniXNU's patches and
metal2vulkan 720ef1ce. Until then the GPU result above is present-only.

**Build path chosen: GitHub Actions.** `.github/workflows/userland.yml` is a manual
`workflow_dispatch` job that installs the toolchain on `macos-15`, applies UniXNU's Mesa
patch series (pinned to UniXNU `8542980d`) and `phone/metal2/bridge/patches`, builds
Turnip, the bridge and `SweetMetal2.bundle`, and uploads `sweetmetal2-ios16` with the
bundle and `m2vtest`/`vktest`. All 24 Mesa patches and all 8 bridge patches apply cleanly
to the pins above (checked locally on 2026-11-19).

The first CI run (2026-11-19, Xcode 16.4 / iPhoneOS18.5 SDK) failed in Turnip's first
compile. The SDK ships CarbonCore `Endian.h`, and the runner's filesystem is
case-insensitive, so Mesa's `#include <endian.h>` resolved to it; it has no
`__BYTE_ORDER`, leaving `UTIL_ARCH_*` unset and hiding every `PIPE_FORMAT_*` alias.
`tools/build-turnip-ios.sh` now passes `-DUTIL_ARCH_LITTLE_ENDIAN=1`,
`-DUTIL_ARCH_BIG_ENDIAN=0`, `-DHAVE_DLFCN_H` and `-D_DARWIN_C_SOURCE` in the iOS cross
file; the workflow also probes the SDK and prints the configure/build logs on failure.
The rerun got past the SDK headers and failed in Turnip's first object: the pinned
backend (`tu_knl_darwin.cc`) names ABI v2's optional
`NativeAdrenoMethodSetPriority`/`NativeAdrenoMethodNote`, `NativeAdrenoAbiPriority`,
`NativeAdrenoAbiNote`, `NativeAdrenoNoteWords` and `NativeAdrenoParamsOwnedCoherent`,
while `phone/turnip/NativeAdrenoABI.h` and kernel patch 0115 still serve ABI v1.
The header now declares those v2-only names as optional hints but keeps the wire
version at 1 and 16 IBs per submit, so the existing v1 test kernel can be reused:
the kernel refuses priority/notes and Turnip treats them as hints. The next rerun
is pending. Unzip the artifact at the repository root so
`phone/metal2/out/SweetMetal2.bundle` lands where `tools/cepheus-metal2-install.sh`
expects it.

### Warm: TWRP's full order, a quiet calibration, and the early scan all fail (2026-11-18)

**TWRP's exact probe → DRM blank → unblank order is not enough.** `fts-twrpfull.img`
(`8ef4cadc…`, `SHIM_FTS_TWRPFULL=1`, `SHIM_FTS_LATE_TOUCH_MS=60000`) replays the whole TWRP
order in the late window: sense-on, 2 ms, sense-off, 50 ms, sense-on (`fts_init_sensing`),
300 ms, sense-off + `A2 03 20 00 00 01` + `A0 01` + `C0 02 00 00` (the DRM blank), 267 ms,
`panel_reinit()`, reset, sense-on. Every write returned 0 and the reset ready check passed.
The late window still reported only `43 05 01` + the sense-on echo and **0 touch events in
60 s** (`logs/runs/fts-twrpfull-warm.txt`). The blank-command order is not the difference.

**A quiet late calibration does not help.** `fts-calquiet.img` (`da6f9d61…`,
`SHIM_FTS_CALQUIET_MS=6000`) left the bus idle for 6 s after the late sense-on, then polled:
still 2 events / 0 touch events over 60 s (`logs/runs/fts-calquiet-warm.txt`). The early
scan in the same boot had already failed once (edge burst 3642-3706 ms, `43 05 30` at 3894,
`21` at 4001, `30` at 4126), so this does not by itself rule out polling as the trigger.

**The early polling scan fails too.** `fts-early60.img` (`166095b2…`, `FT_POLL_MS=60000`, no
display init yet) produced the same right-edge/top-row burst at 3661-3776 ms, `43 05 30` at
4258 ms, then silence (`logs/runs/fts-early60-warm.txt`). The failure is not specific to the
post-display bring-up. Note that the `fts-latetouch2` "clean" early window was a *quiet* scan
(no FIFO reads), so its 1 IRQ / 2 events mean the chip was silent, not that it calibrated.

**`43 05 30` is not the killer.** TWRP's own warm dmesg has the same status sequence:
`01` at 1.28-1.87 s, `21` at 2.50 s, `30` at 5.11 s after real taps at 4.95/5.02 s, and touch
then works (1340 samples at 148.5-184.0 s). Warm TWRP can emit `30` and keep scanning; the
shim's failure is the silence after it. The edge burst is the chip's response to contacts
(real grip/palm or injected), not a unique ghost signature.

### Warm control re-read; rescan, GPIO5 and panel drain negative (2026-11-18)

**TWRP gets the same status event and keeps working.** Reading the full warm TWRP control
instead of only its summary lines changes one conclusion: `43 05 30` is not what kills the
chip. `logs/runs/twrp-warm-now/dmesg.txt` is the same boot as
`logs/runs/twrp-now-touch2.txt` (the first captured input event at 148.498903 matches the
dmesg press at 148.498991): a press/release pair at 4.946/5.022 s is followed by
`MS invalid Mutual Strength soft Force cal = 30` at 5.108 s, and the capture then has 1340
`ABS_MT_POSITION_X` samples of coherent touch from 148.5 to 184.0 s. The current live warm
session is cleaner still: only `43 05 01` at sense-on, real presses at 61.4/116.4 s and
`43 05 31` at 163.8 s. So warm TWRP can emit `43 05 30` and keep scanning; the shim's
failure is the silence **after** the status, not the status itself. This corrects the
earlier "TWRP never reports `43 05 30`" note, which came from grepping the spaced hex form.

**Pre-emptive rescan leaves the chip silent.** `fts-rescan.img` (`05f55887...`,
`SHIM_FTS_LATE_RESET_MS=1000`, `SHIM_FTS_LATE_TOUCH_MS=60000`, `SHIM_FTS_RESET2=1`): full
late power-up, sysinfo, reset, sense-on, then a second reset at t=1005 ms right after the
first two events; ready/sysinfo/sense-on all OK. The remaining 59 s contain exactly
`43 05 01` + the sense-on echo and 2 IRQs - no right/top burst and no touch
(`logs/runs/fts-rescan-warm.txt`, first session). Resetting before the burst does not
condition the chip; it produces the same silent state as the dummy scan.

**PM8150B GPIO5 is not it.** `logs/runs/fts-gpio5-warm.txt` entry: `pmic 2 gpio5`
(PM8150B, SID 2) `5=11 10=0 40=0 41=0 42=4 43=0 45=1 46=80` - mode already 0, master
enable set. `SHIM_FTS_GPIO5OFF` clears `46` bit 7 before the FTS rails (`off mode 0->0 (0)
en 80->0 (0)`), and the run still produced 27 events, `43 05 31` and two `43 05 30` at
5213/5451 ms. The always-on `vdd_boost_vreg` is exonerated with the other PMIC GPIOs.

**A 30 s panel drain is not it either.** `logs/runs/fts-drain-warm.txt`: before FTS
power-up the shim held VCI off, DDIC reset low and RPMh L14 off for 30 s (`panel drain
30000 ms, vci=0 rst=0 ldoa14 off=0`), then brought both back. The ghost still fired at
877-897 ms and 3755-4002 ms, with two `43 05 30` at 4272/4423 ms. DDIC/POR residue on the
panel rails is not the source.

**Nov 9 recheck.** The saved `touch-on6` kernel (`logs/runs/touch-on6-recheck`, same
kernel and `cmdline`, no shim log) booted SpringBoard warm and reported zero touch through
repeated swipes and a display off/on. The 11-09 `touch-on6`/`on7` runs were therefore
almost certainly cold-boot successes; the old "warm touch works" claim does not hold.

**Next.** Dump all four TLMM tiles at shim entry, warm vs cold, before touching anything:
the shim has only ever compared its own pins (12/21/55/56/84/122). The unexamined pins are
the fingerprint `goodix` reset 37 (north), IRQ 120 (south) and `fp-gpio-pwr` 125 (west), the
IDT wireless-charger interrupt 90 / enable 104 (north), NFC 41/42 (north), 47 (east) and
113 (west), and the rest of the 176 pads. If those match, the remaining always-on domains
are GCC/dispcc, AOP/RSC, or the QUP-vs-bit-bang I2C runtime.

**TLMM dump, first attempt.** `-DSHIM_FTS_TLMM_ALL` with a save before every read
(`logs/runs/fts-tlmm3-warm.txt`): `tlmm probe 0 at 3d00000`, `--- saved (tlmm) at 7706 ms`,
then nothing. The first read of **pad 0** at `0x03d00000` stalls; the phone resets ~10 s later
(not the 120 s watchdog). TWRP's live `/sys/kernel/debug/gpio` enumerates only pads **4..125
and 130..174**, i.e. 0-3 and 126-129 have no usable GPIO register, so the dump now skips those
eight (`fts-tlmm5.img`, `a8f2b053…`, `tools/cepheus-shim-cold-run.sh`). A cold run of the
image before the skip was wired repeated the pad-0 death (UFS up at 37909 ms), so the cold
comparison is still owed. The same TWRP tree also gives a live warm pin snapshot in
`logs/runs/twrp-pinctrl-warm/` (`gpio`, `pinmux-pins`, `pins`; `pinconf-pins` is empty
because the downstream driver has no pinconf_get).

**Warm dump works.** `logs/runs/fts-tlmm5-warm.txt`: 167 pads in 3.3 s (`pre-tlmm` 7730 ms,
`tlmm done` 11020 ms), then the normal probe flow and `done, resetting` at 11899 ms. Warm
entry state of every pad of interest is the same default: touch 12/55/56/84/122, fingerprint
37/120/125, IDT 90/104 and NFC 41/42/47/113 are all `cfg 1` (pull-down, mux 0/gpio, 2 mA,
input, `io 0`). Only display VCI GPIO 21 is driven (`cfg 3c3`, 16 mA, output high).

**Cold dump matches exactly.** `logs/runs/fts-tlmm5-cold.txt` (full power-off, same image;
`pre-tlmm` 20044 ms because cold UFS init is slower, `tlmm done` 23144 ms): 167 pads, and
`tools/cepheus-tlmm-diff.py` reports **0 of 167 differ**, `io` included. The only difference
between the two dump blocks is the per-pad `saved (tlmm)` timestamps. No TLMM pad state is
retained across a warm reset. The remaining state axes are the always-on blocks
(GCC/dispcc/AOP/RSC) and the I2C runtime: TWRP uses QUP SE17 (GSI/DMA), while both the shim
and the XNU driver bit-bang the same two pads.

**The quiet early scan is not proof of a clean calibration.** `fts-latetouch2` warm and cold
both start with the same 6 s **quiet** early scan (`fts_test()` runs before
`ufs_init()`/`sde_probe()`, while ABL's splash is on): `quiet done 1 irqs`, drain is
`43 05 01` + `43 01 a0`, no `43 05 30`, `st=0 io=1`. But that scan never reads the FIFO, so
"1 IRQ / 2 events" means the chip was silent, not healthy. The early **polling** scan
(`fts-early60`, top) is what shows the warm chip failing ~3.6 s after sense-on, before the
shim's display path. The first divergence in the same-image pair is still the late window:
warm is 2 events / 1 IRQ over 60 s, cold is 1000 events / 2448 IRQs.

### NEGATIVE: the 4.2.14-only commands are dead code on firmware 0060 (2026-11-18)

**Result.** The driver cepheus actually builds (`drivers/input/touchscreen/st/fts.c`, ST
4.2.14, `USE_NOISE_PARAM`) has two paths TWRP's 5.2.4 copy lacks, but firmware `0060`
implements neither, so neither can explain warm boot:

- **Noise save/restore (`C8 01` read -> reset -> `C7 01 <4 bytes>`)**: the shim's
  `SHIM_FTS_NOISEPARAM` flag (`fts-noise.img`, `b9165c02…`) ran it in the 60 s late window.
  `late noise read err=-1` (no event), so the write was skipped; the scan then produced only
  `43 05 01`, 1 IRQ and no touch (warm). Direct TWRP `driver_test`: `01 C8 01` then
  `11 02 17 01 07 D0` -> `pollForEvent: ERROR 80000007` (2 s, zero events), both with scan
  on (`01 A0 00 01` first) and scan off; same for `C7 01 00 00 00 00`. Sysinfo still answers
  (`FW VER 0060`, `CONFIG VER 0037`, `CX VER FFFF`), so the chip is alive and the commands
  are simply unsupported. 4.2.14 also ignores `readNoiseParameters()`' return and only ever
  writes back its static zero buffer, so this path is inert even on Android.
- **Chip info / full init (`B8 FF FF` -> event `13 FF FF` -> `D0 8000` 164 bytes -> `0xA5`)**:
  `01 B8 FF FF` gets no event (1 s and 2 s polls), and `02 D0 80 00 00 A4 01` returns 164
  zero bytes. Without chip info the tuning-version and MP-flag preconditions for
  `FTS_CMD_FULL_INITIALIZATION 0xA5` cannot be met, so the full-init path can never start on
  this firmware. Do not port it.
- **Conclusion:** the earlier "LineageOS has nothing new" note stands. The remaining
  difference from TWRP is still its fuller boot order / display notifier / re-arm, not these
  commands.

### NEGATIVE: dummy scan + rail cycle silences the ghost but leaves touch dead (2026-11-18)

**Result.** A new flag `SHIM_FTS_DUMMYSCAN_MS=N` (`shim/ftstest.c`) does one throwaway
power-up/scan (`a0 00 01` for N ms, `a0 00 00`), drops the rails for 300 ms, then runs the
real bring-up. It removes the warm ghost, but the conditioned chip **does not report real
touch** — so it is not a fix. The "clean" run 2 of `fts-retry` was almost certainly the same
silent state; absence of ghost frames is not enough.

- Dose response (quiet 6 s scan, no finger): N=500 (`7d72a13c…`) ghosts; N=1500
  (`f38666d6…`) leaves one `43 05 30`; **N=2000/2500/3000 (`2ad4c54f…`/`cb9afecf…`/
  `cdf7541c…`) show only `43 05 01` + the sense-on echo**.
- Real-touch test, early blind window (`fts-dummy3-touch.img`, `cc9acab0…`, N=3000,
  `-DFT_POLL_MS=60000`, user swiped for 40 s): **0 touch frames, 2 status events**.
- New `SHIM_FTS_LATE_TOUCH_MS=N` runs a second bring-up **after** the display console is up
  and polls for N ms, so the person can see the screen. `fts-latetouch.img` (`8930f2d6…`,
  N=3000): 0 touch frames in 60 s, only 1 IRQ. `fts-latetouch2.img` (`93a12a56…`, adds
  `SHIM_FTS_RESET2=1`, XNU's post-sysinfo reset): same, 1 IRQ over 60 s.
- **Cold control, same image, full power-off (`logs/runs/fts-latetouch2-cold.txt`): real
  touch works.** 2448 IRQs, 1000 logged events, continuous mid-screen `report`/`move`/
  `leave` frames (e.g. x=771 y=937; x=740 y=883; x=530 y=1014) for the whole 60 s window.
  The shim's I2C/event path is fine; the warm chip is what is broken.
- The cold log also shows `pon spare 0 -> 4`, same as warm, so PON spare does not
  distinguish the two boots.

**Conclusion.** Rail cycling a warm chip only stops it reporting; the underlying warm-boot
fault survives. Any candidate fix must be validated with **real touch**, not just a quiet
FIFO. TWRP's Linux driver still gets real touch on a warm boot, so the fault is recoverable
in software; the missing piece is in TWRP's fuller init (lockdown/CRC probe, display
notifier, or re-arm), not in the rail state.

### Post-sysinfo reset replayed from the shim: same ghost (2026-11-18)

**Result.** `work/boot/runs/fts-reset2.img` (`d0aff594…`, `-DSHIM_FTS_TEST -DSHIM_FTS_QUIET
-DSHIM_FTS_RESET2=1`) ran warm with the exact order that made XNU touch work on 11-09 (0125,
`touch-on6`): power-up reset, `0xa6` sysinfo, a second hardware reset (`post-sysinfo reset 1
ready err=0`), then `a0 00 01` with no sense cycle in between. The chip still failed:
`43 05 31` and two `43 05 30` in the FIFO drain plus the usual 8-contact right-edge/top-row
burst (`logs/runs/fts-reset2-warm.txt`). The known-good XNU order does not reproduce from the
shim, so command order alone does not decide the calibration.

**Timeline for reference.** A polling run (`fts-softrec90`) shows the shape plainly: `43 05 01`
(mutual-negative force cal, benign) at sense-on, silence for ~3.0 s, then 20+ whole-line
contacts on x=1079 and y≈176, with `43 05 30` only later. Nothing is wrong at sense-on; the
baseline drifts into the failure.

**TWRP's missing commands.** TWRP's DRM-blank path (`fts_mode_handler`, FOD branch) writes
`A2 03 20 00 00 01`, then `A0 01` (scan mode LOW_POWER, 2-byte form) and `C0 02 00 00`
(single tap only): the chip runs its gesture/low-power scan for 267 ms before the unblank reset
and final `A0 00 01`. The shim's `SHIM_FTS_TWRPSEQ` replays the sense on/off/on pairs but never
this low-power mode. `fts-blankcmd.img` (`c3bc6486…`, `-DSHIM_FTS_BLANKCMD=1`) adds it before
the final reset.

**Also new.** `tools/cepheus-shim-run.sh NAME IMAGE` wraps the warm loop (reboot, probe, TWRP,
log pull, summary) so each test is one command. `SHIM_FTS_RETRY_MS=N`
(`fts-retry.img`, `d41b88e4…`) does a second full power-cycle bring-up and quiet scan in the
same boot: if run 2 comes up clean, the first power-up is what misframes the chip; if both
ghost, the cause is outside the controller.

### PMIC GPIO blocks: the last un-dumped retained state (2026-11-18)

**Cold run of `a856c75a...` done; the entry dump is clean.** Cold boot into fastboot, same
probe image, log `logs/runs/fts-pmic2-cold.txt`. The whole entry block (arbiter APID map and
owners, SID 1/3/5 LDO banks, SMPS/BOB, PON, panel rails, touch/panel TLMM) diffs against the
warm run in exactly four lines: PM8150 `1:2f00`/`1:3000` `40=8` (warm) vs `40=c` (cold) and
PM8150L `5:1d00`/`5:1e00` `40=20` (warm) vs `40=24` (cold). All four are FTS-SMPS voltage
set points (0x40), i.e. leftover DVFS state, not a touch or panel rail. Every APID, owner,
LDO and pin is identical.

**The gap: PMIC GPIOs were never dumped.** Pinctrl-spmi-gpio blocks (SID 0/2/4, `0xc000 +
0x100*(n-1)`) are not regulators, and no ABL path writes them, but Linux's regulator-fixed
consumers do. In the live DT the only always-on one is `vdd_boost_vreg` on **PM8150B GPIO5**
(`0xc400`, `regulator-always-on`, no DT consumer): a full power-off returns it to POR, a
warm reset from TWRP keeps whatever TWRP left. That is the last candidate that is (a)
retained across a warm reset, (b) cleared by a battery-off, and (c) invisible to every dump
so far. The other PMIC GPIO users are inputs or disabled: `pm8150b_gpios 12` is
`ext_5v_boost` (`status = "disabled"`), `pm8150b_gpios 7` is WPC detect, `pm8150_gpios
6/9/10` are the volume key and VBUS inputs, `pm8150_gpios 1` is the disabled QBT1000
finger-detect.

**New image + test.** `log_pmic_gpios()` in `shim/shim.c` dumps type/subtype/RT-status/mode/
vin/pull/dig-in/out-ctl/enable for every GPIO block on SID 0/2/4 at shim entry; it runs in
`log_panel_state()` before `fts_test()`, so it records what ABL left. `fts-gpio5.img`
(`2300496f...`, `-DSHIM_FTS_TEST -DSHIM_FTS_GPIO5OFF`) additionally releases PM8150B GPIO5
just before the rails: mode `0x40 = 0` (digital input) and enable `0x46` bit 7 cleared,
logging before/after. One warm run answers both questions: GPIO5's entry state, and whether
releasing it clears the right/top-edge burst and `43 05 30`. If the GPIOs match cold vs warm,
the next block is GCC/dispcc always-on (`0xaf03000`) and AOP/RSC entry state.

### DSI link and USB/charger state exonerated; APID map + full rail banks dumped (2026-11-17)

**DSI0 PHY off does not clear the ghost.** `fts-dsioff.img` (`3db96611...`) adds
`-DSHIM_FTS_DSIOFF`: after the panel POR, `ft_dsi_off()` runs the
`dsi_phy_hw_v4_0_disable()` sequence (LPRX off, lanes off, blocks off, `ctrl0 = 0`) before
the FTS rails come up and leaves the PHY off through the scan. The PHY genuinely went down
(`lprx 3->0`, `ctrl0 7f->0`, `block 1f->0`, `st 7->5`) and the ghost still fired at
4261/4475 ms (`logs/runs/fts-dsioff-warm.txt`). Lane activity and the live link are not the
trigger; this was the last display-side block that differs from a cold boot.

**USB/charger state at reset does not matter.** Warm reset performed with the cable
unplugged (TWRP Reboot -> Bootloader on battery, replugged once fastboot appeared), so SBL1
saw `vbus_low` and skipped charger/CDP detection: `logs/runs/fts-pmic2-warm-novbus.txt`,
26 events, `43 05 30` at 3965/4122 ms. The entry register dump is identical to the
USB-attached warm run except two PM8150 SMPS set-points (`1:2f00`/`1:3000` `40=8` vs
`40=c`), neither touch-related.

**Entry state, warm.** `a856c75a...` (`-DSHIM_FTS_TEST`) dumps the whole arbiter map plus
SID 1/3/5 LDO banks and SMPS/BOB. 345 mapped APIDs; owners 0:99, 1:14, 2:12, 4:220.
PM8150B (SID 3) exposes only APIDs 304-308 (`b0`,`b1`,`b2`,`c0`,`d0`, owner 0); its
charger/regulator blocks (`0x10`-`0x31`) are absent from the apps-EE map, so SID 3 LDO
reads report `no apid` (that is a map property, not a failed read). PM8150L L1 (touch AVDD)
entry: `8=3 42=0 43=0 46=0`; powered: `4=4 5=71 8=84 40=8 41=7 42=8 43=7 44=7 45=2 46=80
48=0 4c=0 53=43 60=14 62=1 64=1 88=d0 a0=80`; powered-off restores the entry values. Log:
`logs/runs/fts-pmic2-warm.txt`.

**Still different.** TWRP boots warm and works; the shim reproduces its commands, rails,
rail order, resets, panel state, DSI state, and entry registers and still ghosts. Remaining
candidates: PMIC GPIO always-on state (new section above), GCC/AOP/RSC always-on state that
only a full power-off clears, a non-USB difference in ABL's warm path, and a shim-vs-TWRP
runtime difference (Linux IRQ-driven drain; regulator ramp under QUP/I2C load).

**New shim instrumentation (uncommitted).** `SHIM_FTS_DSIOFF` + `ft_dsi_off()`,
`spmi_dump_map()`, `log_ldo_bank()` for SID 3/5, the full L1 register dump in
`ft_l1_log()` entry/powered/off, `log_pmic_gpios()`, and `SHIM_FTS_GPIO5OFF` + `ft_gpio5_off()`.

### Panel exonerated; TWRP driver_test, mid-scan panel blank, pre-probe (2026-11-17)

**Panel emission is not the trigger.** `fts-bldim60.img` (`2ab67032...`), warm: the shim
brings the panel up fully (VCI, SET_ON, backlight `0x7ff`), sends DCS `0x51 = 0` *before*
the FTS rails/sense-on, restores `0x7ff` 15 s after sense-on, then polls for 60 s
(`SHIM_FTS_BLDIM_MS=15000`, `FT_POLL_MS=60000`). The same 10-contact right/top-edge burst
arrived at 3260-3471 ms **while the panel was dark**, then silence; no touch after the
backlight came back. With `fts-quiet-paneloff30` (VCI + reset held low 30 s, panel off
through the whole scan) also ghosting, both OLED emission and DDIC drive are out.

**LineageOS 23.2 has nothing new for warm boot.** `drivers/input/touchscreen/xiaomi/fts_521`
is the same ST 5.2.4 code as TWRP's (`fts_lib/*` differs only in whitespace; `fts.c` differs
only in Android 13/14 plumbing: `linux/msm_drm_notify.h`, `tp_common`,
`CONFIG_TOUCHSCREEN_XIAOMI_TOUCHFEATURE`). Cepheus does not even use it:
`arch/arm64/configs/vendor/xiaomi/cepheus.config` sets
`CONFIG_TOUCHSCREEN_ST_FTS_V521=y` and the overlay DT has `fts@49 compatible = "st,fts"`, so
the shipped driver is `drivers/input/touchscreen/st/fts.c` **4.2.14** (with
`CONFIG_FTS_FOD_AREA_REPORT=y`). Its probe path is `fts_system_reset` -> `readChipInfo` ->
`fts_interrupt_install` -> delayed `fts_fw_update` -> sense-on, with an ESD/watchdog
power-cycle recovery; there is no warm-boot-specific init. It does define `USE_NOISE_PARAM`
(5.2.4 does not) and reads/writes `C8`/`C7` around the resume reset, but firmware 0060
implements neither command (see the 2026-11-18 entry above), so that path is inert.

**Status decode correction.** `43 05 01` is `EVT_TYPE_STATUS_FORCE_CAL` subtype `0x01`,
"Mutual negative detect Force cal" - a normal status emitted right after sense-on, which
TWRP also reports and works with. The earlier note that TWRP emits a distinct `43 05 02`
was a misread (the extra byte is `event[7]`, the FIFO counter). `43 05 30` remains
`EVT_TYPE_STATUS_FORCE_CAL` subtype `0x30`, "MS invalid Mutual Strength soft Force cal".

**What is still different.** TWRP boots warm and works; every attempt to reproduce its
environment from the shim still ghosts: full command sequence (`fts-twrpseq`, 32 events then
`43 05 30`), TWRP rail order (`fts-twrppwr`), panel POR with or without age, panel held off,
300 s in fastboot, and an entry register dump that is byte-identical cold vs warm. A full
power-off is still the only thing that clears it, yet `twrp-after-xnu` shows touch working
again in TWRP after a failed XNU run. So the remaining difference is not the FTS command
list, the rails, the panel, or AP-visible entry state.

**TWRP's own driver cycles cleanly.** `tools/cepheus-fts-driver-test.sh` drove TWRP
3.5.2's `/proc/fts/driver_test` with the screen awake:

- `baseline logs/runs/fts-dt-baseline`: sysinfo OK, MS raw/filter/strength/baseline read
  (16x34 shorts, all four frames distinct; raw mean +283, strength -0.4, baseline -7.7).
  `GETMSFRAME` stops the scan; the script re-arms `A0 00 01` afterwards.
- `cycle logs/runs/fts-dt-cycle3`: `14` (power cycle, 20.4 ms: VDDIO off, AVDD off, reset
  low, VDDIO on, 1 ms, AVDD on, 5/10 ms, reset high), `12` (system reset, 22 ms),
  `01 A0 00 01`, then `17 01`. `43 05 01` arrived 1 ms later; no `43 05 30` and no contact
  events in the 8 s watch; frames still read normally. A 15 s `getevent` capture of
  `/dev/input/event4` then recorded a real swipe (207 `ABS_MT_POSITION_X` events), i.e. the
  chip is fully functional after the driver's own power cycle.

Two traps for the next run. Driver_test's `14` masks the AP IRQ (`fts_disableInterruptNoSync`)
and never restores it; TWRP's ESD path enables it last, so the script sends `17 01` after
sense-on. And TWRP's screen timeout blanked the panel after ~400 s idle, which put the
driver in FOD/gesture mode ("Sense OFF by FOD") and invalidated the first `cycle` run; also
`adb wait-for-device` hangs on TWRP's `recovery` transport, so the script uses `adb get-state`.

**Reading.** This matches `twrp-after-xnu`: the command/rail/reset sequence does not ghost
a chip that TWRP has already brought up. The one ordering element never replayed is TWRP's
DRM blank: it senses on at 1.272 s, senses off and drops the panel rails at 1.579 s, the
unblank replays SET_ON at 1.828 s, and only then resets and senses on again (1.850 s).
Every shim replay put the panel power change *before* the FTS bring-up, or ran with the
panel off for the whole scan.

**Mid-scan panel transition also ghosts.** `fts-midpanel.img` (`0e469cf1...`), warm,
payload-less shim with `-DSHIM_FTS_TWRPSEQ -DSHIM_FTS_TWRPSEQ_PANEL -DSHIM_PANEL_INIT
-DSHIM_PANEL_CYCLE`: TWRP's exact ordering - on1, off1, 50 ms, on2, 350 ms, off2 (the DRM
blank's sense-off), 250 ms, `panel_reinit()` (VCI/L14 off, TE stops, back on, DDIC reset,
34/34 SET_ON, backlight `0x7ff`), then the controller reset and final sense-on. Ghost
burst at 3698-3788 ms (20 contacts), `43 05 30` at 4070, water-mode enter/exit at
4183/4440. Panel ordering and DDIC state are now fully exonerated.

**The controller is genuinely unpowered at a warm entry.** `fts-preprobe.img`
(`d3b92977...`, `-DSHIM_FTS_PREPROBE`), same warm entry: `preprobe recover=0 ack=-1
sda=0 scl=0` - with VDDIO/AVDD still off the bus cannot even be raised, and the entry
dump has GPIO 84 `io=0` (VDDIO off), reset low and PM8150L L1 `46=0 41=7 45=2`. After
`ft_power_off()`, L1 reads `46=0`, so the RPMh off vote really lands. The same run ghosted
again (44 events, `43 05 30` at 4895 ms). Together with the 300 s fastboot dwell, this
rules out retained state inside the touch controller: the poison is on the AP/PMIC side,
or a difference between the shim's and TWRP's driver *runtime* that has not been isolated.

**What is left.** Every AP-visible entry register (touch pins, RPMh TCS, PM8150L L1, panel
pins, PON) is byte-identical cold vs warm, and the panel, command order and chip state are
out. Remaining candidates, in rough order: (1) an always-on domain the dumps do not cover
(GCC/AOP/RSC, PM8150 SID 0 which the shim cannot read) - dump those at entry on a warm and
a cold boot; (2) a shim-vs-TWRP runtime difference (the only untested ones are the
Linux IRQ-driven event drain and the regulators' ramp under the QUP/I2C load, both hard to
change from the shim); (3) temperature - every "cold" run also let the phone cool, and the
ghost is stochastic. A cool phone with a warm reset, or a warm phone with a full power-off,
would separate (3) from the reset type before investing in (1).

### 0147 warm no-replay: re-arm works, but the AFE stays silent (2026-11-17)

**Run.** `touch-0147-noreplay` with kernel `e16c5dc0...` (0147), warm from TWRP,
`cepheustouchdelay=20 cepheustouchlog=1 cepheustouchpanel=0 cepheustouchpanelpor=0`.
The user reports SpringBoard booted and that they touched the lit screen and pressed
power twice. Logs: `logs/runs/touch-0147-noreplay/`.

**Timeline.** rails 30.251, sense-on 30.967, `43 05 01` 30.969; ghost burst
34.226-34.348 (10 contacts, IDs 0-9, right/top edges); `43 05 30` 34.593 and 34.806;
`force cal; re-arm 1 after 600 ms` 35.193; `re-arm 1 off=0 on=0` 35.245; `43 05 01`
35.255; display on 55.068 with the 0141 reset (`display reinit err=0` 55.331, `43 05 01`
55.339); power-key wake 71.273 (`43 05 01` 71.546); auto-lock wake 103.213 (`43 05 01`
103.486). Every `alive` line reports `irq=1` (none asserted). **There is not one
`SWEET FTS: enter`, `motion`, or `leave` in the whole run**, although the display was
lit and SpringBoard was running while the user touched it.

**Reading.** `43 05 01` is a status response to the scan command, not proof of a sensing
AFE; the 0147 re-arm only removes the *second* force cal. A hardware reset + sense-on at
55 s and 71 s did not revive the AFE either, so this is not a one-shot FIFO stall that a
reset clears. The 5 ms-polled, RAM-logged shim `fts-softrec90` run showed the same silence
after its 3.08 s burst, so the serial-logging stall is not required to reproduce it.

**Tested since.** The panel-emission hypothesis in the older version of this section was
tested and rejected (`fts-bldim60`, above); LineageOS and the real `st/fts.c` 4.2.14 driver
were checked as well. See the newer section at the top.

### Panel POR cannot fix the warm ghost; AFE matches TWRP; soft re-arm test (2026-11-17)

**TWRP source + live DT re-check.** Cloned TeamWin's `android_device_xiaomi_cepheus`
and `LasagnaTeam/TWRP_Kernel_Cepheus` (`twrp-9.0`) into `work/twrp-src/`.  The panel
supplies are `vddio` (PM8150 L14) and `vcie` (`disp_vci_vreg`, east TLMM GPIO 21); the
DT's `vci-supply` (L13) is not used by any ea8076 panel entry.  TWRP's
`dsi_panel_power_on`: vddio -> 10 ms -> vcie -> reset low 1 ms/high 10 ms; power-off is
the exact reverse with 10 ms pre/post sleeps.  `touch_vddio_vreg` (north GPIO 84) is the
FTS VDDIO load switch, not panel VDDIO.

**TWRP's FTS probe/blank sequence** (source + dmesg): probe resets, sense-on `A0 00 01`
(1.2216 s), sense-off `A0 00 00`, 50 ms (`WAIT_AFTER_SENSEOFF`), sense-on (1.272 s); DRM
blank senses off (1.579 s); panel SET_ON system-resets at 1.828-1.850 s and the final
sense-on is at 1.850 s.  Only `43 05 01` appears; no `43 05 30`.  FOD blank sends gesture
`A2 03 20 00 00 01`, low-power `A0 01`, `C0 02 ...`; unblank resets and senses on.

**AFE and rails match.**  New shim readback: `si[28..30] = cfg/cx/panel = 3/3/3`,
`si[22..23] = 0x3e`, `si[26..27] = 0x3e`, `si[16..17] = 0x60`; TWRP reports
`AFE VER: CFG = 03 - CX = 03 - PANEL = 03`, config `0037`, fw `0060`.  Firmware/config
mismatch is ruled out.  PM8150L L1 SPMI readback (`ft_l1_log`): entry `46=0 41=7 45=2`,
after the TWRP-order vote `46=80 41=7 45=2`, proving the RPMh vote enables AVDD.

**Shim panel-POR series** (all warm, same phone boot cycle; logs under `logs/runs/`):

- `fts-panelpor.img` (`6a33e04c...`): true L14+VCIE POR + full 34-command SET_ON
  (34/34, 0 errors, pos 458/458, backlight `0x7ff`), TWRP rail order, quiet drain.
  Ghost at 6028/6031 ms.  Log: `fts-panelpor/`.
- `fts-panelpor-poll.img` (`65c9ecec...`): same but continuous polling.  **First run
  clean (no `43 05 30`), then the same image ghosted on two repeats.**  The clean run is
  a fluke, not a fix.  Logs: `fts-panelpor-poll/`, `fts-panelpor-poll2/`.
- `fts-age25.img` (`adc68720...`): POR, then 25 s of panel-on age, fast polling.
  Ghost at 3677/3878.  Log: `fts-age25/`.
- `fts-evdelay6.img` (`617da611...`): POR, 6 ms delay per logged event (models XNU's
  synchronous serial printf) -> ghost at 3476/3695.  Log: `fts-evdelay6/`.
- `fts-age25-repor.img` (`7dd4ba7d...`): 25 s age, then a **second full POR immediately
  before FTS power-up** -> still ghost at 3608/3821.  Log: `fts-age25-repor/`.

Every POR reports `init 34 cmds, 0 errors, pos 458/458`, backlight `0x7ff`, `te after
init 6 edges`, so the panel itself is healthy each time.  **Conclusions:** a panel POR
before sense-on does not clear the warm ghost; panel age at sense-on is not the
differentiator (the second POR rules it out); the 6 ms/event stdin stall alone also
ghosts.  The defect tracks the warm FTS start itself, and the outcome is stochastic
(identical images clean/ghost across runs).

**New shim flags.** `SHIM_FTS_PREDELAY_MS` (panel-on age), `SHIM_FTS_EVDELAY_MS`
(per-event delay), `SHIM_FTS_REPOR` (second POR after the age delay),
`SHIM_FTS_SOFT_RECOVER` (recover in software after the force-cal).

**Soft re-arm works (`fts-softrec.img`, `bf517818...`; `fts-softrec12.img`,
`0929e62c...`).**  On the first `43 05 30`, the shim waits 600 ms, sends sense-off
`A0 00 00`, waits 50 ms and senses on `A0 00 01` again.  In both runs the chip came back:
`43 05 30` at 3958 / 3690 -> (water pair in the 12 s run: `43 06 01` 3797, second
`43 05 30` 3923, `43 06 00` 3992) -> sense off/on -> `43 1 a0 00` + **`43 05 01`** at
4350-4363, then **no further events for the remaining 7.6 s** (baseline bursts recur at
~3.3 s).  Logs: `logs/runs/fts-softrec/`, `logs/runs/fts-softrec12/`.  This is the first
recovery that returns the controller to an armed, sensing state without a panel or rail
cycle.

**`xnu/patches/0147-cepheus-fts-force-cal-rearm.patch`** ports that to the FTS thread:
on `43 05 30/31` or water-mode enter within 10 s of sense-on and at most 4 times, arm a
`rearm_at = now + cepheustouchrearmms` (default 600 ms) deadline; the poll loop keeps
draining events until the deadline, then sends sense-off/50 ms/sense-on and refreshes
`sense_time`.  `cepheustouchrearm=0` disables it; the existing 0137 power-cycle recovery
remains as the fallback when the re-arm is disabled or its budget is spent.  The patch
applies cleanly after 0146 (checked with `git apply --check` against a reconstructed
post-0146 tree).

**First warm 0147 run (`touch-0147-warm`, kernel `e16c5dc0...`; default panel replay).**
The re-arm works exactly as in the shim: sense-on 31.204, ghost burst 34.824-34.926
(six contacts), `43 05 30` 35.502, `force cal; re-arm 1 after 600 ms` 36.097,
`re-arm 1 off=0 on=0` 36.149, then `43 01 a0 00` + `43 05 01` at 36.158.  No second burst
and no second force-cal afterwards.  The display-on reset at 54.734 re-sensed cleanly
(`43 05 01` 55.331) and the power-key wake at 71.615 did too (`43 05 01` 71.692).  The
screen went black at the display-on SET_ON replay (54.936-55.207), the known 0144/0145
defect; the user pressed power twice (display off 71.310 / on 71.615) but the panel stayed
dark.  **SpringBoard was nevertheless alive**: SMP SpringBoard samples at 99.762,
`SWEET AKS: SpringBoard` calls and 60 fps / 3000 framebuffer swaps through 135 s, so the
"SpringBoard did not boot" impression came from the unlit panel.  Log:
`logs/runs/touch-0147-warm/`.

**Next:** the no-replay warm run above answered this: the re-arm works but real touch
still does not.  If the first ghost burst injects phantom taps, add a HID quarantine
until the first re-arm completes.

### 0145/0146: skip the replay for a stable display; warm FTS calibration still fails (2026-11-16)

**Warm runs of the 0144 kernel** (`e94f1b6f...`, built from `892a586`):

- `touch-0144-warm` (delay 55, por=1): rails 65.36, replay 65.66-65.95, reassert 65.99,
  sense-on 66.46. Ghost 71.21, `43 05 30` 71.71. **Screen black from ~66 s**; power
  presses 97.6-102.6 only toggled the logical display.
- `touch-0144-nopor` (delay 55, por=0): rails 65.2 (VDDIO still dropped), replay 65.5,
  reassert 65.88. Ghost 66.8/70.1, `43 05 30` 70.52. **Screen black again.**
- `touch-0144-default` (delay 20, por=0): rails 30.2, ghost burst 34.8; display-on 54.52,
  replay 54.73-55.08, reinit 55.19. **Screen never lit.**

**Warm runs of the 0145 kernel** (`93b0d415...`, built from `6f76c61`; marker
`panel por=%u, vddio=%u` verified):

- `touch-0145-keepvddio` (`por=0 vddio=0`): rails 30.25 (`panel por=0, vddio=0`), sense-on
  30.97; display on 49.97, `DCS 0x51` 442 at 50.03, replay 50.18-50.54 (33 sent, 0
  errors), reassert 50.59, reinit 50.65. **Screen black exactly at the replay**, even
  though north GPIO 84 never dropped. Power presses 74.8-88.5 logical only.
- `touch-0145-noreplay` (`panel=0 por=0 vddio=0`): rails 30.25, sense-on 30.97; **no
  ghost burst at 34.8 and no `43 05 30`**; display on 50.15, brightness 442; power
  off/on 66.38/67.24 restored the screen (442 again at 67.45). FTS was alive
  (`sysinfo`, probe, sense-on all OK) but produced **zero touch events**: the AVDD-only
  cycle leaves the controller half-reset and silent.

**0145 conclusion was partly wrong.** `touch-0145-noreplay` used `vddio=0`, so north
GPIO 84 (the shared VDDIO rail) stayed low from boot and the FTS was never properly
powered. The clean "no ghost, no touch" result proved only that, not that AVDD alone
causes the ghost. With `vddio=1`, the warm ghost returns with or without the cycle (0146
runs below).

**0146 patch.** `xnu/patches/0146-cepheus-fts-panel-video-toggle.patch` (after 0145) adds
`cepheustouchvideo=1` (default 0, so existing behaviour is unchanged): before the replay
it saves `DSI_CTRL` (0xae94004) and writes `ctrl & ~7u` to stop the video engine, waits
20 ms; after the sequence's 50 ms `0x29` wait it restores `DSI_CTRL`, waits 20 ms, then
`sweet_bl_reassert()` runs as before. Logs: `panel replay video off ctrl %x -> %x` and
`panel replay video on ctrl %x -> %x`.

**Warm runs of the 0146 kernel** (`042044ff...`, built from `181838b`; strings
`panel replay video off/on` and `panel por=%u, vddio=%u` verified):

- `touch-0146-video` (`por=0 video=1`): rails 30.25, display on 77.95, `DCS 0x51` 442,
  replay 78.18-79.92 with `34 cmds, 33 sent, 33 errors`; `video off ctrl 1f5 -> 1f0`,
  `video on ctrl 1f0 -> 1f5`. Clearing `DSI_CTRL & ~7u` also stops the command DMA, so
  every replay command failed and the video engine never resumed: the panel froze on the
  Apple logo while SpringBoard booted underneath (0 fps). A second replay at 272.6-274.5
  failed the same way. **Unusable as implemented.**
- `touch-0146-noreplay-full` (`por=0 panel=0`): full VDDIO+AVDD cycle at 30.2, display on
  54.47, brightness 442 at 54.53; ghost at 34.14 (3 edge contacts), `43 05 30` at 34.44,
  recovery power cycle 34.44-35.53, then silent. Power off/on 64.01/65.17 restored the
  screen at 442 and a later wake at 144 s worked. **Full rail cycle with no replay keeps
  display and wake stable.**
- `touch-0146-nopwr` (`por=0 panel=0 pwr=0`): rails unchanged 29.68, display on 49.74,
  brightness 442; ghost at 33.39 (10 contacts), `43 05 30` 33.63, recovery 33.63-34.72,
  then silent. Power off/on 57.44/58.76 restored the screen. **The rail cycle is not the
  ghost's cause.**
- `touch-0146-late` (`delay=45 por=0 panel=0 pwr=0`): display on 49.71; FTS starts 54.69,
  sense-on 55.43; ghost 58.55 (~3.1 s later), `43 05 30` 58.6, recovery 58.8-59.88; power
  off/on 70.43/71.13 restored the screen. **Starting after display-on does not help.**

**Conclusions.** (1) The 34-command SET_ON replay is the only display killer: with
`panel=0` the display, brightness and power-key wake survive a full FTS rail cycle. (2)
`cepheustouchvideo=1` cannot be used: `DSI_CTRL & ~7u` disables command DMA too. (3) The
warm ghost/`43 05 30` is intrinsic to the FTS warm start, not the rail cycle or start
time: it appears ~3 s after sense-on in all three configurations. The recovery power
cycle runs but the chip stays silent afterwards. (4) `touch_vddio_vreg` on GPIO 84 is the
shared VDDIO rail; dropping it does not kill the panel (display on at 54.5 s after the
cycle) and is not the ghost's cause.

**`touch-0146-minimal` (2026-11-17).** Same kernel with every optional step off:
`cepheustouchpanelpor=0 cepheustouchpanel=0 cepheustouchpwr=0 cepheustouchprobe=0
cepheustouchreset2=0 cepheustouchsensecycle=0 cepheustouchdisplay=0 cepheustouchrecover=0`.
Entry `reset=0x1/0 vddio=0x1/0 irq=0x1/0 vci=0x3c3/0x3 prst=0x2c3/0x3` at 29.703 s, rails
unchanged, sysinfo 29.838 s, **one** `a0 00 01` at 29.839 s, `43 05 01` at 29.841 s, ghost
burst 33.193-33.229 s (same right/top edges), `43 05 30` at 33.454 s, **no recovery sent**,
no further events. Display on 49.744 s, DCS brightness 442 at 49.809 s; power off/on
58.842/59.363 s restored 442. Logs: `logs/runs/touch-0146-minimal/`.

**Conclusion: the defect needs nothing from our probe.** One hardware reset, the sysinfo
read and one sense-on are enough to reproduce the warm ghost and `43 05 30`. Probe replay,
extra resets, the sense cycle, display reinit, recovery and rail cycling are exonerated as
causes (they may still matter as fixes).

**Not off-first.** TWRP's probe sequence was re-read from its dmesg: after three probe
resets it senses on (1.221 s), immediately sends sense-off, waits 50 ms and senses on again
(1.272 s); the DRM blank then senses off (1.579 s), and the panel SET_ON path system-resets
and senses on a third time (1.850 s). 0140 already implemented the init-sensing toggle
exactly (`on, 2 ms, off, 50 ms, on`) and failed warm; 0142 added a reset before the final
on (`on 350 ms, off 50 ms, reset, ready, on`) and failed too. The only TWRP ordering still
untested is the **third** scan: ON->OFF(50 ms)->ON->307 ms->OFF->250 ms->reset->ready->ON.
`shim/ftstest.c` has `SHIM_FTS_TWRPSEQ` to replay that whole sequence before a 6 s quiet
drain.

**Next.** Warm shim run of `-DSHIM_FTS_TEST -DSHIM_FTS_QUIET -DSHIM_FTS_TWRPSEQ`. If the
drain has no edge burst and no `43 05 30`, port the sequence to 0147. If it still ghosts,
FTS command order is exonerated; next test `cepheustouchpwrms=2000` with the full
VDDIO+AVDD cycle and `panel=0`, then look at the rail ramp and the ABL warm path rather
than more FTS commands. Logs: `logs/runs/fts-twrpseq/`.

### 0144 FTS panel POR + brightness reassert: tested warm, superseded by 0145/0146 (2026-11-16)

**Why.** 0143's register-only replay is clean on the bus but does not fix the warm path.
In `touch-0143-warm` the replay ran at 65.510-65.873 (33 sent, 0 errors, pos 458/458, DSI
`hw 20030000 ctrl 1f5 status 5 fifo 1010 int 501 dma 10000000`) and the sense-on at
66.391 was clean, but the ghost burst still came at 70.084-70.165: x=1079 at
y=0/1346/1754/2187/2572 plus (717,1754) and (946,1754), `43 05 30` at 70.419, then the
early recovery power cycle. No touch after it. Worse, the power-key wake at 84.3 s left
the screen dark: the 83.7 s `0x51 0` was deferred as "still busy" (the 0143 replay clears
the DMA-done bit that 0136's `pending`/`last` state relies on), so `last` stayed 442 and
the wake's identical 14125-level write was skipped, while the 84.52 s replay skipped the
sequence's own `51 00 00`. The phone had to be rebooted.

**Patch.** `xnu/patches/0144-cepheus-fts-panel-power-cycle.patch` (both files; applies
after 0143):

- `fts_rails_off()` now sends TWRP's `DSI_CMD_SET_OFF` (`28 00` then `10 00`, 120 ms
  after sleep-in) and then drops panel VCI (east TLMM GPIO 21) and reset (south TLMM
  GPIO 6); VDDIO (north GPIO 84) is already dropped by the touch rail cycle.
  `fts_rails_on()` restores the DTB order: VDDIO -> AVDD -> 20 ms -> VCI -> 10 ms ->
  reset high -> 10 ms. This is the DDIC POR the shim tests could not do (link off) and
  TWRP gets from its DRM driver.
- `fts_panel_replay_on()` is now shared by the boot path, the first display-on and the
  early recovery; it sets `ft.panel_ready` so later power-key wakes do not replay SET_ON
  (the 0143 wake replay is what reset the brightness/gamma state).
- `sweet_bl_reassert()` in `SweetFramebuffer.cpp` resends the last `0x51` level after a
  replay, clearing the stale `pending` flag and bypassing the `level == last` skip.
- waits 200 ms after display-on so CoreBrightness's first `0x51` lands before the replay.
- boot args: `cepheustouchpanelpor=0` keeps VCI/reset powered (control);
  `cepheustouchpanel=0` still disables the replay itself.

**Apply check.** The first CI build (run #62, `eec9fde`) silently skipped the patch:
`git apply --check` failed on `SweetFramebuffer.cpp` because the generated hunk was
missing the blank-line context before `IOReturn` (the local apply-test used a synthesized
baseline). Fixed 2026-11-16; the patch now passes `git apply --check` against the #62
buildtree with all six hunks.

**Warm test.** Build, then from TWRP:
`tools/cepheus-run.sh touch-0144-warm -k <kernel> -s 200 -a "cepheustouchdelay=55 cepheustouchlog=1"`
Expected: display on ~49.9 s, FTS start ~64.7 s, SET_OFF + POR then `panel replay`
~65.2 s, the `sweet_bl_reassert` DCS 0x51 level 0x372d, sense-on ~66 s, no x=1079 burst
and no `43 05 30`; then press power twice: brightness 0 must be sent (no "still busy"),
no second replay, and the screen must visibly return. Logs: `logs/runs/touch-0144-*`.

**Controls / next.** If the screen stays dark, rerun with `cepheustouchpanelpor=0` to
separate the POR from the reassert. If the ghosts persist, the DDIC state is not the
whole story and the next step is moving the replay into `SweetFramebuffer.cpp`, where
brightness and display-on are owned. Known cosmetic cost: on a normal boot the logo goes
dark when FTS starts (SET_OFF + VCI off) and returns with the replay at display-on.

### 0143 panel SET_ON replay from XNU: design (superseded by 0144 in the warm test)

**Why the replay has to come from XNU.** In the warm shim-entry state the DSI0 link is off
(`DSI_CLK_STATUS` `0x2343` vs cold `0x8037c3`), which is why the shim's 34-command panel
replay never reached the DDIC (`touch-0144`/`0145`/`panelinit`). XNU's DSI FIFO path is
live: the DCS `0x51` brightness writes work. TWRP has a live DSI link and works warm after
`DSI_CMD_SET_OFF` at 1.706 s, `DSI_CMD_SET_ON` at 1.838 s and FTS sense-on at 1.880 s. So
the replay has to run from XNU, not from the shim.

**Patch.** `xnu/patches/0143-cepheus-panel-set-on-replay.patch` (only `bsd/dev/sweet_fts.c`;
applies after 0142):

- copies the live DTB's 34-command EA8076 on sequence (the same bytes as `shim/panel.c`
  `f1mp_on`, 458 bytes) and sends each command through the DSI0 command FIFO with the
  shim's packing, wait values and LP mode;
- runs it at display-on before the 0141 reset/sense-on, and at FTS start when the display
  is already up (the `cepheustouchdelay` case), which is TWRP's order: panel SET_ON, then
  FTS reset + sense-on;
- skips the sequence's `51 00 00` (it would blank the panel; XNU owns brightness), waits
  200 ms after the display-on counter so CoreBrightness's first `0x51` (~57 ms) is not
  overlapped, and 50 ms after `0x29` like TWRP;
- logs `SWEET FTS: panel replay ...` at entry and `panel replay N cmds, N sent, N errors`;
- boot args: `cepheustouchpanel=0` disables the replay, `cepheustouchpanelhs=1` sends it
  in HS instead of the DTB's LP mode. The panel can blink once while the gamma table is
  rewritten; that is expected.

**Result (2026-11-16).** The replay itself is clean (33 sent, 0 errors, pos 458/458) but
the warm ghost burst survives and the next power-key wake is dark. See the 0144 section
above for the run details and the follow-up design. Logs: `logs/runs/touch-0143-warm/`.

### Panel VCI + reset off 30 s does not clear the warm poison (`fts-quiet-paneloff30`, 2026-11-16)

**Run.** Warm from TWRP, shim built with `-DSHIM_FTS_QUIET -DSHIM_FTS_PANELOFF_MS=30000`
(`work/boot/runs/fts-quiet-paneloff30.img`). It drives panel VCI (east TLMM GPIO 21) and
panel reset (south TLMM GPIO 6) low, sleeps 30 s, and keeps both off through the scan.
Log: `logs/runs/fts-quiet-paneloff30/shim.txt`.

**Result.** 32 events / 24 contacts, still the warm signature: `43 05 01`, then
`43 01 a0 00 01`, then `43 05 31` (SS invalid self strength) before any contact, and
24 contacts with y=174 dominant (x=972, 432, 1003, 1016, 573, 951), plus two `43 05 30`
(MS invalid mutual strength) and two `43 06` errors. Events are drained after the 6 s
window, so their `t=` values are read times, not event times. Compare the panel-on quiet
run (`fts-quiet`): 23 events / 20 contacts, no `43 05 30`/`43 06`.

**Reading.** Dropping VCI and holding reset low for 30 s does not clear the warm state.
The scan is *worse*, not better: `43 05 31` appears immediately, which is expected when the
panel's ground plane is removed, so this test cannot separate DDIC state from the missing
panel. What it does show is that a crude VCI-only power-down is not TWRP's panel POR; the
shared VDDIO (GPIO 84) and the DTB power-up ordering are still untested.

### Warm poison survives 300 s in fastboot: it is latched, not charge (`fts-probe-warm-wait300`, 2026-11-15)

**Run.** Warm from TWRP (`adb reboot bootloader`), then **300 s idle in fastboot**
(15:05:17-15:10:17), then the same pre-XNU probe image (sha256 `5a6513aa0cbf9550...`).
Logs: `logs/runs/fts-probe-warm-wait300/shim.txt`.

**Result.** Entry block, rails (`vddio=1`, `ldoc1 err=0`), sysinfo and `sense on err=0` all
match the earlier warm probe. 17 events: the two startup events (`43 05 01` t=0,
`43 01 a0 00 01` t=6 ms) and **15 contacts from t=3.598 to 3.688 s**: right edge x=1079
at y=1343, 1496, 1754, 2207; top row y=175/176 at x=106, 704, 1024; interior 1038,1496.
Same signature as the 31-contact burst (`fts-probe-pre-xnu`), smaller. No `43 05 30` in
the 6 s window. A five-minute dwell does not clear it: the poison is a latched state, not
a decaying charge or temperature transient.

**ABL logs (corrects the earlier "identical" claim).** `logs/runs/abl-after-probe/` holds
the logfs dump. The two probe boots are identified by the probe image cmdline
(`... cepheusreboot=600`) and by the EBS -> shim-entry match (+240 ms in both):

- **Cold = `UEFILOG1`**: PON 128, KeyPress 8, UEFI start 1283 ms, UEFI Total 2103 ms,
  EBS 9202 ms, shim entry 9.44 s.
- **Warm = `UEFILOG3`**: PON 1, KeyPress 0, UEFI start 2158 ms, UEFI Total 769 ms,
  EBS 6460 ms, shim entry 6.70 s.

Panel/power init text is identical (panel id 00070000, 1080x2340, same regulator steps).
The differences are USB/charger order and boot timing: warm does CDP charger detection in
SBL1 (~0.9 s; cold sees `vbus_low` and detects USB later in UEFI), cold spends ~2.2 s
longer between `display picture 1` and `display picture 2` (3116 vs 903 ms), and boot
reason/keypress/haptics differ. Net warm reaches the image 2.74 s earlier. USB/charger is
already ruled out (`touch-0150-warmnousb`) and the panel/power state is identical, so this
is a timing/boot-reason difference, not a panel or rail difference.

**Next.** Warm boot of the 0141+0142 kernel with `cepheustouchdelay=0 cepheustouchpoll=0`
(IRQ-only reads, no 10 ms FIFO polling): if the burst disappears and real touches work,
host FIFO polling during calibration is the trigger and the driver default should change.

### Cold probe is clean: the warm poison is pre-XNU reset state (`fts-probe-pre-xnu-cold`, 2026-11-15)

**Run.** Same image as the warm probe (sha256 `5a6513aa0cbf9550...`), booted at 15:02:15
from a true cold fastboot (phone powered off in TWRP, Vol- + Power). Logs:
`logs/runs/fts-probe-pre-xnu-cold/shim.txt`.

**Entry state is byte-identical.** The shim entry block (lines 9-52) diffs empty against
the warm probe: same touch pins, RPMh RSC/TCS 0-7, PM8150L LDO1 (`46=0`), panel pins,
`pon:` registers and reboot reason. `saved (ufs up)` is 15.961 s cold vs 13.222 s warm,
but entry-to-UFS is 6.52 s in both; the 2.74 s difference is inside ABL (cold EBS 9.202 s,
warm 6.460 s; see the wait300 section), not in the shim or UFS.

**Clean scan.** Same rails (`vddio=1`, `ldoc1 err=0`), ready (`err=0`, idle SDA/SCL high),
sysinfo (`a5 01 ...`), `sense on err=0`. The 6 s window logged **2 events**: `43 05 01`
at t=0 and `43 01 a0 00 01` at t=6 ms. No ghost contacts and no `43 05 30`. The warm twin
logged 33 events: the same two plus **31 contacts from t=3.786 to 3.978 s**.

**Consequence.** The shim setup (bit-bang, no `.ftb` load, pre-UFS, early boot) is not the
confound: the same binary calibrates correctly cold and mis-calibrates warm. The warm
poison is created before ABL hands off and survives a warm reset, but a full power-off
clears it. The ABL cold/warm logs differ only in USB order and boot timing (wait300
section), so the leading candidate is always-on state the dumps do not cover (GCC/AOP/
RSC/PMIC); the 300 s dwell rules out a decaying charge/thermal transient.

### Pre-XNU shim FTS probe reproduces the burst (`fts-probe-pre-xnu`, 2026-11-15)

**Run.** Payload-less shim built with `-DSHIM_FTS_TEST`
(`shim/build.sh "" work/boot/runs/fts-probe.img`, image sha256 `5a6513aa0cbf9550...`),
warm from TWRP. `fts_test()` runs right after `wdt_off()`, before `ufs_init()` and
before any XNU code (a few seconds after handoff; `saved (ufs up)` at 13.2 s). Logs:
`logs/runs/fts-probe-pre-xnu/shim.txt`.

**Setup.** Entry state matches every warm run (touch pins GPIO input/pull-up, panel pins
unchanged, PM8150L LDO1 `46=0`, RPMh TCS 0-7 empty). VDDIO high, ldoc1 1800 mV (`err=0`,
ACTIVE TCS0 3 commands, done; the off vote later shows TCS0 `first 4=0`). Reset low 10 ms /
high 50 ms: ready event `err=0`, idle SDA/SCL high; sysinfo `err=0` (`a5 01 ...`);
sense-on `err=0`.

**Result.** `43 05 01` at t=0 and `43 01 a0 00 01` at t=6 ms, then **31 contacts from
t=3.786 to 3.978 s** (33 events with the two status events), the same coordinates as XNU
warm: x=1079 right edge (y=175, 1465, 1759, 2175), y=174-176 top row (x=0, 165, 169,
372), and the same interior points (728,1512 / 169,1508 / 372,1472). No real touch. No
`43 05 30` in the 6 s window (the probe stops there); both rails were then restored off
with reset asserted before returning to fastboot.

**Consequence.** The identical warm mis-calibration happens with no XNU code, 3.8 s
after a clean reset and scan started a few seconds after ABL handoff. The poison is in
pre-XNU warm-reset state (ABL warm path, an always-on domain, or something the shim
setup shares with warm XNU boots). The cold control (see the section above) is clean, so
the shim setup is not the confound; the poison predates XNU.

### Warm early scan fails: the poison is present before ~11 s (`touch-warm-early`, 2026-11-15)

**Run.** Same 0141+0142 kernel (`logs/runs/touch-0150-warmnousb/kernel`), warm from TWRP,
cmdline `cepheustouchdelay=0 cepheustouchlog=1 cepheusreboot=150`. Logs:
`logs/runs/touch-warm-early/`. The user confirmed SpringBoard booted, display on, no touch.

**Timeline.** FTS thread 9.696 s; first rails-off probe 9.862 s unpowered; rails off
200 ms unpowered 10.226 s; rails cycled 10.253 s; ready 10.335 s (`sda=55 scl=56
idle=1/1`); `sense on err=0 ... cycle=1 display=1` 10.970 s; `43 05 01` 10.973 s; ghost
burst 14.373-14.520 s (+3.4 s) with the same ten contacts (x=1079 right edge at y=174,
450, 1488, 2180; y=174-176 top row at x=0, 233, 306, 430, 569; interior 1050,1757);
`43 05 30` 14.690 s; power cycle 14.690-15.778 s; display on 50.289 s; `display reinit
err=0` 50.353 s; no real touch.

**Consequence.** Starting the first scan at ~11 s, ~20 s before XNU's display-on and
before backboardd, still produces the identical burst. The poison is present at the start
of XNU bring-up; it is not created by later XNU subsystems (USB, display, GPU, UI).

### Cold with the panel already on works: the warm poison is host-side, not panel/chip (`touch-cold-delay55`, 2026-11-15)

**Run.** Same 0141+0142 kernel (`logs/runs/touch-0150-warmnousb/kernel`), cmdline
`cepheustouchdelay=55 cepheustouchlog=1 cepheusreboot=150`, booted at 14:33:28 from a true
cold fastboot (power off in TWRP, Vol- + Power). Logs: `logs/runs/touch-cold-delay55/`.
The user confirmed display on and touch working.

**The panel was live 15.7 s before the chip powered up.** `SWEET FB: display on` 65.966 s;
FTS thread start 25.398 s + 55 s. `controller before rails-off: unpowered` 80.565 s,
`rails off 200 ms: controller unpowered` 80.928 s, rails cycled 80.954 s, ready 81.038 s,
sysinfo 81.061 s (fw 0060 cfg 0037 cx ffff afe 03/03/03, identical to warm), reset2
81.206 s, `sense on ... cycle=1 display=1 ms=350 cr=1` 81.673 s, `43 05 01` 81.681 s,
`43 05 21` (SS invalid self strength soft force cal) 82.567 s.

**Touch works.** Real contacts from 82.967 s through the end of the log (user swipes at
spread-out coordinates, no edge column). No ghost burst and no `43 05 30`. Later soft
force-cals at 125.021 s (`43 05 21`) and 137.533 s (`43 05 31`) did not stop touch.

**Warm comparison.** `touch-0142-delay` is the same panel environment (display on
49.706 s, sense-on 65.012 s, 15.3 s later) and failed with the usual ghosts and
`43 05 30`. `touch-warm-paneloff` held the panel reset low through the failed 30-33 s
scan and still produced the identical right/top-edge burst. The panel/DDIC is therefore
not the warm differentiator, and an active panel is not a poison by itself.

**Entry state is identical cold vs warm.** `touch-cold-delay55/shim.txt` and
`touch-warm-prepower/shim.txt` have the same touch pin state (all five pins `cfg 1
(pull D mux 0 2mA in) io 0`), the same empty TCS 0-7, the same PM8150L LDO1/LDO2
registers (LDO1 enable `46=0`), and the same panel pins. The chip is unpowered in both.

**Consequence.** The warm defect needs no OS in between (`touch-ablwarm-0138`: cold
fastboot -> `fastboot reboot-bootloader` -> XNU, failed), is not chip state, supply
ramp/POR, rails, panel, temperature, VBUS or display timing, and is present with every
visible entry state identical. It is host-side state created after ABL and surviving a
warm reset, or an ABL cold-vs-warm boot-path difference.

**Next control: scan early on a warm boot.** Done as `touch-warm-early` (see the section
above): first sense-on at 10.97 s still fails with the same burst at +3.4 s. A shim-side
FTS bring-up (`fts-probe-pre-xnu`) also fails at +3.8 s with no XNU code, so the poison
predates XNU. The same probe image is clean cold (`fts-probe-pre-xnu-cold`, top section):
the poison is pre-XNU reset state cleared by a full power-off.

### Pre-powered rails do not fix warm touch; rail ramp/POR timing is ruled out (`touch-warm-prepower`, 2026-11-15)

**Run.** Same 0141+0142 kernel (`5cc977a1...`) as 0149/0150, shim rebuilt with
`-DSHIM_TOUCH_PREPOWER`, cmdline `cepheustouchpwr=0 cepheustouchlog=1 cepheusreboot=150`,
warm from TWRP (`adb reboot bootloader` -> `fastboot boot`). Logs:
`logs/runs/touch-warm-prepower/`. The user confirmed no touch; SpringBoard booted normally.

**What the shim did.** Entry `reset 1/0 vddio 1/0`. It then held reset low, drove VDDIO
(GPIO 84) high, waited 50 ms, enabled ldoc1 at 1800 mV (ACTIVE RPMh vote, err 0), waited
200 ms, and released reset at 8.561 s (`reset 2c3/3 vddio 200/3`). The chip therefore ran
with both rails stable for ~22 s before the driver touched it.

**The RPMh vote is real at the PMIC.** The payload-less control
`work/boot/runs/prepower-probe.img` (`logs/runs/prepower-probe/shim.txt`) read PM8150L
LDO1 at both SPMI SIDs. SID 4 has no regulator block (all reads error/zero); SID 5 is the
real LDO and changed `46=0 -> 80`, `08=3 -> 84`, `42=0 -> 8` after the enable. This is the
missing positive control for the ldoc1 vote; the earlier `touch-pmic-cold/warm` dumps at
SID 5 were reading the right rail.

**Still fails, with the same burst.** FTS TLMM 30.073 s, `rails unchanged, off 200 ms,
avdd ldoc1 err=0` 30.100 s, sysinfo/lockdown/CRC unchanged, reset2 30.354 s, `sense on ...
cycle=1 display=1 ms=350 cr=1` 30.820 s, `43 05 01` + `43 01 a0 00 01` 30.823 s. At
**32.607-32.665 s (+1.79 s)** six contacts entered with the same right/top-edge pattern as
all other warm runs: x=1079 at y=175/1754/1343/1466 and y=176 at x=170/4. There was **no
`43 05 30`** this time (the `pwr=0` path also suppressed it in `touch-0139-pwr0`); the chip
went silent. Display on 50.654 s, `display reinit err=0` 50.720 s, clean `43 05 01`
50.729 s, alive ticks through 93.3 s, zero real finger events. The log ends at 103.5 s
because the user rebooted to TWRP.

**Consequence.** A 22 s stable supply before the driver's reset does not change the
failure; only the ghost-burst timing moved earlier (+1.8 s vs +3.2 s) and the explicit
`43 05 30` disappeared. The chip's power-up ramp, POR timing and rail settling are ruled
out. The defect is created during scanning, after a correctly powered and reset chip, and
needs a warm-specific environment difference (or a host-side difference in how the first
scan is serviced).

**Next control: cold boot with the display on before sense-on.** Done as
`touch-cold-delay55` (see the section at the top): it works, with the panel live 15.7 s
before the chip powers up. Panel/chip state is ruled out; the warm poison is host-side.

### 0150 warm with USB unplugged: VBUS makes the +0.5 s burst, not the +3.5 s failure (2026-11-15)

**Run.** `touch-0150-warmnousb`, same 0141+0142 kernel (`5cc977a1...`), warm from TWRP,
default shim, `cepheustouchlog=1`, 150 s. `adb reboot bootloader` -> `fastboot boot`; the
USB cable was unplugged when the Mac spoke, 6 s after the image was sent (before XNU start
at 9.459 s), and stayed off through the run. Logs: `logs/runs/touch-0150-warmnousb/`
(`xnu.txt`, `shim.txt`).

**Still warm, still fails.** Shim jump 9.459 s (warm UFS 7.5 s); FTS rails 30.163-30.190 s,
sysinfo 30.298 s (same fw/cfg/afe/res), reset2 30.443 s, `sense on ... cycle=1 display=1
ms=350 cr=1` 30.910 s, `43 05 01` + `43 01 a0 00 01` 30.913 s. XNU's battery line still
prints `external=1` with `100% 4200mV`, but SWEET BATTERY is a hardcoded static fallback
(the battery was at 92% when checked in TWRP), so it is not evidence of VBUS; the cable was
physically off.

**The +0.5 s ghost burst is gone.** In 0148 (USB on) five contacts appeared at +0.47 s;
here there is **no event at all between sense-on and +3.22 s**. The early burst was
USB/charger-coupled noise, and it also explains the "touch" seen at +0.45 s in 0147.

**The main failure is unchanged.** At 34.133-34.206 s (+3.22 s after sense-on) six contacts
entered: x=1079 at y=176/1481/1343 and y=176 at x=697/440/149, the same right/top edge
pattern. `43 05 30` at 34.481 s (+3.57 s), early recovery 34.481-35.583 s, clean status
35.583 s. Display on 49.929 s, reinit 49.991 s; second display on 92.138 s, reinit 92.201 s;
alive ticks through 81.9 s (`irq=1 errors=0`); no finger events after the 34.2 s burst.

**Consequences.** VBUS/charger is dead as the cause of the main failure: without a cable the
chip still mis-calibrates at +3.2 s and goes silent. The early burst is a separate charger
transient, not the defect. The phone is not hot either (battery 25.5 C, SoC 30-34 C in TWRP
after the run), so the remaining cold/warm delta is not power, not temperature, and not the
FTS chip's retained state (unpowered at entry, POR'd in both). The next clean control is a
**cold boot with the display on before sense-on** (`cepheustouchdelay=55`): if it fails like
the old touch-on4/on5, the panel's active state is a sufficient poison and the warm failure
may be the same mechanism; if it works, the warm reset itself is the cause.

### 0149 cold control: 0142 works cold, no ghosts, 0141 display reinit harmless (2026-11-15)

**Run.** `touch-0149-cold`, same 0141+0142 kernel (SHA-256 `5cc977a1...`), **cold**
(power off, power+vol to fastboot) at 12:16:50, default shim, `cepheustouchlog=1`, 150 s.
The user touched only after SpringBoard was up. Logs: `logs/runs/touch-0149-cold/`
(`cmdline`, `kernel`, `xnu.txt`, `shim.txt`).

**Cold timing.** XNU starts at 40.237 s (cold ABL takes ~40 s; warm ABL ~9.5 s). FTS
`starting in 20 s` 40.395 s, TLMM 60.397 s, rails-off positive probe 60.562 s
(`controller before rails-off: unpowered`), rails off 200 ms 60.927 s, rails on 60.954 s,
ready 61.038 s, sysinfo 61.062 s (fw 0060, cfg 0037, cx ffff, afe 03/03/03, 2339x1079
tx=16 rx=34), lockdown 61.079 s, CRC 61.080-61.144 s, reset2 61.207 s, `sense on ...
cycle=1 display=1 ms=350 cr=1` 61.674 s, then only `43 05 01` and `43 01 a0 00 01`
(61.677 s).

**No ghosts.** From sense-on 61.674 s to display-on 80.365 s (18.7 s, exactly the window
where a warm chip fires its first five-contact burst at +0.47 s) there are **zero** events.
Display on 80.365 s, `display on 1; reset and sense on` 80.367 s, `display reinit err=0`
80.430 s, clean status 80.438 s; still nothing after the reinit. Alive ticks at
74.165/86.653/99.120 s with `irq=1 errors=0`. No `43 05 30` anywhere in the run.

**Touch works.** First finger: `enter 13 01 64 02 8b 00 07 00`, `touch 0 down at
612,2224` 105.858 s (25.4 s after display-on, 44.2 s after sense-on), then normal
enter/motion/leave through `touch 0 up at 464,1642` 135.338 s; 24 down/up pairs (48 events)
in that window. One `43 05 31` at 192.677 s, immediately after the user's volume-up/power
key events (192.515/192.557 s) and far outside the 5 s early-failure window, so it is not
a failure; touch had already worked for 30 s.

**What it settles.** (a) 0142 and 0141's display-on reset/reinit do not break cold touch;
(b) a cold chip stays quiet through the whole warm failure window; (c) the shim's
pre-XNU dump is identical warm vs cold except SDE scanout counters: PON/PS_HOLD,
`reboot reason: pon spare 0 -> 4 (imem 77665500)`, panel VCI/VDDIO (both off), AMOLED
AB/IBB, pm8150l ldo1/ldo2 and UFS all match. The differentiator is not a register the
shim sees; it is either the warm environment during the scan (USB/charger, temperature)
or state the shim does not dump.

**Next experiment: warm boot with USB unplugged right after `fastboot boot`.** Sense-on
is at ~30.9 s warm (XNU ~9.5 s + 21.4 s), so unplugging the cable 2-3 s after the image is
sent removes VBUS/charger negotiation well before the first scan. Logs live on userdata;
replug and pull from TWRP afterwards. A 5 s rails-off warm run (`touch-warm3`) already
failed, so discharge time and FTS retained state are ruled out. Cold with USB connected
works (0149), so a warm-unplugged pass would isolate VBUS; a warm-unplugged fail would
rule the charger out. **Done as `touch-0150-warmnousb`: the main failure is unchanged, so
VBUS/charger is ruled out; only the +0.5 s burst disappeared. See 0150 above.**

### 0148 quiet run: the chip mis-calibrates by itself on a warm boot (2026-11-15)

**Run.** `touch-0148-quiet`, same 0141+0142 kernel (`5cc977a1...`), warm from TWRP,
default shim, `cepheustouchlog=1`, 150 s. The user was told **not to touch at all** until
SpringBoard had been visible for ~20 s. Logs: `logs/runs/touch-0148-quiet/` (`xnu.txt`,
`shim.txt`).

**Init identical to 0147.** FTS thread 9.654 s, TLMM 29.656 s, rails off 200 ms 30.185 s,
rails on 30.212 s, ready 30.297 s, sysinfo/lockdown/CRC 30.320-30.402 s, reset2 30.466 s,
`sense on ... ms=350 cr=1` 30.932 s.

**The chip produced ghosts on its own.** At **31.405-31.443 s**, only 0.47 s after the
second sense-on and ~18 s before XNU's display on, five contacts entered and left within
38 ms: (135,988), (300,1763), (589,352), (591,1636), (589,2073). No finger was on the
panel. A second five-contact burst followed at 34.599-34.646 s, four of them on the
x=1079 edge plus (443,175); `43 05 30` came at 34.965 s (+4.03 s after sense-on), then
the early recovery power cycle (34.965-36.057 s).

**No touch ever worked.** Display on 49.825 s, `display reinit err=0` 49.895 s; second
display on 88.358 s, reinit 88.427 s. The user swiped after the display was up; the log
has **zero** finger events after 36 s.

**Consequences.** The early-swipe confound is dead: a warm boot mis-calibrates with an
untouched panel. The single "touch" seen at +0.5 s in 0147 and 0143 was this same early
ghost burst, not a real finger; 0150 later showed that burst is USB/charger noise and that
it is not the defect. The defect is the chip-side calibration/baseline that trips
`43 05 30` ~4 s later.

**Next: cold control with this kernel** (same protocol: no touch until display-on + 20 s).
Open questions it answers: (a) does cold still work with 0142, (b) does 0141's display-on
reinit at ~50 s poison an otherwise healthy cold boot, and (c) what does the panel do
during a cold `fastboot boot` (the shim's SDE probe shows a live scanning pipe with
autorefresh on both cold and warm, so the panel is *not* simply off on cold). **Done as
`touch-0149-cold`: cold touch works, no ghosts, the reinit is harmless; see above. Next:
the warm USB-unplugged test.**

### 0147 sense-window run: the cycle ran, an early ghost, then the same failure (2026-11-15)

**Run.** `touch-0147-sensewindow`, 0141+0142 kernel (SHA-256 `5cc977a1...`, contains
`cepheustouchsensems` and the `ms=%u cr=%u` format), warm from TWRP, default shim (no
`SHIM_PANEL_*`), `cepheustouchlog=1`, 150 s. Logs: `logs/runs/touch-0147-sensewindow/`
(`xnu.txt`, `shim.txt`).

**The cycle ran.** FTS thread at 9.656 s (`starting in 20 s`), probe at 29.658 s, rails
off 200 ms at 30.188 s, rails cycled 30.215 s, ready 30.299 s, sysinfo 30.323 s (fw 0060,
cfg 0037, cx ffff, afe 03/03/03, 2339x1079 tx=16 rx=34), lockdown 30.340 s, CRC 30.341 s,
crc reset 30.405 s, reset2 30.468 s, and `sense on err=0 ... cycle=1 display=1 ms=350 cr=1`
at 30.935 s. The first (cut) sense-on was at ~30.47 s, so the 350 ms window, the 50 ms
sense-off and the reset all executed. The usual `43 05 01` (mutual negative detect) and
`43 01 a0 00 01` arrived at 30.938 s.

**An early ghost, not a finger.** `SWEET HID: touch 0 down at 199,172` at 31.389 s and up
at 31.404 s (17 ms after the second sense-on). The controller is not dead at sense-on (the
status is clean), but 0148 later produced this exact burst with no finger on the panel, so
this contact is display/touch noise, not a valid touch. The user was swiping from ~30 s as
instructed.

**Then the same failure, slightly later.** Ghost burst 35.146-35.235 s: six contacts,
x=1079 (y=176, 1754), x=1068 (y=1482), x=983 (y=2190), x=981 (y=1758 edge), x=429
(y=176), x=830 (y=1480). `43 05 30` (MS invalid Mutual Strength soft Force cal) at
35.523 s, i.e. +4.21 s after the second sense-on and +4.68 s after the first one. Early
recovery: `early status failure; power cycle 1` 35.523 s, `power cycle 1 err=0` 36.615 s,
clean `43 05 01` + `43 01 a0 00 01` 36.623 s, and **no finger events after that** through
the end of the run. Display on 49.537 s, `display on 1; reset and sense on` 49.539 s,
`display reinit err=0` 49.602 s, `43 05 01` 49.611 s; second display on 71.025 s, reinit
71.092 s, same status; alive ticks through 119.5 s with `errors=0`.

**Reading.** The reset-before-the-latch idea is wrong: the chip still goes MS-invalid
~4 s after scan start. The clean status right after the second calibration shows the
controller and the I2C path are healthy at sense-on, so the failure is a later
calibration/baseline event, not a dead bus or a rejected sense-on. The cycle also moved
the failure later (from ~3.5 s to ~4.2-4.7 s after the first sense-on), so the 350 ms
first scan did change the chip's timing.

**Open confound (answered by 0148).** In every failing warm run the user was swiping a
black screen from ~30 s, i.e. during/just after the chip's calibration; the cold runs that
work were touched only after the display came on (~60-80 s). `touch-on5` (cold, delay 55,
untouched at sense-on) failed on the first touch at +3.7 s, so early touch is not proven to
be the trigger, but the warm runs never had a clean no-touch calibration. `touch-0148-quiet`
(warm, same kernel, no touch before ~75 s) was the control: it produced the ghost burst at
~35 s with no touch at all, so the warm state itself is the trigger.

### 0142 sense-window patch: cut the first scan early, reset, sense-on (2026-11-15)

**Patch.** `xnu/patches/0142-cepheus-fts-sense-window.patch`, on top of 0141 (only
`bsd/dev/sweet_fts.c`; applies cleanly with `git apply`). `fts_sense_cycle()` now does
sense-on, waits `cepheustouchsensems` ms (default 350), sense-off (`A0 00 00`), waits
50 ms, then `fts_release_all()` + `fts_reset()` + `fts_bb_recover()` + `fts_wait_ready(500)`
before the final sense-on. The sense-on line ends with `ms=350 cr=1`;
`cepheustouchsensecycle=0` still gives the old single sense-on, and
`cepheustouchcyclereset=0` keeps the 350 ms pulse but skips the reset (control).

**Rationale.** TWRP cuts its first scan at 1.6048 s, ~373 ms after sense-on at 1.2315 s,
then power-cycles the panel, system-resets FTS at 1.8582 s and senses on at 1.8805 s; its
second calibration stays healthy. A warm XNU scan left running latches the bad mutual
baseline after ~3.5 s, and a reset *after* that does not clear it (0140/0141). The untested
variable is a reset *before* the latch, on a chip whose first scan was stopped the way
TWRP stops it.

**Test (ran as `touch-0147-sensewindow`; failed, see above).** Build 0141+0142, boot warm
from TWRP with the default shim (no `SHIM_PANEL_*` flags), `cepheustouchlog=1`, and swipe
from ~30 s to the end of the run. Expected: `sense on ... ms=350 cr=1`, no edge ghost
burst, no `43 05 30`, real `SWEET HID: touch` events. Actual: the cycle ran, one early
ghost at +0.45 s, then the ghost burst and `43 05 30` at +4.2 s.

### 0146 shim PHY cycle restores TE, not touch; panel state is ruled out (2026-11-14)

**Probe.** Image built with `SHIM_CFLAGS="-DSHIM_PANEL_INIT -DSHIM_PANEL_CYCLE
-DSHIM_PHY_CYCLE"` and the 0141 kernel (`35b48585...`), booted warm from TWRP with
`cepheustouchlog=1`, 300 s. Shim log: `phy entry ctrl0 7f pll 1 rbuf 1 vreg 5b/19 ctrl3 4
cfg 21/84 clk 31/31 st 7`, `phy cycle done ctrl 1f5 clk 23f status 0`,
`init 34 cmds, 0 errors, pos 458/458`, `backlight 0x7ff -> 0`, **`te after init 6 edges`**,
`dma restored 14000000`. The full DSI0 PHY/controller re-init does revive the DDIC's TE
after the panel rail POR.

**Touch unchanged.** XNU: sense-on 31.145 s, ghosts 34.856-34.921 s, `43 05 30` 35.166 s,
recovery 35.8 s, display on 50.394 s, `display reinit err=0` 50.457 s. The user touched
repeatedly; no HID touch events through 97 s. `SWEET FB: 5s: 0 swaps` throughout (the
screen stayed black: the re-init breaks XNU scanout, while the DDIC itself is scanning).

**Consequence.** With the DDIC reinitialized and TE streaming *before* FTS calibrates, the
warm FTS failure is identical. The panel/DDIC state theory is dead; the 0142-delay control
(display on with content before sense-on) already pointed the same way. The remaining delta
is FTS chip calibration timing/state, not the panel.

**TWRP blank path decoded.** At 1.6048 s TWRP's `fts_mode_handler` takes the FOD branch
(`fod_status` set): it sends `A2 03 20 00 00 01` (gesture + longpress), `A0 01` (low-power
scan, 2 bytes) and `C0 02 00 00` (single-tap delay). The non-FOD branch sends only
`A0 00 00`. XNU has no FOD/gesture client, so `A0 00 00` is the correct analog; the gesture
command does not belong in the kernel cycle.

### 0145 rail-only cycle + TE probe: the RPMh vote really cuts panel VDDIO (2026-11-14)

**Probe.** Image `work/boot/runs/touch-0145-teprobe/touch-0145-teprobe.img` (SHA-256
`0a5f9651...`) is the 0144 code with the DCS 0x28/0x10 sleep/display-off removed: it only
cycles VCIE + LDO14, so the DSI link stays alive, and samples the panel's TE output (north
TLMM GPIO 8 pad level) for 50 ms at three points. Log:
`logs/runs/touch-0145-teprobe/shim.txt`.

**Result: TE 6 -> 0 -> 0.** `panel: cycle vcie cfg 3c3 io 3 te 50ms 6 edges` before the
cycle, `panel: vcie off, ldoa14 off -> 0`, `panel: te with rails off 0 edges`, then
`panel: ldoa14 on -> 0`, `panel: rails cycled, vcie io 3 te 50ms 0 edges`. Both RPMh votes
report `done=1 ... ACTIVE vote complete`, so the vote reaches the RSC. Six edges per 50 ms
(~60 Hz, two edges per frame) collapse to zero while LDO14 is off: **the shim's RPMh
`ldoa14` vote genuinely removes panel VDDIO.** But the panel never resumes scanning when
VDDIO returns. The DSI controller state at replay is unchanged (`hw 20030000 ctrl 1f5
status 0 fifo 11111010`), yet the DDIC is left in a state only a DSI controller/PHY
re-init recovers — the same dead end as 0144, now without the DCS sleep-in as an excuse.

**Touch is unchanged.** Sense-on 31.082 s, ghost burst 34.607-34.680 s (four edge contacts
at x=1072-1079), `43 05 30` 34.880 s, recovery 35.568 s, display on 50.353 s (dark), no
real touch. A real POR of VDDIO alone neither fixes nor changes the warm FTS failure.
Logs: `logs/runs/touch-0145-teprobe/`.

### DSI0 PLL/PHY configuration is identical cold and warm (`dsi-probe`, 2026-11-14)

**Probe.** Payload-less image `work/boot/runs/dsi-probe/dsi-probe.img` (SHA-256
`953282ac...`), built with `SHIM_CFLAGS=-DSHIM_DSI_PROBE shim/build.sh "" OUT`. At shim
entry it samples TE (north TLMM GPIO 8) for 20 ms and dumps the whole DSI0 controller
(`0xae94000`, `0x400`), PHY (`0xae94400`, `0x760`) and PLL (`0xae94900`, `0x260`) blocks
with offsets. Warm run from TWRP: `logs/runs/dsi-probe-warm/shim.txt`; cold run (phone
powered off, Vol- + Power to fastboot, then `fastboot boot`):
`logs/runs/dsi-probe-cold/shim.txt`.

**Result: every PLL and PHY configuration register matches, word for word.** The only
differences are volatile link status:

- `DSI_LANE_STATUS` (`0xa8`): warm `0x1f0f`, cold `0x1f00`. Bits 8-12 (all five lanes not
  in ULPS) are set in both. The low nibble is the per-lane stop-state status.
- `DSI_CLK_STATUS` (`0x120`): warm `0x2343`, cold `0x8037c3`. Bit 16 is the sticky
  PLL-unlocked flag (set on cold, from the ABL PLL startup; cold works), bit 31 is the
  byte-clock/link state. Warm is the "link off" signature TWRP leaves when it powers the
  panel down for reboot; it is not a configuration difference.
- PHY lane blocks at `0x144`/`0x1c4` differ only in bit 0 (`0x4e` warm vs `0x4f` cold),
  the matching link-active bit. The PLL range `0x1bc-0x1c4` changes between two reads of
  the same address inside one run (it is a live counter/status).

TE measured 2 edges/20 ms warm and 4 edges/20 ms cold; a 20 ms window is 1.2 frames, so
this is pulse phase, not a refresh-rate difference (the 0145 50 ms probe gives ~60 Hz).

**Consequence.** The "warm PLL/PHY/SSC register state" theory is dead: the SoC-side DSI
configuration is already the cold one on a warm boot. Therefore the panel difference that
TWRP repairs cannot be read from the AP's DSI registers; it is either DDIC-internal state
or the link on/off state, which XNU (or ABL) already recovers for scanout in the working
0142 run. Note the PHY node's 0x760 window overlaps the PLL node (`0xae94400 + 0x500 =
0xae94900`), which is why the last 19 lines of the PHY dump repeat the PLL dump.

### 0144 full shim panel POR: RPMh works, panel does not come back (2026-11-14)

**Implementation.** `shim/rpmh.c` (new) ports the XNU ACTIVE-set transaction from
`bsd/dev/sweet_rpmh.c` (0062/0102) to the shim's identity-mapped Device memory, and
`shim/panel.c` (`SHIM_PANEL_CYCLE`) now does TWRP's `dsi_panel_power_off/on` analogue
sequence: reset low, VCIE (east TLMM GPIO 21) off, PM8150 LDO14 (`ldoa14`, panel VDDIO)
off via RPMh, 20 ms, LDO14 on (1800 mV / mode 7 / enable), VCIE on, then the existing
34-command replay. A failed probe skips the cycle so the panel is left as ABL had it.

**0144 warm run (`touch-0144-panelpor`).** Image SHA-256 `1ec633a1...`, booted warm from
TWRP with the 0141 kernel and the usual `cepheustouchlog=1`. Shim log:
`rpmh: ldoa14 addr=0x42100 slave=4`, `probe ldoa14 ok, TCS0 free`, the 1-command disable
and 3-command enable both `done=1 ... ACTIVE vote complete`; panel `vcie off, ldoa14 off
-> 0`, `ldoa14 on -> 0`, replay `34 cmds, 0 errors`, backlight sent. **Screen stayed dark
for the whole run.** XNU: sense-on 31.109 s, ghost burst 34.742-34.806 s, `43 05 30`
35.057 s, recovery 35.737 s; display on 50.248 s but no visible output. The DCS 0x51
`int ... (pending)` at display-on is not a clue: the working 0142 run logs the same.
Logs: `logs/runs/touch-0144-panelpor/`.

**Why it is dark.** TWRP recovers from a panel power cycle because `dsi_display_enable`
re-initializes the DSI controller and PHY. The shim only replays DCS commands through
ABL's DSI state; after the DDIC loses power (and especially after DCS 0x10 sleep-in),
the replay reports DMA done at the controller but the panel never accepts it. So this
run cannot yet test the panel-state theory: the touch result is the same as before
because the panel state after the cycle is not TWRP's state.

### 0141 display-on reinit failed; TWRP's real panel power cycle is the remaining delta (2026-11-14)

**0141 warm run (`touch-0141`).** Kernel with 0141 (`work/boot/kernel-brightness/kernel.development.sweet`,
SHA-256 `35b48585...`), booted warm from TWRP, `cepheustouchlog=1`. Sense-on at 30.503 s
(`cycle=1 display=1`), then the usual ghosts at 34.137-34.173 s, `43 05 30` at 34.430 s,
clean recovery at 35.112 s, display on at 49.743 s, `display reinit err=0` at 49.813 s and
`43 05 01` + `43 01 a0 00 01` at 49.821 s. A second display off/on at 110.158/110.261 s
reinit'd again at 110.265-110.329 s with `err=0`, still **no real touch** through 117 s.
Logs: `logs/runs/touch-0141/`.

**TWRP comparison, part 2.** `fts_fw_update` (1.209-1.282 s) does not burn: CRCs clean,
one system reset, no CX/panel initialization, then `fts_init_sensing` sends the same
`a0/00/01` sense-on and IRQ enable as us. `fts_read_touchmode_data` only reads
(`0xc1/05`, `0xc1/08`); the `0xc0`/`0xa2` writes are sysfs-only (edge rejection, FOD,
gesture) and never run during TWRP's boot. **There is no missing chip command left.**

**New reading of the failure.** TWRP's first scan starts at 1.231 s but it senses OFF at
1.586 s — only 0.35 s later, before the +3.5 s point where a warm scan reports the ghost
burst and `43 05 30`. It then does a real panel power cycle (`FB_BLANK POWER DOWN` 1.586,
`DSI_CMD_SET_OFF` 1.706, `DSI_CMD_SET_ON` 1.838) and resets FTS + senses on at 1.860.
Our 0141 reinit is only an FTS reset: `SweetFramebuffer`'s display off/on never powers the
panel off, so every scan still runs in the same stale warm panel environment and dies at
+3.5 s. The `43 05 30` (MS invalid mutual strength) is an analogue/panel-state failure,
which is why rail-cycling the touch controller and resetting FTS cannot recover it.

**Next test 1 (no rebuild, cheapest).** Boot 0141 with
`cepheustouchdelay=55 cepheustouchpwr=0 cepheustouchrecover=0`, so the first calibration
happens after the iOS display/UI is up and after the framebuffer has taken over, skipping
the early bad calibration entirely. If touch works, the early scan poisons the chip and
the fix is to gate the first sense-on on display-on. If it fails, the panel environment is
the variable.

**Next test 2 (shim).** Cycle VCI (east TLMM GPIO 21) off/on plus panel reset before the
existing 34-command replay, then jump to XNU. The earlier replay only toggled panel reset
with VCI live; a real VCI POR is the part of TWRP's panel power cycle that replay missed.
Expect the screen to come back; if it does, check FTS with the same kernel.

### 0140 sense cycle failed; display-on reinit is next (2026-11-14)

**0140 warm run (`touch-0140`).** Kernel built with 0140
(`work/boot/kernel-brightness/kernel.development.sweet`, SHA-256 `9635bbb...`), booted
warm from TWRP 3.5.2. The sense-on line reports `cycle=1`: sense-on at 30.495 s, then the
usual failure: ghost multi-touch burst at 34.027-34.137 s, `early status failure` at
34.320 s (+3.8 s), clean recovery power cycle at 35.000 s, display on at 49.554 s, alive
through 90 s, **no real touch**. Logs: `logs/runs/touch-0140/`.

**Result.** The sense-on/off/on cycle is not the trigger either. The last TWRP difference
is its DRM notifier: on `DSI_CMD_SET_ON` it resets the FTS controller (reset
1.838-1.860 s) and sends sense-on (1.8604 s). XNU's display turns on at 49.554 s, long
after the first scan and recovery.

**Next: 0141.** `xnu/patches/0141-cepheus-fts-display-reinit.patch` makes
`SweetFramebuffer.cpp` bump `sweet_fb_display_on_count` in `kSweetFBRequestPowerChange`
when `!off`; the FTS thread polls it and, on a change, resets the controller
(`fts_reset`), waits for ready and sends sense-on (`a0 00 01`, no rail cycle), matching
TWRP's `fts_drm_state_chg_callback`. `cepheustouchdisplay=0` disables it, and the sense-on
line now ends `display=1`. Build on top of 0140, boot warm from TWRP, swipe from ~30 s to
~90 s: expect `SWEET FTS: display on 1; reset and sense on` around 49.5 s, and touch
should start after it. Run with `cepheustouchlog=1` to see the force-cal byte that follows
the reinit; the reinit itself logs regardless. If it still fails, rerun the same kernel
with `cepheustouchpwr=0 cepheustouchrecover=0` to remove every rail cycle and isolate the
display reinit.

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

**0140 (tested, failed).** `xnu/patches/0140-cepheus-fts-sense-cycle.patch` adds
`fts_sense_cycle()` (on, 2 ms, off, 50 ms, on) and uses it at init and recovery;
`cepheustouchsensecycle=0` keeps the old single sense-on, and the sense-on line ends
`cycle=1`. The warm test still failed (see the 0140 section above). The last TWRP step is
the display-notifier: a system reset + sense-on right after the panel's `DSI_CMD_SET_ON`
(TWRP 1.838-1.860 s; XNU display-on was 49.527 s in `touch-0139-pwr0`).

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
