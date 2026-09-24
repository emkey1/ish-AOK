# Pixman Composite Accelerator (Paravirt Provider) — Implementation Plan

Status (2026-09-24): **phases 0-2 DONE** (b2c97524, c2f0d45a); v2 mask
compositing, OVER_MASK_A8 (db6c4d57) and x8r8g8b8 as the destination
(2f0c587b) DONE; the app Settings toggle DONE ("Pixman Accel (Wayland
rendering)" in Settings, UserPreferences `kPreferenceEnablePixAccelKey`,
default off); `setup-wayland.sh` builds the shim best-effort. The first
real-client decline breakdown is in "v2 step 1" below: foot's glyphs still
decline (solid-fill source + clip, not the destination format), and the
accelerator made multi-threaded clients SLOWER until a JIT invalidation bug
was fixed (73b9112f, 082d9ac1). Phase 3's device number is in: on the M4
iPad labwc spends 3.4x less time in pixman and 56% less CPU, GTK 2.6x less
in pixman; drag frame rate does not move on the M4, and foot is still
slightly slower from jit->lock contention -- see "NEXT" at the bottom.
Owner: unassigned. Companion plan: `jit_code_cache_plan.md` (cold start;
NO-GO, unaffected by this plan). Direct precedent: the ChaCha20 crypto
accelerator (kernel/ish_accel_crypto.c + opt/AOK/crypto/ish_provider.c) —
same architecture, same lessons apply.

## 0. Progress so far

**Phase 0 (profiling):** built an LD_PRELOAD profiling-only shim
(scratchpad, not committed -- read-only wall-time instrumentation around
pixman's 4 public entry points) and a GTK3 redraw-loop benchmark (labwc +
a `Gtk.DrawingArea` doing translucent-rectangle + text redraws at ~60fps
target). Measured, 3 runs, on the local Arch aarch64 CLI guest: labwc +
the GTK app spend **~23.5% of the interactive redraw window's wall time
inside raw pixman calls** (~1.44s of 6.13s). Well above a working "is this
worth it" bar, though below the plan's originally-guessed 40% -- treated as
a clear GO given the precedent (crypto accel got 15-19x on a similarly-
sized share of ssh/scp time).

**Phase 1 (host core + differential harness): DONE, commit b2c97524.**
- `kernel/user.c`: `user_transform_rect` / `user_transform_rect_two`, new
  direct-host-pointer primitives generalizing `user_transform_two` from one
  linear buffer to a strided 2D sub-rectangle (declared in `kernel/calls.h`).
- `kernel/ish_accel_pix.h` / `ish_accel_pix_kernels.c`: pure pixel kernels
  (FILL/COPY/OVER) operating on already-resolved host pointers.
- `kernel/ish_accel_pix.c`: guest ABI (`struct ish_pix_req`), self-test-gated
  enable (`doEnablePixAccel`, `ISH_PIX_ACCEL=1`), decline logic (self-overlap,
  oversized, misaligned stride/base).
- `ISH_SYS_PIXOP = 0xacc1` wired into `kernel/calls.c` (both the dispatch
  switch AND the arm64/riscv64 range-check bypass gate -- missing the
  second one initially caused a `SIGSYS`, first bug found).
- OVER's blend arithmetic (premultiplied, saturating per-channel add,
  fast divide-by-255) was independently validated against real pixman
  *before* being written into the kernel (mint oracle, 200,125 cases, 0
  mismatches) -- caught that a naive non-saturating formula is wrong for
  malformed/non-premultiplied test inputs.
- `tests/manual/pixman_accel.c`: differential test, dlopen's real pixman
  as the oracle (SKIPs cleanly without it), covers FILL/COPY/OVER across
  tight/offset/padded/multi-page/full-1280x720-frame geometries + 30
  random-fuzz cases + the 3 decline paths. PASSES through the actual
  `setup-regressions.sh` harness. Two harness-only bugs were found and
  fixed during validation (both in the TEST, not the kernel -- confirmed
  by direct-syscall debugging before assuming otherwise): a hand-typed
  self-test expected value was arithmetically wrong, and the test's
  hand-derived `PIXMAN_a8r8g8b8`/`PIXMAN_x8r8g8b8` format constants had
  the wrong `bpp` field (32 bits, not 4 bytes) -- verified the correct
  values (`0x20028888`/`0x20020888`) against real pixman.h before fixing.
- Full existing regression suite reruns clean with the accelerator off
  (default): zero regressions from the new primitives/dispatch gate.
- Also confirmed empirically (not assumed) before writing the kernel:
  `pixman_fill`'s `_xor` parameter is a plain overwrite, not a real XOR;
  its `stride` parameter is in 32-bit WORDS while `pixman_image_create_
  bits`'s `stride` is in BYTES -- two different unit conventions in the
  same library, verified separately on the mint oracle.

**Phase 2 (guest-side LD_PRELOAD shim): DONE, commit c2f0d45a.**
`opt/AOK/tools/pixman/ish_pixman_shim.c` (note: lives under `opt/AOK/tools/
pixman/`, not `opt/AOK/pixman/` as originally sketched above -- the
fs/aok-tools.manifest baking mechanism only reads from `opt/AOK/tools/`).
Interposes `pixman_image_composite32` + `pixman_fill`, plus the five
property setters (`set_transform`/`set_repeat`/`set_filter`/
`set_alpha_map`/`set_clip_region(32)`/`set_has_client_clip`) needed to
shadow-track state pixman has no public getter for; `pixman_image_get_
component_alpha` DOES have a getter, so that one is queried directly, no
shadowing needed. Shadow entries live in a pointer-keyed hash table,
created lazily by the first setter call, purged when real
`pixman_image_unref()` reports the image was actually freed -- verified
empirically on the mint oracle first that it returns nonzero exactly on
the unref that hits refcount zero, never before, so a later `malloc()`
reusing the same address can never inherit stale state.

`setup-wayland.sh` now builds the shim (best-effort) to `/usr/local/lib/
ish-pixman/libish-pixman.so`; `start-wayland.sh` exports `LD_PRELOAD`
automatically when that file exists (`ISH_WAYLAND_DISABLE_PIXMAN_SHIM=1`
opts out). `fs/aok.c` needed a new `aokfs_tools_pixman_dir` node (mirroring
the existing `ktop` subdirectory node exactly) -- **trap discovered the
hard way**: `fs/aok-tools.manifest`'s generated-file table has NO automatic
subdirectory support; every subdirectory needs its own hand-wired
directory node in `fs/aok.c` (path string, `is_dir` membership, lookup-
table entry, and a readdir case scanning the generated table by path
prefix), exactly duplicating what `ktop/` already required. Also: the
manifest is read at **meson configure time** (`fs.read()` inside a
`foreach` in meson.build), so editing it and running a bare `ninja` is
NOT sufficient -- `meson setup --reconfigure` is required before new
manifest entries actually appear in the built `/AOK/tools` tree (a plain
`ninja` will silently keep serving the old file list).

**Verified end-to-end on the local CLI harness through the REAL
start-wayland.sh** (not a hand-rolled test): built the shim via
setup-wayland.sh's new step, ran a full session, confirmed `LD_PRELOAD`
was exported and picked up by real children. `ISH_PIXMAN_STATS=1` showed
genuine acceleration in two unmodified production Wayland clients:
- labwc itself: 3 composites + 31 fills accelerated, 2 mask + 35 format +
  3 bounds declines.
- **foot (the real terminal)**: 4948 fills accelerated against only 82
  mask declines -- a very high hit rate, since a terminal's cell-blit
  rendering is close to pure FILL/COPY with almost no masking, transforms,
  or scaling. This was a stronger, more convincing proof than the
  synthetic GTK bench would have been.

A separate visual (VNC-screenshot) sanity check was inconclusive -- the
screenshot came back a blank labwc background in BOTH a shimmed and an
unshimmed run of the same custom GTK bench script, so it's a pre-existing
test-environment issue (likely window placement/mapping in this specific
headless setup) unrelated to the shim; not chased further given the
stats + differential-test evidence already available. If picked up later,
worth a fresh look with a simpler test client (e.g. `foot` itself, whose
real session already proved to composite correctly per labwc's stats).

## v2 progress

**Mask support (`OVER_MASK_A8`): DONE, commit db6c4d57.** New kernel op,
`user_transform_rect_three` (three-image direct-pointer walk, mask at its
own bpp=1), pixel kernel `ish_pix_over_mask_row` -- blend formula (scale
src's premultiplied channels by mask_alpha/255, then ordinary OVER)
independently validated against real pixman on mint FIRST (300,625 edge +
random cases, 0 mismatches) before being written into the kernel. Shim's
`pixman_image_composite32` now routes OVER-with-a8-mask through it,
declining SRC-with-mask and any other op+mask combo (real pixman
operations, just not yet differential-tested). Two more harness-only bugs
found and fixed during validation (kernel correct throughout, confirmed
via direct-syscall debugging before assuming otherwise): (1)
`pixman_image_create_bits` requires EVERY stride to be a multiple of 4
bytes, even for a8 (1 byte/pixel) images -- an unaligned mask stride
doesn't error, it silently corrupts the image, which looked exactly like
a kernel bug until traced down. (2) A hand-typed self-test expected value
forgot every channel of premultiplied OVER blends identically, not just
alpha. Full existing regression suite reruns clean (all archs).

**KNOWN GAP surfaced by end-to-end testing**: re-ran the real
start-wayland.sh session with mask support live -- labwc's own compositing
still accelerates (3 composites + 31 fills), but **foot's masked glyph
composites still decline**, via `decline-format` rather than accelerating.
foot's terminal surface is very likely **x8r8g8b8** (opaque background,
no real alpha channel needed for a terminal), and v1 only supports
a8r8g8b8 as the DESTINATION format (x8r8g8b8 is only supported as a
*source*, where "ignore the top byte" is unambiguous). Supporting
x8r8g8b8-as-dst is deliberately still open -- it needs its exact quirks
nailed down empirically first (does pixman write 0xff to an XRGB dst's
top byte during a blend, leave it untouched, or blend it like any other
channel with whatever garbage was already there?), the same "verify
before implementing" discipline as every other part of this accelerator.
**This, not more mask coverage, is now the highest-value next step** --
it's specifically what would unlock real acceleration in foot, the
single most mask-composite-heavy real client measured so far.

**x8r8g8b8-as-DST support: DONE (2026-07-24).** Empirically nailed down on
the mint oracle FIRST (`xrgb_dst_check.c`, `xrgb_src_opaque_check.c`,
scratchpad, not committed) before any kernel change, per the project's
standing discipline:
- For a **non-opaque** src (real a8r8g8b8 alpha) composited OVER an
  x8r8g8b8 dst, pixman does NOT special-case the dst at all -- the dst's
  top byte is read and written as an ordinary alpha channel, byte-identical
  to the a8r8g8b8-dst math already shipped. Verified with the dst's top
  byte deliberately set to garbage (0x37) and to 0x00: the computed output
  alpha matched treating that garbage byte as a real input alpha in the
  standard OVER formula, both for plain OVER and OVER_MASK_A8.
- For a **fully-opaque src that is ALSO x8r8g8b8 format** (the
  `PIX_FLAG_SRC_OPAQUE` case) composited OVER an x8r8g8b8 dst, pixman takes
  a *different* fast path than the a8r8g8b8-dst case: instead of computing
  alpha=0xff and writing it, it does a literal word-for-word copy of src,
  **including whatever garbage is in src's own top byte**. This only
  happens when dst is ALSO x8-format; the same opaque src onto an
  a8r8g8b8 dst still gets a computed, real alpha=0xff written (verified
  side by side in `xrgb_src_opaque_check.c` -- same src, same op, only the
  dst format differs, and the two results differ in exactly the top byte).
  This is the one genuine semantic difference dst format makes, and it
  only matters for the plain OVER path (mask and non-opaque-src cases
  don't trigger it; COPY/SRC already does a literal copy unconditionally
  and was already correct).

Implementation, commit 2f0c587b: new `ISH_PIX_FLAG_DST_OPAQUE` guest-ABI flag;
`ish_pix_over_row` gained a `dst_is_opaque` parameter -- when both
`src_is_opaque` and `dst_is_opaque` are true it short-circuits to a raw
`memcpy` of the row instead of the per-channel blend formula, matching the
oracle exactly. `OVER_MASK_A8`/`FILL`/`COPY` are untouched (confirmed
dst-format-independent already). Shim's dst-format check widened to accept
`PIXMAN_x8r8g8b8` in addition to `PIXMAN_a8r8g8b8`, and now passes the new
flag whenever dst is x8-format. `tests/manual/pixman_accel.c` gained a
`dst_opaque` parameter threaded through `test_composite`/
`test_composite_mask`, with a full parallel set of x8r8g8b8-dst cases
(tight/offset/padded/full-frame/random-fuzz, both plain OVER and
OVER_MASK_A8) -- these initially caught the `opaque=1 dst_opaque=1`
combination failing (19/19 of that combination), correctly traced to the
kernel's real semantic gap (not a test bug, unlike every earlier round of
validation here) via a targeted single-case repro before writing the fix.
Full differential suite passes clean after the fix (0 failures, all
combinations including the new dst_opaque ones); end-to-end verified
through the REAL shim (LD_PRELOAD, ISH_PIXMAN_STATS=1) accelerating an
x8r8g8b8-dst OVER that a plain rebuild-forgetting run first showed
declining (a `ninja` rebuild was needed after editing the shim source --
the usual "did you actually rebuild" trap, not a design issue). This is
exactly the shape foot's masked glyph composites need; the real
end-to-end confirmation that foot itself now accelerates is still
pending (see NEXT).

## v2 step 1: real-client decline breakdown (2026-09-24)

A real session through start-wayland.sh on the Mac CLI (-O2 build,
`build/devuan-arm64-desk` clone: labwc 0.8.3, foot, waybar, l3afpad and
galculator; glibc, like the device), accelerator on, with the instrumented
shim (fd3997c1: every composite recorded as a shape -- op, src, mask, dst
and the full set of decline reasons -- with calls, pixels and time). The
workload: foot scrolling `find /usr/share/doc | head -3000`, window drags
(foot, l3afpad, galculator), l3afpad paging through GPL-3 and typing,
galculator clicks. Host load was 7-87 from other sessions' builds, so the
times are noisy; the shapes and counts are not.

**foot's masked glyphs still DECLINE.** Every glyph is
`OVER src=solid mask=a8 dst=x8r8g8b8`: the source is a
`pixman_image_create_solid_fill` colour, and foot has a clip region on its
buffer. x8r8g8b8 as the destination is no longer a reason. The old
"decline-format" was the solid source all along: `pixman_image_get_format`
answers PIXMAN_null for a solid fill.

Decline shapes ranked by time inside pixman (one run, ms):

| process | shape | reasons | calls | ms |
|---|---|---|---|---|
| labwc | SRC x8r8g8b8 -> x8r8g8b8 | clip | 722 | 1362 |
| labwc | SRC a8r8g8b8 -> x8r8g8b8 | clip | 928 | 1301 |
| labwc | SRC solid -> x8r8g8b8 | clip+src-kind | 5001 | 1005 |
| labwc | OVER a8r8g8b8 -> x8r8g8b8 | clip | 5601 | 463 |
| foot | (fill_rectangles entry, fills themselves accelerated) | per-call overhead | 18915 | 2437 |
| foot | OVER solid, a8 mask -> x8r8g8b8 (glyphs) | clip+src-kind | 4804 | 405 |
| l3afpad | pixman_blt (not interposed) | -- | 1554 | 627 |
| l3afpad | composite_glyphs_no_mask (not interposed) | -- | 731 | 196 |
| galculator | IN a8 -> a8 (cairo mask building) | op+format(+transform/repeat) | 1961 | 147 |
| l3afpad | OVER a8r8g8b8, SOLID mask -> a8r8g8b8 | mask-kind | 310 | 73 |
| galculator | OVER solid, a8 mask -> a8r8g8b8 | src-kind | 2008 | 63 |

What that says:
- **Clip on the destination is the biggest single gap**: about 80% of
  labwc's pixman time. wlroots clips every texture blit and every clear to
  the damage region. Supporting it means intersecting the composite rect
  with the dst clip's boxes (shadowed from set_clip_region32) and issuing
  one kernel request per box, ideally one syscall per call.
- **Solid-fill sources are second**: labwc's clears, every foot glyph, and
  GTK's text and rectangles. Needs the colour (shadowed from
  create_solid_fill) and a solid-source variant of FILL/OVER/OVER_MASK.
- **fill_rectangles/fill_boxes (NEXT #2) matters for foot**: its fills
  already reach the accelerator through pixman's internal pixman_fill
  call, but the per-call setup around them (region code, 78-pixel cell
  fills) costs 2.4 s. Interposing fill_rectangles/fill_boxes directly skips
  that.
- **pixman_blt (NEXT #2) matters for GTK**: 0.6 s in l3afpad.
- **SRC-with-mask and other op+mask (NEXT #3) do not**: 2 calls of SRC
  solid+a8 in the whole run, 2 of IN_REVERSE.
- Everything from step 4 of the brief (scaled blits, repeat=NORMAL,
  format conversion) is small: transform/repeat declines are GTK's gradient
  buttons and a few glyph edge cases, well under 100 ms.
- **wayvnc is the largest CPU consumer during a window drag** (1600-1950
  ticks per scripted run against labwc's 530-820), and none of it is in
  pixman -- it never calls pixman at all. That is neatvnc's own capture and
  encode, a separate target (plan item "wayvnc capture copies").

**The accelerator made foot SLOWER, and why.** Interleaved on/off runs of the
same scripted session (ISH_PIXMAN_SHIM_OFF=1 for the off arm, same
instrument): labwc's pixman time roughly halved (2.6/3.4 s on vs 5.6/6.2 s
off) and l3afpad's dropped (0.6/0.8 s vs 1.2/1.3 s), but foot's TRIPLED
(1.6/1.2 s vs 0.4/0.5 s). Per accelerated fill: 56 us against 9 us in
pixman. Not the syscall itself -- a single-threaded microbenchmark
(pixbench) shows 0.4 us per syscall and the accelerator winning at every
size from 1x1 up, 26x for a full-frame OVER. The cost was contention: four
threads doing small accelerated fills took 114-949 us per call. Sampling
showed them waiting in jit_cleanup_jetsam_after_interrupt, i.e. the JIT had
translations to free -- for a program that never writes code.
jit_invalidate_range walked the page-hash BUCKET (page % 1024), not the
page, so every kernel write through mem_ptr(MEM_WRITE) threw away the
translations of every code page sharing a bucket with any page it touched,
and every other thread then waited for the jetsam write lock. Fixed in
73b9112f (exact-page match). With it, 4-thread fills are 19-28 us per call,
and as a side effect OpenJDK runs a Java test program ~3x faster
(144/153 s -> 45/53 s). Remaining: small accelerated fills still scale worse
across threads than in-guest pixman (2.4 -> 9 -> 21 us for 1/2/4 threads
vs flat 2-3 us), from jit->lock taken per written page. A size floor in the
shim, or a cheaper "does this page have code" check, is the next lever;
tune it with the device numbers.

## Phase 3 device measurement, M4 iPad Pro (2026-09-24)

Same scripted session as the Mac A/B, driven from the Mac over the
Thunderbolt link (ssh + a VNC tunnel; start-wayland.sh run by the guest's
own user, stats in ~/pixab), with the shim built on the device by its own
gcc. Devuan 6 aarch64 root, 1280x720 headless output, accelerator on in
Settings; the off arm is the same shim with ISH_PIXMAN_SHIM_OFF=1. Runs
interleaved on, off, on, off.

**Build without the JIT fix** (built 12:29 BST from 565e231e -- the
accelerator as shipped until 73b9112f):

| per session | on | on | off | off |
|---|---|---|---|---|
| labwc: ms in pixman | 2261 | 2228 | 6086 | 6121 |
| labwc: CPU ticks | 479 | 468 | 782 | 774 |
| l3afpad: ms in pixman | 485 | 552 | 1005 | 1100 |
| foot: ms in pixman | 21106 | 22080 | 10246 | 7526 |
| foot: CPU ticks | 1630 | 1597 | 838 | 652 |
| drag fps (foot, l3afpad windows) | 37.5, 36.1 | 37.5, 36.0 | 37.9, 36.0 | 37.7, 35.2 |

- labwc: the accelerator takes its pixman time down 2.7x and its CPU by
  39%. The biggest single shape is the full-frame SRC x8r8g8b8 copy (924
  calls, 922k pixels each -- the capture for wayvnc): 230 ms accelerated
  against 4500 ms in pixman, 19.5x. What is left (2.2 s) is almost all the
  clipped shapes: SRC a8r8g8b8/x8r8g8b8 -> x8r8g8b8 with a clip (~1.4 s),
  clipped solid clears and clipped OVERs.
- foot: 2-3x WORSE with the accelerator, the JIT bug (73b9112f) on a 9-CPU
  device. An accelerated 78-pixel cell fill cost 695 us against 72 us in
  pixman, and foot's DECLINED glyph composites went from 320 us to 3.3 ms
  each -- the whole process pays for the translations every write evicted.
- Off, foot spends 5.1 s in fill_rectangles for 18k calls: 283 us per call,
  of which the fill itself is 72 us. pixman's own per-call setup around a
  one-cell fill is ~210 us on this device -- NEXT #4 (fill_rectangles/
  fill_boxes interposition) is worth more here than on the Mac.
- Drag frame rate did not move in any run (~36-38 updates/s): on the M4 the
  drag is not pixman-bound. The frame-rate test needs the A10X iPad Pro the
  plan names as the real target (`bip`), or the iPhone SE.

**Build with 73b9112f (exact-page invalidation only)**, built 15:02 BST:

| per session | on | off | on | off |
|---|---|---|---|---|
| labwc: ms in pixman | 5612 | 9600 | 7987 | 7230 |
| foot: ms in pixman | 2689 | 1229 | 2778 | 1458 |
| foot: CPU ticks | 183 | 139 | 223 | 166 |
| l3afpad: ms in pixman | 1518 | 1762 | 2788 | 1206 |

foot's CPU fell ~5x in BOTH arms (650-1630 ticks -> 139-223): the old bucket
invalidation was costing it even without the accelerator, through its own
syscalls and munmaps. But every accelerated kernel call got slower: the
full-frame copy 0.25 ms -> 3.6-8.7 ms, a fill 18 us -> 183 us. In isolation
(pixbench on the same device build) the copy is 124 us, so it was not the
kernel path. It was the fix: exact matching leaves the other pages' blocks
in the bucket, where the old code had emptied it on first use, so every
kernel write walked a full bucket per page touched -- dozens of blocks deep
with 1024 buckets and a process of labwc's size. Reproduced on the Mac
(labwc's accelerated copy 299 us pre-fix -> 3909 us with 73b9112f).

Fix: the page index grows with the code (one bucket per two blocks, power
of two, 1024 minimum), so a data page's bucket is almost always empty. Mac,
interleaved against the pre-fix binary: foot 240-250 ms in pixman against
1212-1612, labwc 1351-1411 against 1661-1996, accelerated fill 7-8 us
against 27-38, full-frame copy 159-170 us against 305-344; 4-thread small
fills 5.9 us against 1013; OpenJDK test program 35-43 s against 132 s, node
1.8-1.9 s against 5.3 s. Landed as 082d9ac1.

**Build with 082d9ac1 (growing page index) -- the Phase 3 number.** Built
16:23 BST; six sessions, interleaved on/off:

| per session | on | on | on | off | off | off |
|---|---|---|---|---|---|---|
| labwc: ms in pixman | 1786 | 1778 | 1744 | 6026 | 6062 | 6137 |
| labwc: CPU ticks | 329 | 324 | 324 | 735 | 742 | 749 |
| l3afpad: ms in pixman | 445 | 460 | 440 | 1167 | 1184 | 1215 |
| l3afpad: CPU ticks | 339 | 346 | 340 | 380 | 391 | 396 |
| foot: ms in pixman | 1587 | 624 | 1623 | 1241 | 1213 | 1236 |
| foot: CPU ticks | 181 | 72 | 189 | 154 | 148 | 159 |
| wayvnc: CPU ticks | 518 | 519 | 517 | 462 | 465 | 468 |
| drag fps, foot / l3afpad window | 36.5 / 30.8 | 37.2 / 31.0 | 37.1 / 31.4 | 37.3 / 31.4 | 37.5 / 31.6 | 37.5 / 31.1 |

- **labwc: 3.4x less time in pixman, 56% less CPU.** The compositor is
  what the accelerator was built for, and it delivers there.
- **l3afpad (GTK3): 2.6x less time in pixman**, 12% less CPU.
- **Drag frame rate is unchanged**: the M4 is not pixman-bound for a
  1280x720 drag (every run ~37 and ~31 updates/s). The frame-rate claim
  needs the A10X iPad Pro (`bip`) or the iPhone SE.
- **foot is still slightly WORSE** (the f3 run rendered fewer frames). Its
  declined glyph composites now cost the same in both arms (43 us), so the
  JIT damage is gone. What is left is its accelerated cell fills: 26-27 us
  each against 11 us in pixman. foot fills from several render threads at
  once, and every accelerated write takes jit->lock once per page it
  touches (mem_ptr's jit_invalidate_page). A host sample of 4 guest threads
  doing 64x64 fills: nearly every sample waiting on that mutex inside
  jit_invalidate_range. Device, 4 threads: the accelerator loses at 8x17
  (7.1 vs 0.96 us), 32x32 (12.1 vs 4.1), 64x64 (23.8 vs 13.1) and 128x128
  (50.5 vs 44.8), and wins from 256x256 (128 vs 173). Single-threaded it
  wins at every size. Fix: take jit->lock once per request for all the
  pages it will write, rather than once per page (NEXT #2).

## NEXT
1. Drag frame rate on a device that is pixman-bound: the A10X iPad Pro
   (`bip`) or the iPhone SE. The M4 is not.
2. One jit->lock per accelerated request instead of one per written page,
   so multi-threaded clients (foot) stop contending. If that is not enough,
   a size floor for small requests.
3. Clip-region support for the destination (most of labwc's remaining
   1.7 s on the device).
4. Solid-fill sources (labwc's clears, foot's and GTK's glyphs).
5. `pixman_image_fill_boxes`/`fill_rectangles` and `pixman_blt`
   interposition (foot's per-cell overhead; GTK's blits).
6. SRC-with-mask and other op+mask combinations: the data says not yet.

Each of 2-4 is validated against real pixman FIRST, with the differential
harness in tests/manual/pixman_accel.c.

## 1. Problem and evidence

The Wayland desktop's steady-state pipeline is software rendering all the way
down, all under JIT emulation:

    GTK/cairo raster (pixman) → wl_shm buffer → labwc composite
    (wlroots *pixman renderer*) → wayvnc framebuffer capture → RFB → applet

Every stage above the RFB link runs emulated scalar ARM code. The crypto work
proved the paravirt-provider pattern: a guest-side shim routes a hot,
well-specified operation through a private syscall to host-native code —
version-independent, whole-operation coverage, measured 15–19x for ChaCha20
(vs fingerprint-HLE which was rejected for crypto and measured near-neutral
on real workloads generally).

pixman is the single best target because BOTH heavy stages (cairo raster and
labwc/wlroots composition) sit on the same small public API of `libpixman-1`,
which both link dynamically on Arch and Devuan → one interposable seam
accelerates the whole desktop's pixel movement.

Unlike OpenSSL, pixman has NO provider/plugin API. Delivery is therefore an
`LD_PRELOAD` shim interposing pixman's public entry points, falling back to
the real library (`dlopen`/`dlsym(RTLD_NEXT)`) for anything not covered.

## 2. Hypercall interface (host side)

New op family alongside the crypto accelerator, same dispatch style
(calls.c intercepts the number BEFORE the syscall-table range check):

- `ISH_SYS_PIXOP` = 0xacc1, arg = guest pointer to `struct ish_pix_req`:
  - `op`: PIX_FILL, PIX_COPY (SRC), PIX_OVER (premultiplied OVER),
    PIX_OVER_MASK_A8 (OVER with an a8 mask — the glyph-blit shape) — v1 set.
  - per-surface descriptors (dst, src, mask): guest base pointer, stride
    (bytes, may be negative), format code (a8r8g8b8 / x8r8g8b8 / a8 for v1),
    width/height.
  - a box list (guest pointer + count) — one hypercall per composite call,
    batched over all boxes/rows, to amortize dispatch (crypto lesson:
    per-byte copy costs dominate before dispatch does; still, don't call per
    box).
  - `flags`: PROBE (feature/self-test query — the shim uses this to decide
    whether to activate, mirroring the crypto provider's decline path).
- Implementation `kernel/ish_accel_pix.c`:
  - direct guest-page access, NO bounce buffers — generalize
    `user_transform_two()` (kernel/user.c) to resolve dst (MEM_WRITE, COW
    honored) + up to two RO sources per row-span; rows are guest-contiguous so
    the walk is per-row per-page-span, same lockstep discipline as crypto
    (never hold a resolved pointer across the next mem_ptr).
  - pixel kernels in portable C, compiled -O2 (clang autovectorizes these
    trivially on arm64 host; measure before reaching for vImage/Accelerate —
    the crypto experience says plain -O2 C at direct pointers is already
    hundreds of MB/s, and vImage adds format-conversion constraints).
  - gated `doEnablePixAccel`, default OFF; `ISH_PIX_ACCEL=1` on CLI; lazy
    self-test (render a reference vector set and memcmp against baked-in
    expected output) exactly like the crypto selftest gate.
  - CRITICAL correctness rule: bit-exactness vs pixman for the covered ops.
    OVER/premultiply in pixman is defined on 8-bit lanes with exact rounding
    ((a*b + 127)/255 style); replicate pixman's arithmetic precisely, verified
    by differential tests, or decline the op. No "close enough" rendering —
    wayvnc damage tracking and user expectations both want determinism.

## 3. Guest shim (delivery)

`opt/AOK/pixman/ish_pixman_shim.c` → `libish-pixman.so` (per guest arch,
built in-guest by `build-shim.sh`, packaged like opt/AOK/crypto):

- Interposes (v1): `pixman_image_composite32`, `pixman_fill`, `pixman_blt`,
  `pixman_image_fill_boxes`/`fill_rectangles`.
- Uses only pixman PUBLIC accessors to inspect images
  (`pixman_image_get_format/data/stride/width/height`, repeat/transform/
  filter/alpha-map/clip queries). Accelerate only when: op ∈ {SRC, OVER},
  formats ∈ v1 set, no transform, no filter beyond nearest-identity, normal
  repeat=NONE, no alpha map, no component alpha, clip region representable as
  the box list. EVERYTHING else → `real_pixman_image_composite32(...)` via
  `dlsym(RTLD_NEXT)` — behavior identical to no shim.
- Probes `ISH_SYS_PIXOP` once at load; on ENOSYS/failed probe the shim
  permanently passes through (safe on stock iSH, real Linux, or accel-off).
- `ISH_PIXMAN_STATS=1`: per-op accelerated/declined counters + decline
  reasons dumped at exit — this drives v2 coverage the same way the HLE
  near-miss tracer was supposed to (but with exact call shapes, not
  prologues).
- Wiring: `start-wayland.sh` exports
  `LD_PRELOAD=/AOK/pixman/$(uname -m)/libish-pixman.so` when the file exists
  and a probe helper succeeds (tiny `/AOK/pixman/probe` binary, same pattern
  as the crypto provider's decline). Session-scoped only — never a global
  ld.so.preload, so a broken shim can't take out the whole guest; Reconnect
  with the toggle off gives a clean rollback.

## 4. Where the wins should land (validate in Phase 0)

- labwc composition: every damaged frame is OVER/SRC of window surfaces onto
  the output buffer (wlroots pixman renderer) — full-frame-scale pixel work
  at up to 60 Hz during drags/typing.
- cairo in GTK apps: widget fills, box blits, a8 glyph masks — exactly
  PIX_FILL/PIX_COPY/PIX_OVER_MASK_A8.
- foot is NOT pixman-based (its own renderer) — terminal-only sessions won't
  move; the target metric is GTK app interaction + window drag smoothness.
- wayvnc capture is memcpy-shaped (already partially HLE-able) — out of scope
  here, but the same 0xacc1 op family leaves room for a PIX_COPY-based
  fast path later if Phase 0 shows it matters.

## 5. Phases

### Phase 0 — profile the pipeline (1–2 days)
On-device (or CLI + VNC harness, which this session already built —
`rfb_drive.py` in the transcript): drive a window drag and a GTK redraw loop
while sampling the emulator host-side (`sample`/Instruments on Mac;
thread-name attribution of guest tasks) + guest `/proc/<pid>/stat` deltas for
labwc vs app vs wayvnc. Deliver: % of interactive-load CPU inside
pixman-shaped work per process. GO gate: labwc+app pixel work ≥ ~40% of
interactive load. Also microbench: guest pixman OVER of a 1280x720 frame
(cairo perf-like loop) emulated vs host -O2 C — sets the expected multiple.

### Phase 1 — host core + syscall + differential harness (1 week)
`ish_accel_pix.c` kernels (FILL/COPY/OVER/OVER_MASK_A8) + `ISH_SYS_PIXOP`
glue + selftest. Test `tests/manual/pixman_accel.c` (guest): generates
randomized surfaces/boxes (incl. negative strides, page-straddling rows,
overlapping src/dst for COPY — define as decline, pixman does), runs each op
BOTH via hypercall and via the guest's real libpixman, memcmp — 0 mismatches
over thousands of cases, both arches. This is the crypto differential
methodology transplanted.

### Phase 2 — shim + session wiring (1 week)
Shim + build script + packaging via fs/aok-tools-style manifest;
start-wayland.sh preload wiring + probe; STATS counters. Validation: full
desktop session on CLI harness with shim on — pixel-identical screenshots
(rfb_drive) for a scripted scene vs shim off; then labwc/GTK interaction
soak. Measure: window-drag frame rate over VNC + avahi-discover/bssh redraw
latency, shim on vs off, CLI and device.

### Phase 3 — device productization (0.5–1 week)
App Settings toggle (`doEnablePixAccel` + preference, default OFF; mirrors
the crypto/HLE cells), per-arch shim builds staged into the rootfs prep,
device measurement on the iPad (the real target: drag smoothness at 1280x720
on A10X), memory-of-record + release-notes entry.

### v2 candidates (driven by STATS decline data)
Nearest/bilinear scaled blits (media viewers), repeat=NORMAL patterns,
x8r8g8b8↔a8r8g8b8 conversion, wayvnc capture copies, a 16-bit lane OVER for
r5g6b5 if any surface actually uses it.

## 6. Risks
- **Coverage cliff**: if real traffic is mostly transformed/filtered
  composites, v1 declines everything and wins nothing — Phase 0's microbench
  plus Phase 2's STATS output make this visible early; v1's op set was chosen
  from what cairo/wlroots actually emit for untransformed UI.
- **Bit-exactness of OVER**: pixman's rounding must be replicated exactly;
  the differential harness is the enforcement. Any op that can't be made
  bit-exact gets declined, not approximated.
- **LD_PRELOAD fragility**: scoped to the Wayland session env only; probe
  fails closed; `dlsym(RTLD_NEXT)` fallback keeps ABI identical. Static
  pixman (rare; Alpine builds link dynamically too) simply never hits the
  shim.
- **Hypercall overhead on small ops**: batch boxes per call; decline
  composites under a size floor (e.g. <1–2 K pixels) where emulated code is
  already fine — tune with STATS + the Phase 0 microbench.
- **Security surface**: the request struct is guest-controlled — validate
  every stride/extent against the mapped region via the user_transform walk
  (which inherently faults cleanly on bad guest pointers), reject negative
  areas/overflow (64-bit math, explicit caps), and keep the kernels
  branch-simple. Same review bar as the crypto accelerator.

## 7. Effort
~3 weeks end-to-end. Phases 0–1 (~1 week) produce the decisive data and a
tested host core before any guest-visible integration exists.

## 8. Sequencing vs the code cache
Independent codebases (kernel/user.c + a new accel file vs jit/gen.c), so they
can proceed in parallel. If serialized: run BOTH Phase 0 measurements first
(2–3 days total) and let the numbers pick the first build-out — cold start
(code cache) and interactive feel (pixman) are different user-visible pains;
the Wayland experience needs the second one more once sessions are long-lived.
