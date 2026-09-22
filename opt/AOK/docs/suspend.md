# Suspend to disk

Save the whole session and get it back on the next launch.

iOS ends this app routinely — memory pressure, or you swiping it away — and
without this that always loses the session. With it on you get the same one
back: same processes, same open files, same shell.

**It is off by default.** Turn it on in the iOS Settings app, under iSH-AOK →
**Suspend to Disk**. It spends storage, and a session it cannot describe is one
it will not save, so it is not something to switch on for somebody.

## Once it is on

Nothing to do. The session is saved when the app goes to the background and
comes back on the next launch.

To save one *now*, without waiting to be backgrounded:

- **Workspace**: the ☰ menu, **Save Session**.
- **iPad**, anywhere: the ⤓ button on the accessory bar above the keyboard. It
  turns into a checkmark when the session is on disk. It appears only while
  Suspend to Disk is on.
- **Workspace → Utilities… → Workspace → Sessions** has the same thing on a
  card, with the last save's size and process count.
- From the shell:

    /AOK/tools/suspend.sh

That writes the session and stops the machine, so the next launch resumes it.
There is also `--save FILE`, which takes a copy and lets the session carry on,
and `--status`, which reports what the last attempt did.

The control underneath is a `/proc` file, like every other AOK knob:

    echo suspend             > /proc/ish/checkpoint
    echo save /host/path     > /proc/ish/checkpoint
    cat /proc/ish/checkpoint

`cat` is worth reading. Above the blank line is an inventory of what this guest
is holding that a save would have to deal with — how many processes, how many
descriptors, and how many of them are the awkward kind. Below it is what the
last attempt actually did.

## What comes back

Every process and its place in the tree, with its pid unchanged. A shell
blocked in `wait` and the child it is waiting for both resume. A pipeline comes
back with the bytes still in it. A file comes back at the offset it was read to,
and two processes that shared a descriptor still share it.

Your zsh session comes back with its variables, functions and aliases. A native
program is host code on a host thread, so it cannot be photographed the way an
emulated process can — instead it is asked to write down its own state, and zsh
knows how. It is **re-launched** from that description rather than resumed
mid-instruction, which for an interactive shell means a prompt with your session
still in it.

The terminal comes back too, and it is the one you are looking at. A terminal
cannot be restored -- the one you had belonged to an app process that no longer
exists -- so a fresh one is made and the session is re-attached to it, with the
same session, the same foreground job and the same line settings. In the app
that means the window you resume into is your session, not a new shell beside
it. Your hostname, your background jobs and your job table are all still there.

The system consoles are re-attached the same way, each to its own: a getty on
`tty3` comes back on `tty3`, not alongside everything else on the console.

The clocks come back the way Linux's do after hibernation. `uptime` keeps
counting, the time the session spent saved included, and the boot time stays
what it was; `CLOCK_MONOTONIC` carries on from where it stopped. So a program
asleep until a deadline -- Python's `time.sleep`, a timer, a condition
variable -- wakes when it should, not "uptime at the save" late.

Timers come back too: `alarm()`, `setitimer`, `timer_create` and timerfds,
each with the deadline it had, on the clock Linux would count it on. One on
`CLOCK_MONOTONIC` -- which is where a relative timer and `alarm()` live --
does not count the time the session spent saved; one on `CLOCK_BOOTTIME`, or
set for a wall-clock time, does, and comes due that much sooner. A `sleep 5`
or a `select()` timeout the save interrupted sleeps what it had left, and a
signal sent but not yet taken is still waiting, with everything it carried.
Two limits: other timed waits -- a futex or `sigtimedwait` timeout, a
socket's receive timeout -- start their timeout over, and the CPU-time clocks
start again from zero (a CPU-time timer still keeps the CPU time it had left).

## What it will not save, and how it tells you

**A native program does not stop a save.** `/AOK/native/zsh` can describe
itself and comes back exactly where it was. The others — bash, dash, the
editors — cannot, so they are **re-launched from their command line** instead.
For a shell sitting at a prompt that is the same thing. For one part way
through a script it means the script runs again from the top, so the save says
which programs those were, in `/proc/ish/checkpoint` and in the confirmation.

This used to be a refusal, and it was the wrong trade: the alternative to a
degraded restore is not a perfect one, it is no restore at all, because iOS
kills the app either way.

**A save never damages the session you already had.** The image is written to a
temporary file beside the target and renamed into place only once it is whole,
and fsynced before it becomes visible. So a save that is refused, or an app that
is killed part way through one, leaves the previously saved session exactly as
it was. That matters because the header is written first: a half-written file
would otherwise still look like a real saved session in the picker, and then
fail part way through resuming.

It still refuses rather than writing something that will not come back. Every
refusal names the process and the reason, in `/proc/ish/checkpoint`:

- **A descriptor with no rule for rebuilding it.** Regular files, directories,
  terminals, pipes, sockets and the standard streams all have one.
- **A native program that is not making any system calls**, because there is
  nowhere to stop it. A shell at a prompt is waiting on a read and is fine; a
  native program in a tight compute loop is not.

A refusal costs nothing: the session carries on exactly as it was. A save is a
copy, and the machine is stopped only for as long as it takes to write one.

**A resumed Workspace session brings its screen back with it.** What each
terminal had printed -- the screen and the scrollback -- is carried across with
the arrangement and written back before the session starts, so a resumed window
looks like the one you left rather than an empty box.

Two limits worth knowing. The text comes back, the **colour does not**: the
terminal cannot hand its attributes back, so old output is restored in plain
text. And in full-screen shell mode (outside Workspace) the screen is not yet
carried, so a resumed terminal there is blank until you press Return -- nothing
has been lost, the shell simply has no reason to reprint a prompt it already
printed.

**Sockets are rebuilt, not copied.** A socket belongs to the process that owns
it and cannot outlive it -- on iOS it does not even outlive a suspension, since
the system tears connected sockets down while the app is frozen. So what
travels is a description complete enough to build one again:

- A **listening** socket comes back listening, on the same address and with the
  same backlog. A guest running `sshd` could not be saved at all before this.
- A **bound** socket comes back bound, and one that was never bound or
  connected comes back as a plain new socket.
- A **netlink** socket is rebuilt exactly, port id included. AOK emulates these
  end to end, so there was never a host object to lose.
- A **connected** socket comes back **hung up**: reading it gives end-of-file
  and writing gives `EPIPE`, which is what every program already handles as
  "the peer went away" -- and what did in fact happen. Unix-domain sockets come
  back this way too for now, because putting a bound one back means recreating
  its node in the filesystem.

That is a session that resumes with some connections dropped, rather than no
session at all; refusing never preserved a working connection, because iOS
destroys it during the suspension regardless.

An image from a **different build** is refused on the way back in. The register
file travels as bytes, and reinterpreting one from another build would be worse
than declining it — so after an update, the first launch boots normally.

## What it is not

It is not a snapshot of your filesystem — that is
[`/proc/ish/snapshot`](proc-ish.md), and the two are independent. A session
saved against a root you have since changed will resume against the changed
one, exactly as it would if the app had simply stayed running.
