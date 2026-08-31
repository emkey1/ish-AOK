# 19. Sockets and networking

A guest socket is a host socket. `socket(AF_INET, SOCK_STREAM, 0)` in the guest
becomes a real BSD socket in the application's own descriptor table, and every
subsequent `bind`, `connect`, `send` and `recv` is forwarded to it with the
arguments translated.

That one decision buys an enormous amount. Networking works — really works, not
in a simulated way. `apk` downloads packages over the device's LTE connection.
`ssh` reaches a real host. TLS negotiates against real servers with real
certificates. Nothing in AOK implements TCP, and nothing needs to.

It also means AOK inherits Darwin's networking, and Darwin is not Linux. This
chapter is about the seams: the places where a forwarded answer is the wrong
answer, and the one subsystem that exists purely because iOS takes sockets away
while you are not looking.

## 19.1 Three kinds of divergence

The differences sort into three kinds, and each needs different handling.

**Names and encodings.** A socket option that both systems have under different
names and shapes. `SO_BINDTODEVICE` takes an interface *name* on Linux; Darwin
spells the same intent `IP_BOUND_IF` and takes an interface *index*. That is a
translation problem, and the translation is `if_nametoindex`.

**Errnos.** Darwin returns errors Linux does not, in situations Linux handles
differently. `sock_translate_err` is the mapping layer, and Section 19.4 is
about the day it mapped too eagerly.

**Behaviour.** The hardest kind: both systems implement the same call, both
succeed, and the observable consequence differs. There is no translation for
this — the emulator has to change what it does.

## 19.2 A bound socket that would not refuse

Here is the clearest instance of the third kind.

On Linux, `bind` puts a socket in the bound hash and `listen` puts it in the
listening hash. A SYN arriving for a port that is bound but not listening gets an
RST, and the client's `connect` returns `ECONNREFUSED` immediately.

Darwin silently *drops* that SYN. The client retries and times out about eight
seconds later. And AOK inherited that, because a guest socket is a host socket.

Measured three ways — on the host directly, in the CLI guest, and on an iPad
with an external client:

| port state | Linux | AOK before |
|---|---|---|
| nothing bound | refused, 0.1 ms | refused, 0.1 ms |
| bound, not listening | refused, 0.0 ms | **timed out, 7.8 s** |
| listening | connected | connected |

Eight seconds instead of instantly is not a cosmetic difference. Service
discovery, health checks, and every "is anything on this port?" probe are built
on a refusal being fast.

The fix does not translate anything, because there is nothing to translate. It
changes what AOK does: a plain TCP `bind()` is validated against a throwaway
socket and then *remembered* rather than applied, and the real host `bind`
happens at `listen()`. The host never holds the port in the state that produces
the wrong behaviour.

> **How this was measured, and why it counts**
>
> The device measurement was taken with an external client **and a listening
> control** — a second port that was known-good, tested in the same run.
>
> The commit says why: "an earlier attempt at it had no control and was wrong."
> A network measurement without a control tests the network, the client, the
> Wi-Fi, and the code, and reports one number for all four.

## 19.3 An option that reported success it could not deliver

`SO_BINDTODEVICE` binds a socket to a specific network interface. AOK's
`setsockopt` refused it with `ENOPROTOOPT`, while its `getsockopt` reported
success with an empty name.

So a program that bound a socket to an interface and then checked was told the
bind had happened, when it never could. `ping -I lo0 127.0.0.1` failed outright
with "can't bind to interface" — the witness this was found by.

The implementation is a good example of what a careful translation looks like,
because three separate small decisions all had to be right:

- The name is resolved with `if_nametoindex` to Darwin's `IP_BOUND_IF` /
  `IPV6_BOUND_IF`. The guest sees the host's own interface names, so nothing
  else needs translating — a simplification worth noticing, and one AOK gets for
  free by not virtualizing the network.
- A name matching no interface is `ENODEV`, not `EINVAL`. "The request was well
  formed, the device is not there" — the errno carries which of those two it
  was.
- An empty name unbinds, as on Linux, and `getsockopt` reports the name it was
  given with the length Linux reports (including the NUL), and an empty name for
  a socket that was never bound. That last case is not hypothetical: it is what
  OpenSSH's routing-domain probe reads.

## 19.4 The error that AOK consumed

The most instructive socket bug in the tree is two bugs, and the second one is
about the emulator interfering with its own evidence.

> **The bug that taught us this**
>
> **First: a spin that would not end.** iOS kills connected sockets when the
> device sleeps. Reads then return `ENOTCONN`, and `sock_translate_err` mapped
> that to `ECONNRESET` — on every call, forever, because the host went on
> answering `ENOTCONN` while `sock_poll` went on reporting the descriptor
> readable. `chronyd` spun on it.
>
> The obvious fix is to report `POLL_ERR|POLL_HUP`. The entry records, as a
> correction to its own plan, that **this alone would not have worked**:
> `kernel/poll.c`'s `SELECT_READ` counts both of those as readable, matching
> Linux, so a `select`-based loop like chronyd's still wakes. What ends the loop
> is the *read* returning end-of-file. Both halves were needed, and only one of
> them had been planned.
>
> **Second, and unrecorded until it was measured:** a TCP peer resetting with
> `SO_LINGER 0` makes `recv()` report `ECONNRESET` on macOS and on Linux 6.12
> alike. An AOK guest agreed — *until it called `poll()` first*, after which the
> same `recv()` returned zero bytes.
>
> `SO_ERROR` is read-and-clear. AOK reads the host's `SO_ERROR` itself on every
> `sock_poll` of a stream socket, to decide whether a pending connect has
> completed. So AOK's own poll consumed the pending error, and the guest's
> subsequent read saw a clean end-of-file where it should have seen a reset.
>
> A stash for exactly this already existed — and was consulted only by
> `getsockopt(SO_ERROR)`. `read`, `recvfrom` and `recvmsg` consult it now too.

That second bug is worth sitting with, because its shape recurs anywhere an
emulator inspects host state on the guest's behalf. **A read-and-clear register
is destroyed by reading it.** The emulator's own introspection was not passive:
it consumed the very thing the guest was about to ask for, and it did so only on
the path where the guest had polled first — which is why the behaviour looked
like an intermittent, poll-dependent mystery rather than a missing stash.

## 19.5 sockrestart, or: iOS takes your sockets

Some code exists because of a platform decision rather than a design one. The
header of `fs/sockrestart.h` does not disguise its opinion:

> Hack to work around the idiotic way iOS handles suspending apps that have
> listening sockets. Basically the actual socket part of the file just gets
> freed, and the socket ceases to be a socket. Any attempt to do socket things
> with it will just immediately fail, and anyone blocked on accept will never
> wake up.

That is the platform behaviour, documented by Apple in TN2277 and not negotiable.
An application backgrounded with a listening socket comes back with a descriptor
that is no longer a socket, and every thread that was blocked in `accept` is
blocked forever on something that will never produce a connection.

For a terminal application with an `sshd` inside it, that is fatal to the entire
use case.

The workaround is bookkeeping plus reconstruction:

1. Track every listening socket, and every task blocked waiting on one.
2. On suspend, record the names and configuration of the listening sockets.
3. On resume, open new host sockets, reconfigure them identically, `dup2` them
   over the originals so every guest-visible descriptor number stays valid, and
   restart the waits.

The guest never learns any of this happened. A process blocked in `accept`
across a suspension simply continues to be blocked in `accept`, and the next
connection arrives normally.

It is worth naming what this subsystem really is: **an entire piece of the
kernel that exists because backgrounding is not transparent.** Chapter 17's
quiesce gate is the same tax paid in a different currency, and Chapter 28
collects the rest.

## 19.6 Unix sockets, and passing a descriptor

Unix-domain sockets are the one socket family AOK implements rather than
forwards, because both ends are guest processes and the names live in the guest
filesystem.

The feature that matters most is `SCM_RIGHTS`: sending a file descriptor over a
socket. In AOK that means duplicating a `struct fd` reference into another
task's descriptor table — an operation entirely internal to the emulator, with
no host equivalent involved.

It matters because it is a prerequisite for a whole class of modern software.
Wayland passes buffers as descriptors. systemd passes activated sockets to
services. Container runtimes and sandbox helpers pass descriptors as
capabilities. Chapter 42's Wayland work lists `SCM_RIGHTS` as one of the four
things that had to exist before any of it was possible.

## 19.7 Names, routes, and interfaces

The rest of the networking surface is smaller than it looks, because the host is
doing the work.

Name resolution goes through `/etc/resolv.conf` in the guest, which means a
freshly created root has *no* resolver configuration at all and every lookup
fails with "Temporary failure in name resolution" while networking itself works
perfectly — a confusing first ten minutes for anyone building a root by hand.

`fs/net_route.c` and the `/proc/net` files report interfaces, addresses and
routes, built from the host's own interface list. Because the guest sees the
host's interface names unchanged, `en0` in the guest is `en0` on the device,
which keeps `SO_BINDTODEVICE`, `ping -I`, and everything that reads
`/sys/class/net` (Chapter 18) consistent without a translation table.

## 19.8 What the host gives and what it costs

This chapter is the clearest case in the book of a recurring trade.

Forwarding to the host is what makes the feature real. Nobody implemented TCP,
congestion control, TLS, or DNS, and all of them work at full speed with the
device's own connectivity.

And every forwarded call inherits a decision made by a different operating
system for different reasons. Darwin drops a SYN where Linux sends an RST.
Darwin spells an option differently and takes a different argument type. Darwin
frees your socket when the app suspends. `SO_ERROR` clears when read, and the
emulator's own bookkeeping reads it.

The rule that falls out is the same one Chapter 14 arrived at from the poll
side, stated for a different subsystem: **the host is an implementation, not a
specification.** Where it agrees with Linux, forwarding is free. Where it does
not, the guest's contract is Linux's, and the emulator has to make up the
difference — sometimes with a translation, sometimes by changing what it does,
and once, in this chapter, by remembering an error before its own diagnostics
destroyed it.

---

*Anchors:* [fs/sock.c](../../fs/sock.c), [fs/sock.h](../../fs/sock.h),
[fs/sockrestart.c](../../fs/sockrestart.c), [fs/sockrestart.h](../../fs/sockrestart.h),
[fs/net_route.c](../../fs/net_route.c), [fs/proc/net.c](../../fs/proc/net.c),
[kernel/poll.c](../../kernel/poll.c),
[opt/AOK/docs/networking.md](../../opt/AOK/docs/networking.md),
[docs/TODO.md](../../docs/TODO.md) (the chronyd entry), Apple TN2277.

*Story:* a TCP reset that arrived correctly until the guest called `poll` first —
because `SO_ERROR` is read-and-clear and AOK's own poll implementation consumed
the error before the guest's `recv` could see it.
