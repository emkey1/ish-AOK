# Networking: why your server needs a port ≥ 1024

If you are running anything meant to accept connections from other machines
— `sshd`, a web server, a dev server — **listen on a port at or above
1024.** A privileged port will appear to work and won't be reachable.

## The rule

iSH-AOK never runs as root on iOS or macOS. Ports below 1024 are
privileged: the operating system refuses to let a non-root process bind
them. That refusal happens at the *host* level, underneath the emulated
kernel, so there is nothing the guest's own root account can do about it —
being `root` inside the guest doesn't make the app privileged outside it.

When a guest daemon binds a privileged port on the IPv4 wildcard address
(`0.0.0.0`), iSH-AOK doesn't fail the call. It quietly rebinds the socket to
`127.0.0.1` on an ephemeral port and reports success. The daemon starts
normally, `getsockname` still answers `0.0.0.0:22`, and nothing in the daemon's
own output looks wrong. `netstat` is the exception: its rows come straight from
the host socket, so a substituted listener shows there as
`127.0.0.1:<some high port>`.

The IPv6 wildcard (`::`) is *not* covered by that fallback — the substitution
only handles `AF_INET` — so a privileged `::` bind fails for real. A dual-stack
daemon therefore logs a permission-denied bind failure for its IPv6 half and is
left holding only the substituted IPv4 listener.

But that listener is only reachable through iSH-AOK's internal guest-to-guest
loopback NAT. Not from your laptop. Not from another device on the same
Wi-Fi. Not from anything outside the app.

## What to do instead

Pick a port ≥ 1024 and the wildcard bind succeeds for real — no
substitution, genuinely reachable from the network.

For `sshd`, edit `/etc/ssh/sshd_config`:

```
Port 2222
```

then connect with `ssh -p 2222 user@<device-ip>`. Anything else follows the
same pattern: change the listen port in the service's own configuration.

## How to tell it happened to you

The kernel logs a warning the moment it substitutes the bind:

```
WARNING: 947(sshd) bound 0.0.0.0:22 but iSH-AOK can't bind privileged ports as
non-root -- this socket is loopback-only, NOT reachable from the network.
Use a port >=1024 for anything meant to accept external connections.
```

If a service "starts fine but nothing can connect," run `dmesg` in the guest and
look for that line first — the emulated kernel's log goes into the ring buffer
your distro's own `dmesg` reads, so you do not need a Mac attached. `netstat
-ltn` is the other quick check: a listener the daemon believes is on
`0.0.0.0:22` that shows up there as `127.0.0.1:<some high port>` has been
substituted. It is the single most common cause.

## What is *not* affected

Binding a loopback address — `127.0.0.1`, or any `127.x.y.z` alias — is
fine at **any** port number, privileged or not. Those binds are also
substituted internally, but to something with identical reachability: a
loopback listener is only reachable locally either way, so nothing changes
for the guest and no warning is logged.

So a service you only talk to from inside the guest can use port 80 or 22
quite happily. The rule only matters when you want the outside world to
reach it.
