# Reflection notes — session-transcript mining, 2026-07-02

Source: all 71 session transcripts in `~/.claude/projects/-Users-mke-git-ish-AOK/`
(Jun 12 – Jul 2, 2026), scanned by six extraction subagents (user messages,
corrections, command-frequency stats, tool-error counts), then clustered here.
Session ids are transcript basenames (first 8 chars).

Ranked most-leverage-first. Each cluster: the evidence, the recurrence, and a
verdict — **skill**, **automation/fix**, **CLAUDE.md fix**, or **nothing**.

---

## 1. Device & test-rig connectivity is volatile session state — fix with ssh aliases + CLAUDE.md

**Verdict: automation/fix (cheap, ~30 min) — highest ratio of pain to build cost.**

The iPad's IP has been hand-supplied, corrected, or lost in at least 10 sessions,
under at least 5 different addresses (100.80.152.34, 100.94.156.7, 100.80.152.3,
169.254.143.229, 169.254.184.173, 169.254.202.200, 169.254.107.119, 100.75.66.69):

- 39abcc7e: "Dude, you apparently lost track of the fact that the iPad's IP is 100.80.152.34. The ssh port is 1022"
- e94bbe8a: "iPad IP has changed to 169.254.202.200 FYI. Still port 1022"
- 74159734: "Sorry, that IP got truncated. 100.80.152.34, port 1022"
- 6489731a: "Sorry, port 1022 for ssh"; c523b77e: "Sorry, port 1022. My bad."
- f7346308: "please save that the app always is configured to start sshd on 1022"
- e7c5b4f9: "Try 100.75.66.69, apparently tailscale IP's can change"; ad-hoc intro of rigs m4t/m2t ("forgot to copy keys")

The long incantation `ssh -p 1022 -o BatchMode=yes -o StrictHostKeyChecking=no …`
was retyped **96×** in one session (e1221292) and 254× in another (6489731a).

**Proposal:** `~/.ssh/config` Host aliases (`ipad`, `mint`, `m2t`, `m4t`) carrying
port/user/options, so the only thing that ever changes is one `HostName` line —
plus a short CLAUDE.md "Test rigs" section listing the aliases and a discovery
rule ("if `ssh ipad` fails, ask the user for the current IP; update ssh config,
not the transcript"). Kills an entire class of [re-explain] and [correction].

## 2. "Commit and push" ritual + invisible-uncommitted-work loops — codify in CLAUDE.md

**Verdict: CLAUDE.md fix (near-zero cost). The single most repeated user message across the corpus.**

"Please commit and push" (and variants) was explicitly requested in **~30 of 71
sessions** — 65 mentions in one 12-session batch alone; 15× in c523b77e, 13× in
d11b90d1, 4-5× each in bfd1aaaa/ce06e4e3. Worse than the typing cost: the user
builds the iOS app in Xcode **from the committed tree**, so uncommitted or
wrong-branch work is invisible and produces wasted verification rounds:

- d11b90d1: "Sigh, you did this in a different branch, right? Because I see no difference. Please remove the instruction that tells you to do that and merge these changes into the main branch." + repeated "No change." / "Still borked"
- f096c167: "Sorry, please don't do that. Please stick to the main project."
- fa188b20: "Please commit the jit.c change if it hasn't been, and push it." (chasing)
- 616c33d4: "Have all pending changes been committed and pushed?"

**Proposal:** add to CLAUDE.md: *"After each validated change, commit and push
without being asked. For app/UI work, work on the user's main checkout branch —
never a side worktree — because on-device verification builds from the committed
tree."* (The worktree habit was explicitly revoked by the user in d11b90d1.)

## 3. Pasted-device-log / crash-dump triage — the de facto standing pipeline → skill

**Verdict: skill (`/triage-log` or similar). Most recurrent *workflow* in the corpus: ~18 sessions.**

The dominant intake format is a raw paste — an iSH `ERROR: illegal instruction
/ missing syscall NNN / needs full-width args` line, an lldb `thread backtrace
all`, a dmesg dump, an Apple-portal crash, or a UIKit exception — often one
issue per turn, serially:

- SIGILL/opcode: 15d75a9a (movq2dq), e666cd3f (pmullw), 94f51e84 (pinsrd), 9940cca5 (rep movsw), e26739f2 (cvttsd2si ×2), cf0c936d, 4e727039 ("More issues…" ×3-4)
- missing/full-width syscalls fed serially: 0820daaf, 172ae175 ("Thank. Next one…"), f7346308 (153/40/434)
- lldb backtrace-all pastes: b58d9228, b6fad0c9, 3d661528, 28574a54, f7327f87, e7c5b4f9, bfd1aaaa, 2425592a
- app/UIKit crashes: 53032a65, 7997ecc4, ce06e4e3 (Apple dev site, "can I somehow give you access to the developer site?")

The playbook is stable and currently re-derived every time: classify the paste →
decode opcode window / classify syscall arity (the MIN-arity rule is already in
memory) → build a musl repro with zig cc → run in local fakefs → **validate on
mint** → fix → commit+push. A skill would encode the decision tree and the
gotchas in one place and remove the per-session warm-up.

## 4. Guest-test harness ritual (fakefsify / ninja / zig cc) — script it + document the traps

**Verdict: automation (a checked-in helper script, e.g. `tools/testfs.sh`) + CLAUDE.md gotchas.**

The build→cross-compile→tar→fakefsify→run cycle is executed as raw Bash
hundreds of times per session: fakefsify ×304 and zig cc ×228 in 1cf00593,
fakefsify ×120 in 83a01950, ×91 in 0820daaf, ×72 in 4e727039, ×57 in 31cb6945;
ninja ×263 in 69e7272f, ×136/×124/×88 elsewhere. Known traps keep firing:

- fakefsify does NOT cleanly overwrite an existing target — "rm -rf first or it silently runs a stale fs" — re-stated ~20× in 9940cca5, again in b6fad0c9, 9314bd33; `fakefsify failed:` ×9 in a087f218, ×6 in 94f51e84
- fakefsify doesn't preserve the exec bit (e26739f2 workaround)
- Write-tool edits don't bump mtime → `ninja: no work to do` (9314bd33)
- `timeout` doesn't exist on macOS — bit again in 671e2b74 despite being in memory

**Proposal:** one small script wrapping "sources → static musl binaries → tar →
fresh fakefs (rm -rf built in) → run under ./ish", and a CLAUDE.md "Local guest
test harness gotchas" block for the four traps. Recurrence is extreme; build
cost is an hour.

## 5. Hand-authored dispatch/vetting briefs — turn the template into a skill

**Verdict: skill (e.g. `/vet-subsystem <name>` and a generic `/dispatch-brief`).**

The user hand-assembles long, near-identical structured kickoff prompts —
"Read these memories first… use the conform harness… oracle is REAL LINUX on
mint… validate tier0 + device… commit per the repo's usual flow" — for every
campaign resume and subsystem vetting round:

- Conformance-vetting template, verbatim skeleton: 285daa52 (process), 592e2756 (fs), a087f218 (signals), d22a200c (time), f1955b1e (sockets), e3e60341 (memory), fe7777b7 (paths)
- JIT-campaign resume briefings duplicating memory files: 616c33d4, 680ff402, 83a01950, 1cf00593 (pasting prior handoff summaries back in, twice)
- Bug-fix briefs with repro+validation: c6c6929f, cf0c936d, 22846d9d, 2de4742c, e94bbe8a, fa188b20 (device smoke test)
- f7346308: user explicitly asks Claude to author these — "please show me exactly what I should tell the other agent"

That's **~15 sessions** of duplicated authoring effort, and the briefs exist
precisely because memory recall alone hasn't been reliable (see #6). A skill can
load the right memory files and emit/execute the standard methodology, and a
`/dispatch-brief` variant can generate handoff prompts for other agents —
something the user already requests by hand.

## 6. The mint-oracle rule still slips — promote from memory to CLAUDE.md

**Verdict: CLAUDE.md fix (5 minutes).**

Despite the ⭐ MEMORY.md entry (which exists because of 15d75a9a: "please place
info on that system some place where you always see it on session start"), the
user had to police it repeatedly:

- 4e727039: "Also, you are checking these on the mint vm, correct? Check your memory…"
- 94f51e84: "Look for mint in your memory please."
- e26739f2: "If you haven't, please look up the mint system…"
- 74159734 / 616c33d4: "Keep in mind that mint … is available."

CLAUDE.md is loaded unconditionally; memory recall is not. A three-line
"Validation oracle" section in CLAUDE.md (mint = ground truth for CPU/JIT/syscall
work; never trust Rosetta) closes the gap.

## 7. Release cutting — recurring multi-step ritual with repeated pre-flight misses → skill

**Verdict: skill (`/cut-release`), medium cost, solid recurrence (~6 releases in 3 weeks).**

Sessions: 74159734 (532), 9e1d6989 (533), e2834582, 6489731a ("locate and run
the full release testing scheme"), 8adf9999 (work gated on release), a2405aa0.
Recurring failure modes that a checklist skill would catch:

- missing test files on device roots (74159734: fcntl_lock.c; e2834582: mount_flags.c — same class twice)
- release-notes content reconstructed from fallible recall ("I'm sorry, I meant lsof fixes"), format correction ("much less dense")
- signing surprises (9e1d6989: expired dev account, 18× SDK errors)
- e2834582: build script silently reverting IPHONEOS_DEPLOYMENT_TARGET ("find and destroy that")

Much of the recipe is already in `reference_release_process.md`; a skill would
execute it instead of re-reading it.

## 8. Permission-classifier friction blocks autonomous runs — allowlist the hot commands

**Verdict: fix (settings allowlist; the built-in `fewer-permission-prompts` skill does exactly this).**

- 1cf00593: **13×** "claude-opus-4-8 is temporarily unavailable, so auto mode cannot determine the safety of Bash/Write" — blocked the user's overnight (until-4AM) run
- 4e727039: 8× the same outage, left work unvalidated/uncommitted at session end
- 2425592a: 1×; plus scattered "[Request interrupted by user for tool use] … Continue please" (285daa52, 39abcc7e ×2, 996310db, 94f51e84)

The commands are utterly repetitive and safe: `ninja -C build`, `fakefsify`,
`zig cc`, `conductor.py`, `ssh mint`/`ssh ipad`, `scp -P 1022`, `git
commit/push`. An explicit allowlist removes the model-classifier dependency —
exactly what killed the overnight run.

## 9. Hand-rolled autonomous loops — point at `/loop` instead

**Verdict: fix/adoption (no build). The machinery already exists in the harness.**

- c523b77e: 28 ScheduleWakeup cycles, with the user pasting "Then schedule the following iteration (ScheduleWakeup 1800s)…" ~6× verbatim, and asking "do I need to start a separate session… give me some text to kick things off?"
- 1cf00593: overnight run with big safety preambles re-stated each cycle

The `/loop` skill (and spawn-task chips, which the user already uses — 28574a54,
a2405aa0, e2834582) covers this. Worth one line in CLAUDE.md so future sessions
reach for it instead of re-inventing the wakeup protocol.

## 10. On-device UI verification runs through the user's eyeballs — partial mitigation only

**Verdict: investigate, don't build yet.**

Every UI change costs a manual build→deploy→look cycle by the user, generating
long correction chains: f096c167 (4 rounds on keyboard accessory keys), d11b90d1
("Still broke… Still borked", ":-("), a2405aa0 (many screenshot-driven Music-applet
rounds), c1478aab ("is there a way to let you run the simulator?"). The CLI
build+sign recipe exists (reference_cli_build_and_sign.md) and 31cb6945 ran 24
simulator builds — so a **simulator-boot + screenshot loop** is plausibly
scriptable and would compress these sessions dramatically. But it's real build
cost and unproven for this app (terminal rendering, device-only paths), so:
prototype once before committing. The device-only bugs (wedges, first-responder
loss) can't be automated away regardless.

## 11. Discord/community bug relay — small, real, probably "nothing" for now

**Verdict: nothing (below build-cost threshold), revisit if volume grows.**

Sessions: a2405aa0 (Discord Q&A from screenshots), e26739f2 ("Same person,
different crash", "brief overview … that I can feedback to the user please"),
ce06e4e3 (Apple portal), 7825948a (issue-card triage "check if already fixed").
The variable part (the pasted evidence) is covered by cluster #3's triage skill;
the write-up-for-reporter step is a one-liner request. No dedicated tooling
warranted yet.

## 12. Mechanical tool friction — mostly harness-level; two cheap notes

**Verdict: nothing to build; two lines of CLAUDE.md if desired.**

- "File has not been read yet"/"modified since read" errors in ≥8 sessions (6× in 0820daaf, 7× in 1cf00593 — mostly memory-file writes)
- zsh-isms (`no matches found:` globs, `bad substitution`) in e3e60341, fe7777b7, e666cd3f
- wrong-cwd failures (`cd build`, `cd tests/remote`, git-from-build-dir) in 22846d9d, 1cf00593, 0820daaf
- sleep-block hook fights while polling background builds (e7c5b4f9 ×2, e94bbe8a) paired with user-side "Status please?" polling (83a01950 ×3, 616c33d4, e7c5b4f9)
- Mac pty exhaustion from leaked zsh processes (1cf00593, possibly sqz-hook related — worth keeping an eye on)
- context exhaustion/compaction on the big JIT sessions (671e2b74, 69e7272f, 83a01950, 9314bd33, c523b77e, d11b90d1) — sqz already mitigates; the remaining driver is raw log pastes, which cluster #3's skill would shrink

---

## Summary table

| # | Cluster | Recurrence | Verdict | Cost |
|---|---------|-----------|---------|------|
| 1 | Device/rig ssh coordinates drift | ~10 sessions, 5+ IPs | ssh config aliases + CLAUDE.md | tiny |
| 2 | Commit+push ritual / invisible work | ~30 sessions | CLAUDE.md policy | tiny |
| 3 | Pasted-log/crash triage pipeline | ~18 sessions | skill | medium |
| 4 | fakefsify/ninja/zig-cc harness + traps | 100s of invocations/session | script + CLAUDE.md | small |
| 5 | Hand-authored dispatch/vetting briefs | ~15 sessions | skill(s) | medium |
| 6 | mint-oracle rule slips | ~5 policing events | CLAUDE.md section | tiny |
| 7 | Release cutting | ~6 releases | skill | medium |
| 8 | Permission-classifier outages/prompts | 2 blocked runs, 20+ hits | settings allowlist | tiny |
| 9 | Hand-rolled autonomous loops | 2 big sessions | adopt /loop | tiny |
| 10 | UI verification via user | ~4 sessions | prototype sim-screenshot loop | large/unproven |
| 11 | Discord relay | ~4 sessions | nothing | — |
| 12 | Mechanical tool friction | diffuse | nothing / notes | — |

If you only do three things: **#1 + #2 + #6 (one CLAUDE.md edit + ssh config,
under an hour total)** remove the most-repeated corrections in the corpus; then
**#3 (triage skill)** and **#4 (harness script)** attack the two biggest
time-sinks in actual session wall-clock.
