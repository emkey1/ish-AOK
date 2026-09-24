# iSH pixman-accelerator LD_PRELOAD shim

Interposes pixman's public API (`pixman_image_composite32`, `pixman_fill`,
plus the property setters needed to know an image's state -- pixman has no
getter for transform/repeat/filter/alpha-map/clip, or for whether an image is
a solid fill or a gradient) so that plain FILL/COPY/OVER calls on 32bpp
a8r8g8b8/x8r8g8b8 surfaces run through the iSH pixman accelerator syscall
(ISH_SYS_PIXOP / kernel/ish_accel_pix.c) host-natively, instead of pixman's
own C implementation running instruction-by-instruction under emulation.
This is what cairo (GTK rasterization) and wlroots' pixman renderer (labwc's
own compositing) both sit on, so it speeds up both halves of the Wayland
desktop's steady-state rendering.

Requires the accelerator enabled: the app's Settings toggle "Pixman Accel
(Wayland rendering)", or `ISH_PIX_ACCEL=1` for the CLI. If unavailable, every
interposed function is a pure pass-through to real pixman -- so it is always
safe to load, on stock iSH, real Linux, or with the accelerator off.

## Measured (Phase 0, -O2 CLI build, arm64 guest, headless labwc + a GTK3
redraw-loop benchmark)
~23.5% of the interactive redraw window's wall time was inside raw pixman
calls (labwc + the app combined), consistently across repeated runs -- see
`pixman_accel_plan.md` at the repo root for the full methodology.

## Install
    sh build-shim.sh                # builds + installs to /usr/local/lib/ish-pixman
Needs only a C compiler: the system's pixman-1 headers are used when
installed, and otherwise the copy of `pixman.h` (0.44) vendored here.
`start-wayland.sh` exports `LD_PRELOAD` automatically when it finds the
built `.so` there -- no further steps once built.

## Statistics
    ISH_PIXMAN_STATS=1      counts to stderr at exit
    ISH_PIXMAN_STATS=/dir   counts to /dir/pixman-stats.<pid>, refreshed about
                            once a second and at exit (labwc and foot are
                            usually killed rather than exiting, and still
                            leave their numbers)
    ISH_PIXMAN_SHIM_OFF=1   never accelerate, keep counting: the "off" arm of
                            an A/B measured with the same instrument

Per process: time spent inside each interposed pixman entry point
(outermost call only), accelerated/declined counts with pixels, and every
composite SHAPE -- op, source, mask, destination, and the full set of reasons
it was declined -- with its call count, pixels and time. Rank the shapes by
time to decide what to accelerate next.

## Scope / limitations
- Accelerated, each checked byte for byte against real pixman
  (`tests/manual/pixman_accel.c` for the kernel, `tests/manual/pixman_shim.c`
  for the shim):
  - `pixman_image_composite32` / `pixman_image_composite`: SRC and OVER with
    an a8r8g8b8/x8r8g8b8 bits source, OVER through an a8 mask, and the same
    with a SOLID source (SRC or opaque OVER becomes a fill); onto an
    a8r8g8b8/x8r8g8b8 destination, clamped to its bounds and cut by its clip
    region (up to 64 rectangles), one kernel request per rectangle.
  - `pixman_fill` at 32bpp, `pixman_blt` at 32bpp (where the guest's pixman
    implements blt at all), and `pixman_image_fill_boxes` /
    `pixman_image_fill_rectangles` for SRC, CLEAR and OVER, reimplemented
    here instead of passed through (pixman's own builds a region per call).
- Small requests stay in the guest: fills under 1024 pixels and copies under
  512 are done by the shim itself (the same stores), because a syscall's
  share of jit->lock costs more than it saves when several threads of one
  process are drawing at once. OVER has no floor.
- Declines, to real pixman: gradient sources or masks, a solid mask, a clip
  on a source or mask, transform, non-NEAREST filter, repeat other than
  NONE, alpha map, component alpha, a source read outside its bounds, any
  other op, SRC through a mask, and a destination clip of more than 64
  rectangles in the composite's rect.
- The glyph and trapezoid entry points are interposed only to time them:
  pixman composites glyphs through its internal fast paths, which never come
  back through the shim.
- Must be built per guest arch and shipped in the rootfs (a rootfs-prep
  step, same as the crypto provider).
