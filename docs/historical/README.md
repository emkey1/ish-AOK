# docs/historical

Documents that are **finished**, not documents that are wrong. Nothing here is
maintained, and nothing here should be edited except to add the header that says
so and a pointer to whatever superseded it.

The distinction that earns a file a place here: it was true when it was written,
someone may need to check it against what was believed at the time, and deleting
it would destroy that. A file that is simply out of date and helps nobody should
be deleted instead — an archive that accumulates everything stops being an
archive.

| file | what it is | superseded by |
| --- | --- | --- |
| [upstream-ish-changelog.md](upstream-ish-changelog.md) | upstream iSH's own TestFlight-era release notes, builds 33–48, cited by the book as a primary source | nothing; upstream stopped updating it before this fork existed |
| [build_553_musts.md](build_553_musts.md) | the deferred-work list written during the 552 release run, for build 553 | [build_555_musts.md](build_555_musts.md) |
| [build_554_musts.md](build_554_musts.md) | the deferred-work list written during the 553 release run, for build 554 | [build_555_musts.md](build_555_musts.md), which records that three of this file's ten open entries were already fixed and a fourth had a diagnosis that does not reproduce |
| [build_555_musts.md](build_555_musts.md) | the deferred-work list written during the 554 release run, for build 555 | `docs/build_556_musts.md`, which re-checked its six open entries on 2026-09-24 and found one (§4, POLLHUP) no longer diverging |
| [todo-closed-549-550.md](todo-closed-549-550.md) | the closed entries from `docs/TODO.md`'s 549 and 550 cycles, split out on 2026-09-07 when they had grown to two thirds of that file | nothing; they are closed |
| [amd64_port_plan.md](amd64_port_plan.md) | the 2026-04-08 pre-implementation bring-up plan for the amd64 guest | [docs/book/ch07-four-guests.md](../book/ch07-four-guests.md); amd64 shipped and its stated "Hard Blockers" no longer hold |
| [riscv64_guest_plan.md](riscv64_guest_plan.md) | the 2026-07-10 pre-implementation port plan for the riscv64 guest | [docs/book/ch07-four-guests.md](../book/ch07-four-guests.md) §7.5; riscv64 shipped, and `jit/riscv64_vendor_ext.c` (not this plan's proposed generic hook) is the reference implementation |
| [shell_transition_plan.md](shell_transition_plan.md) | the maintainer's 2026-09-10 decision to remove native bash outright and ship dash/zsh in its place | [README.md](../../README.md#native-bash-and-licensing) and [docs/llm_onboarding.md](../llm_onboarding.md); bash ended up disabled-by-default (`native_bash` meson option) rather than removed |
| [native_workspace_design.md](native_workspace_design.md) | the original native-only Workspace design (explicit Non-Goal: guest-side Wayland/X11) | [docs/roadmap.md](../roadmap.md)'s "556 -- the desktop is the product" section; the project later shipped exactly the guest Wayland desktop this ruled out |
| [wayland_workspace_plan.md](wayland_workspace_plan.md) | the Tier 1/2 plan for a guest Wayland desktop shown via a vendored noVNC/WKWebView/WebSocket bridge | [app/DisplayRFBClient.h](../../app/DisplayRFBClient.h); the top-level goal shipped but that specific transport was replaced by a native Metal RFB client before release |
| [suspend_full_resume_plan.md](suspend_full_resume_plan.md) | the 2026-09-12 design note for restoring app/Workspace UI state on top of a guest checkpoint | [docs/roadmap.md](../roadmap.md)'s "557 -- reach" section and [opt/AOK/docs/suspend.md](../../opt/AOK/docs/suspend.md); shipped as Suspend to Disk, confirmed 2026-09-27 |
| [apt_runtime_syscall_audit.md](apt_runtime_syscall_audit.md) | a syscall-gap audit for apt/helper-process startup, self-dated 2026-03-23 | [docs/book/appendices/appendix-c-syscall-coverage.md](../book/appendices/appendix-c-syscall-coverage.md); every gap it lists (`pidfd_send_signal`, `clock_adjtime64`, ...) has since been closed |
| [release-summary-iSH-AOK_547.md](release-summary-iSH-AOK_547.md), [_548.md](release-summary-iSH-AOK_548.md), [_553.md](release-summary-iSH-AOK_553.md) | a one-off "summary" companion file, written for only 3 of ~30 numbered releases | the corresponding `docs/release-notes-since-iSH-AOK_<N>.md` for the same build, which covers the same ground; see the release-process note below |

## The `build_<N>_musts.md` series

Each release run writes one: the work that release deliberately did **not** do,
with the diagnosis already made so the next person does not re-derive it. It is
named for the build that should do the work, not the build that wrote it — so
`build_554_musts.md` is written during the 553 run.

Exactly one is live at a time, at `docs/build_<N>_musts.md`. When the next one
supersedes it, the old one moves here with an "archived, not maintained" header
naming its replacement. They are kept rather than deleted because a musts file
records what was *believed*, and the next cycle regularly discovers that some of
it was wrong — 553 found that 552's list had described a live data-loss bug as a
performance gap. That correction is only legible with both files in hand.

## One release-notes file per release

`docs/release-notes-since-iSH-AOK_<N>.md` is the only release-facing document
the release process writes: one file per numbered build, named consistently,
with its own summary as the opening paragraph. A separate
`release-summary-iSH-AOK_<N>.md` was written for three builds (547, 548, 553)
as a condensed duplicate of the same content — an inconsistently-named,
inconsistently-produced second format that never became the practice for the
other ~27 releases. Those three are archived here rather than continued;
[docs/book/ch37-releasing.md](../book/ch37-releasing.md) states the one-file
rule explicitly so it does not drift again.
