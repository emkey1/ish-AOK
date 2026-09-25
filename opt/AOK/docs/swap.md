# Swap

iOS gives an app a memory ceiling and kills it without warning when it is
crossed. A guest that allocates too much therefore does not get an `ENOMEM` —
the whole of iSH-AOK disappears, terminals and all. Swap gives the emulator
somewhere to put pages the guest is not using, so that a large, mostly idle
working set can exist without spending real memory on it.

It is a file, not a partition, and it lives inside the app's own storage. Paging
to it costs flash writes on your device, which is why it is **off by default and
has to be turned on deliberately**.

There is a second, cheaper way to get the same headroom: compressed memory,
which holds cold pages in RAM instead of writing them out, and which can be
used with swap or entirely instead of it. If you only read one section of this
page, read [Compressed memory](#compressed-memory).

## Turning it on

In the **iOS Settings app** — not the settings inside iSH-AOK — open
**Settings → iSH-AOK → Simulated Swap**. There are two controls:

- **Enable Swap**, off by default.
- **Swap Size**, which starts at *Not chosen (swap stays off)* and offers
  256 MB, 512 MB, 1 GB, 2 GB, 4 GB, 8 GB and 16 GB.

**Both are needed.** Turning the switch on without choosing a size leaves swap
off, deliberately: picking a size for you would be iSH-AOK deciding how much of
your flash to write to. If that is the state you are in, the guest says so —
`cat /proc/ish/swap` reports it rather than just `off`.

**Both take effect the next time iSH-AOK starts**, not when you flip the switch.
The area is created when the guest boots, at its full size, and does not grow.

The sizes offered do not know how much room your device actually has. If there
is not enough free space for the size you picked, iSH-AOK leaves swap off rather
than quietly making a smaller area than you asked for — and again,
`/proc/ish/swap` says which of the two happened.

Pick a size the way you would on any machine: big enough to hold the cold parts
of what you run, small enough that you are not writing gigabytes to flash for
nothing. 256 MB or 512 MB is a reasonable starting point.

From the command-line build there is no Settings app, so use the environment
variable instead — see `ISH_GUEST_SWAP_MB` in
[tuning-knobs.md](tuning-knobs.md).

## External (USB) swap — experimental

**Off by default, and untested on a real drive as of 556.** Instead of the
area living in the app's own container, it can live in a `.aok-swap` file on
an attached USB drive, so paging never wears the device's own flash and the
area can be far bigger than internal storage would allow.

Turn it on in **Settings → iSH-AOK → External USB Swap → Swap to External
Drive**, then return to iSH-AOK: it asks you to pick a folder on the drive,
and the area is used there from the next launch. If the drive is not
attached at launch, **swap stays off for that launch** rather than silently
falling back to the container. To use a different folder, switch it off,
return to the app, then switch it on again — there is no way to change the
folder while it is on.

Worth knowing before you rely on it:

- **The file is unencrypted guest memory**, passwords and keys included. It
  is emptied when swap stops, but a kill leaves its contents on the drive
  until the next launch empties it.
- **Quit the app before pulling the drive.** Removing it while swap is active
  fails the pages that were on it, and any guest process whose memory that
  was crashes; the guest itself keeps running.
- **exFAT — most USB sticks — cannot guarantee preallocated space.** If the
  drive fills up while swap is active, the affected process crashes rather
  than the write silently failing.
- `swapoff` then `swapon` from inside the guest (where guest control of swap
  is allowed at all) re-enables it in the app's own container, not back on
  the drive, because the drive's file handle was already closed.

The command-line build has an equivalent for testing: `ISH_GUEST_SWAP_FILE=path`
alongside `ISH_GUEST_SWAP_MB` puts the area at that path instead of a
container temp file.

## Compressed memory

Compression is the other half of this, and it is worth turning on *before* swap
is: it buys much the same headroom without spending any flash at all.

In the **iOS Settings app**, under **Settings → iSH-AOK → Compressed Memory**:

- **Enable Compressed Memory**, off by default.
- **Compressed Memory Size**, the pool's ceiling. It is clamped to a quarter of
  the device's RAM however large you set it.

What it does depends on whether swap is on too, and you do not pick between the
two shapes — the combination decides:

- **Compression on, swap off.** The pool is the only storage there is. Cold
  frames are compressed and kept in RAM, and *nothing is ever written to
  storage*. A frame that will not compress simply stays resident, which is the
  correct answer. This is the shape Linux calls zram.
- **Compression on, swap on.** The pool sits in front of the file, and only
  what does not compress reaches flash. This is the shape Linux calls zswap.

**Why it helps is not the obvious reason.** iOS already compresses idle memory
for you, for free — but doing so does not move `phys_footprint`, which is the
ledger iOS kills the app on. Only compression iSH-AOK does itself, into its own
buffer with the original released, moves that number. So this is not
duplicating what the system already does.

It measures 2.2–2.8x on real workloads, so a 128 MB pool holds roughly 300 MB
of guest memory, and a page comes back in one to three microseconds depending
on the device — two orders of magnitude under a read from flash.

**Sizing is not the obvious rule either, and it is worth getting right.** The
swap *area* caps how much can be evicted at all; the *pool* caps how much of
what is evicted avoids flash. They are not alternatives. Making the area small
does not save writes — it stops eviction happening, so nothing reaches the pool
either. If you want compression to do the work, give the area room and let the
pool absorb it.

**The pool is resident memory**, so it competes with the thing it is saving.
That is why the quarter-of-RAM ceiling exists.

```sh
cat /proc/ish/zswap          # what it holds, the ratio, and the flash it saved
```

## Seeing what it is doing

The guest sees swap the way it sees anything else:

```sh
free                      # SwapTotal / SwapFree, as on any Linux
cat /proc/meminfo         # the same numbers, plus the rest
cat /proc/swaps           # the area as a device, like swapon --show
vmstat 1                  # si/so columns: pages in and out per second
```

`/dev/aokswap0` is the block device behind it. It is real enough to read and to
name in `/proc/swaps`, but it is the app's own file rather than a disk
partition, and it is not a general-purpose block device: it carries a swap
header and its data, and nothing else.

`swapon` and `swapoff` work on it, and answer as they would anywhere -- running
`swapon /dev/aokswap0` while swap is already on reports the device busy, as
Linux does. **`swapoff` tears the area down rather than parking it**, though, so
it is not a pause: once off, swap stays off until the guest boots again with the
switch on.

For what the pager itself is doing — how full the area is, how much has been
written, whether the clock is finding cold pages, whether it has had to pause —
read `/proc/ish/swap`:

```sh
cat /proc/ish/swap
```

Every counter there is described in [proc-ish.md](proc-ish.md).

## Things worth knowing

- **It is off by default, and that is deliberate.** Paging spends your flash.
  Nothing turns it on for you.
- **There is a write budget.** The pager stops evicting once it has written a
  fixed amount within 24 hours, so a runaway guest cannot quietly grind through
  the write endurance of your device. `/proc/ish/swap` shows the window and how
  much of it is spent.
- **It pauses itself when paging is not helping.** If pages come straight back
  after being evicted — thrashing — the background sweep backs off for a while
  rather than churning. That is the `thrashing` line.
- **Pages shared by a forked family are not evicted.** Only memory reachable
  from a single address space is paged out today, so a process that forked and
  did not exec keeps its shared image resident. Fork-heavy workloads therefore
  benefit less than the totals suggest.
- **Swapping is not free speed.** It buys you the ability to run something that
  otherwise could not run at all. Anything actively touched should stay
  resident, and if it does not you will feel it.
- **It does not survive a reboot of the guest.** The area is recreated empty.

## When it will not turn on

`/proc/ish/swap` says why rather than just reporting `off` — no space for the
area, a size of zero, or a build that was not offered guest control. If the
switch is on and the guest still reports no swap, read that file first.

## See also

- [tuning-knobs.md](tuning-knobs.md) — `ISH_GUEST_SWAP_MB` and the memory-guard
  knobs, for the command-line build.
- [proc-ish.md](proc-ish.md) — `/proc/ish/swap`, `/proc/ish/zswap` and the
  other emulator files.
- [ktop.md](ktop.md) — watching memory and paging live from inside the guest.
