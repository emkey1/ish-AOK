# 18. The synthetic filesystems

Some files are not files. `/proc/meminfo` is a string built when you read it.
`/dev/null` is a rule about what happens to bytes. `/sys/class/net/eth0/statistics/rx_bytes`
is a counter formatted on demand. A pseudo-terminal is a pair of endpoints and a
line discipline. None of them have contents in the sense that Chapter 17's files
have contents, and all of them have to look exactly like files to a guest that
has never heard of any of this.

They also have a property that makes them unusually failure-prone: their
consumers are *tools*, and tools have opinions. Nobody reads `/proc/stat` for
pleasure. `top` reads it, and `top` has a parser, and that parser was written
against Linux.

## 18.1 procfs, and what a number means

`/proc` is a directory tree of generated content: one entry per process, plus
the system-wide files. AOK implements it as a table of entries with handlers —
`fs/proc/root.c` for the system files, `fs/proc/pid.c` for the per-process ones,
and dedicated files for `/proc/net` and the fork's own additions.

A single conformance sweep over it found five defects, and each is a small
lesson in what a synthetic file has to be.

**A zombie had no `/proc` entry at all** — absent from `readdir(/proc)`, `ENOENT`
for every file under it. That is precisely where `ps` gets the `Z` it displays,
and the only way a monitor learns that something exited and was never reaped.
Two independent things hid it: the lookup used the non-zombie path, and the
readdir walk skipped zombies explicitly. The handlers themselves had always been
ready — the state-character function takes a `zombie` flag and returns `'Z'` —
they simply never received one.

The fix has a distinction in it worth keeping: a zombie is now admitted, while a
task that is *mid-exit and not yet a zombie* is still refused, because its fields
are still moving. And the files that need a live address space — `exe`, `maps`,
`mem`, `fd` — keep the stricter guard, because Linux has nothing to show for
those either.

**`/proc/meminfo` printed bytes divided by 1000 and called it "kB".** A kB there
is 1024 bytes — Linux prints pages shifted by `PAGE_SHIFT - 10` — so every figure
was overstated by 2.4%, and `MemTotal` contradicted the guest's own `sysinfo(2)`,
which does use 1024. Anything comparing the two, or trusting `free(1)` against a
memory limit, saw memory that did not exist.

**`/proc/stat`'s `processes` field never moved.** It is a cumulative fork
counter that only grows; AOK reported the current live-task count instead, so
anything deriving a fork *rate* from it saw a flat line. It is a real counter
now, bumped in `task_create_`.

**And two file modes were lies in opposite directions.**
`/proc/self/oom_score_adj` was 0444 despite having a working write handler, so a
process that checked the mode before writing concluded it could not. The
converse case — a mode advertising permission that the handler does not
implement — is the same defect wearing the other sign.

That last pair is the theme of the chapter. A synthetic file has two contents: the
bytes it produces, and the metadata that describes it. Both are read, and both
have to be true.

## 18.2 Read what the consumer reads

The best-documented mistake in this area is worth telling in full, because the
work was careful and the result was still wrong.

> **The bug that taught us this**
>
> `btop`'s disk, network and I/O panels were empty. The recorded diagnosis said
> btop matches mounts to `/proc/diskstats` entries by device name, and that the
> two files disagreed — `/proc/diskstats` named `disk1`, `/proc/mounts` named
> the root `alpine-arm64-test`, so nothing tied a mount to a device.
>
> That was fixed properly. The invariant was verified in every file, on all four
> guest architectures. Then it was installed on an iPad and both panels were
> still empty.
>
> One command settles it:
>
> ```sh
> strings /usr/bin/btop | grep -E "^/(proc|sys|etc)/"
> /etc/fstab   /etc/mtab   /sys/block/{}/stat   /sys/class/net/   /statistics/
> ```
>
> `/proc/net/dev` does not appear anywhere in the binary. Neither does
> `/proc/diskstats`. The invariant that was verified was never what btop reads.
>
> The real causes were two, and unrelated to each other. **Disks come from
> `/etc/fstab`** — btop's `use_fstab` defaults to true, and a rootfs tarball has
> never heard of AOK's root, so Alpine's fstab declares a CD-ROM and a USB stick
> and nothing else. **Counters come from
> `/sys/class/net/<iface>/statistics/`**, which AOK did not have at all, so every
> interface read zero.
>
> With those fixed and btop's stock configuration unmodified: 926 GiB of disk at
> 32% used, and 2.88 MiB/s down against real traffic.
>
> The lesson, in the entry's own words: "the invariant was verified and the
> outcome was not." Running the tool once would have caught it in a minute.
> Running `strings` on it would have caught it before any code was written.

The fixes it produced are instructive in their own right. `ensure_root_fstab_entry()`
adds a root line to `/etc/fstab` when nothing declares `/` — conservatively,
only ever adding, and only when there is no entry at all. And the root device is
now `sda`, defined once in `fs/real.h` and used by `/proc/diskstats`,
`/sys/block`, `/sys/class/block` and `/proc/mounts` alike.

That replaced a deliberate earlier choice: `mount_root` used to report the
root's *directory name* so that `df` would not print a host path. It read well
and it named something no other file in the guest had ever heard of. `/dev/sda`
is the Linux answer and the one that makes tools work.

## 18.3 sysfs, and the daemon that waited thirty seconds

`/sys` gets the same treatment and for the same reason: modern userland reads it
constantly. `/sys/block` for devices, `/sys/class/net` for interfaces and their
statistics, and enough of the device model that `eudev` will start.

That last one is worth a sentence because of how it failed. eudev refused to run
with "sysfs not mounted", and behind that message was a thirty-second sleep — so
a guest booting a full init system sat there, apparently hung, for half a minute
before continuing without device management. The fix was not about containers or
namespaces, which is where the investigation started; it was about what eudev
checks for when it looks at `/sys`.

## 18.4 `/proc/ish`: asking the app about itself

The fork adds a directory that no Linux has:

```sh
cat /proc/ish/version        # iSH-AOK 1.3 (549)
cat /proc/ish/host_info      # the Mac or iPad underneath: OS, release, hardware
cat /proc/ish/ips            # this device's network interfaces
cat /proc/ish/colors         # the 16 ANSI colours, drawn -- a quick theme check
```

Two parts of it are more than diagnostics.

**`/proc/ish/defaults/`** is the app's preference store, one file per setting,
values as JSON. The entries are `0444 root:root`, so an ordinary user reads them
and gets `EACCES` on write — and **root can write them**, with the change taking
effect live and persisting exactly as though Settings had been used. Writes go
through the same validation the Settings screen uses, so a rejected value fails
the write rather than wedging the app, and removing an entry resets that
preference to its default.

**`/proc/ish/<arch>_jit_fuse`** is Chapter 6's measurement surface: the
instruction-fusion bits, readable and writable at run time, in the same variable
the translator consults.

Both exist for the same reason, which is worth stating as a design principle. An
app setting that can only be changed by tapping a switch cannot be scripted, and
a JIT flag that can only be set by an environment variable cannot be A/B'd
without relaunching. Exposing internal state as files makes it available to the
shell — and the shell is the whole product.

## 18.5 Devices without a `/dev`

There is no device model underneath any of this. A device node is an inode whose
mode says `S_IFCHR` and whose `rdev` says which driver — and the driver is a C
struct in `fs/dev.c`.

The standard set is small: `null`, `zero`, `full`, `random`, `urandom`, `tty`,
`ptmx`, `console`, `kmsg`, `fuse`. `fs/dyndev.c` adds dynamically registered
ones. Chapter 1 listed the repair loop that recreates them at every boot, because
a Docker-exported rootfs cannot ship real device nodes and arrives with a plain
regular file at `/dev/null` — which then silently accumulates every byte anything
ever writes to it.

A second family of nodes exists only in the app, and they are the sharpest
expression of this chapter's design idea: an iOS capability exposed as a device
node. `/dev/clipboard` reads and writes the system pasteboard.
`/dev/location` answers with the device's position. And `/dev/url` opens a
link — write to it and iOS acts as though the user had tapped one:

```sh
echo https://example.com > /dev/url
echo 'shortcuts://run-shortcut?name=Goodnight' > /dev/url
```

That last line is the point of it. Chapter 32's Shortcuts integration lets a
shortcut run a guest command; this is the return path, so anything automated on
the phone is reachable from a shell script. And the commit's own justification
for the shape is the argument of Section 18.4 restated: a device rather than a
command, because it "composes with redirection and pipes the way a shell expects,
and needs no binary in the guest filesystem". The command-line build has no iOS
to ask, so these three exist only in the app.

Two of the standard nodes are worth a note too. `/dev/kmsg` is the kernel log, which the
driver had always implemented and no rootfs had ever had a node for, so every
syslog daemon failed at startup on a file that was simply missing. And it is
created 0644 rather than 0666, matching Linux's 1:11 node, because an
unprivileged process may read the log but not write it — and a 0666 node would
let it open for writing and only then be refused, which is a worse failure than
being refused at open.

## 18.6 tmpfs, and an assert that could end the world

`fs/tmp.c` is an in-memory filesystem, and it is also where cgroup2 mounts are
handled well enough for systemd to accept them (Chapter 10's `cgroup_path`).

The thing worth recording about it is a class of bug rather than an instance:
tmpfs had asserts that could abort the whole application. In a normal kernel an
assertion failure in a filesystem is a panic, and a panic is bad but bounded —
the machine reboots. Here the "machine" is a terminal application, and an abort
takes the user's shell, their editor, their session, and any unsaved work with
it. Chapter 1 made this point with `/proc/meminfo` and the Mach calls that
degrade to best-effort values; tmpfs is the same lesson learned in the same way.

**In a system with no isolation, an assert is a decision to destroy the user's
session.** The bar for one is correspondingly higher.

## 18.7 Terminals

The tty layer is the largest synthetic filesystem in the tree by behaviour if
not by line count, because a terminal is not a file at all — it is a state
machine with two sides.

The line discipline is the interesting half: canonical mode with a line buffer
you can backspace through, the flags that mark where a line ended (and which you
cannot backspace past), `VLNEXT` as a one-shot latch making the next character
literal, word erase, `VMIN`/`VTIME` timers for non-canonical reads, XON/XOFF
flow control with the two-flag subtlety of Chapter 12, and the signal characters
that drive job control.

Pseudo-terminals (`fs/pty.c`) provide the same interface between two guest
processes, which is what `ssh`, `tmux`, `script` and every terminal multiplexer
need. And `fs/tty-real.c` connects the guest's terminal to the host's — the CLI's
stdin and stdout, or the app's own terminal view (Chapter 29).

The recurring difficulty is ownership. Which process group is in the foreground,
which session owns the terminal, what happens when the owner exits, and what a
descriptor opened before a hangup should see afterwards. Chapter 12 covered the
rules; the implementation detail worth adding here is that AOK models a hangup as
a *generation counter* rather than a flag, so a descriptor open at the time is
hung up and a fresh open of the same terminal gets a working one. Modelled as a
sticky flag, a hung-up console stayed dead forever — which is exactly what a
system console with no login prompt and no way to get one looks like.

## 18.8 The common thread

Every filesystem in this chapter is a *report* rather than a store. What it
contains is a claim about the state of the system, and the reader is a program
that will act on the claim.

That makes the failure modes distinctive. A wrong byte in Chapter 17 corrupts a
file. A wrong byte here corrupts a *decision*: `free` reports memory that does
not exist, `btop` shows an empty panel, a syslog daemon exits, eudev sleeps for
thirty seconds, a monitor never learns a process died.

Which is why the operational rule from the btop episode is the one to carry out
of this chapter, and it is two words longer than "read the docs": **read what the
consumer reads.** The file that is wrong is often not the file you were
improving.

---

*Anchors:* [fs/proc/](../../fs/proc), [fs/proc/root.c](../../fs/proc/root.c),
[fs/proc/pid.c](../../fs/proc/pid.c), [fs/proc/net.c](../../fs/proc/net.c),
[fs/proc/ish.c](../../fs/proc/ish.c), [fs/dev.c](../../fs/dev.c),
[fs/dyndev.c](../../fs/dyndev.c), [fs/tmp.c](../../fs/tmp.c),
[fs/fifo.c](../../fs/fifo.c), [fs/tty.c](../../fs/tty.c), [fs/pty.c](../../fs/pty.c),
[fs/tty-real.c](../../fs/tty-real.c), [fs/real.h](../../fs/real.h),
[kernel/init.c](../../kernel/init.c) (`ensure_root_fstab_entry`),
[opt/AOK/docs/proc-ish.md](../../opt/AOK/docs/proc-ish.md),
[opt/AOK/docs/tuning-knobs.md](../../opt/AOK/docs/tuning-knobs.md).

*Story:* btop's empty panels — an invariant carefully verified across four guest
architectures, and `strings /usr/bin/btop` showing in one command that the tool
reads neither of the two files that were made to agree.
