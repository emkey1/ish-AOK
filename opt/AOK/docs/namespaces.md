# Namespaces: sandboxes inside the guest

From build 559 AOK's kernel has Linux's **user, mount, pid and network
namespaces**, beside the UTS and IPC ones it already had. That is what
sandboxing tools are built from — `unshare`, `nsenter`, bubblewrap (`bwrap`),
Flatpak-style launchers, systemd's `PrivateTmp=` and friends, and OpenAI's
Codex CLI, whose every shell command runs inside bubblewrap. They now start and
confine instead of failing with "Creating new namespace failed".

**This is new in 559 and experimental.** It reaches every permission check,
every pid lookup, the mount table and the socket layer, and has had days of
testing rather than months. Each behaviour was checked against Linux 6.12 first
and is covered by the guest test suite, but expect rough edges, and tell us
about them.

## Trying it

```sh
unshare -r id                                  # uid 0 -- root of a namespace of your own
unshare -m sh -c 'mount -t tmpfs none /mnt'    # a mount nobody outside sees
unshare -pf --mount-proc ps                    # ps sees itself as pid 1, and nothing else
unshare -n ip addr                             # one interface, lo, and it is down
bwrap --ro-bind / / --dev /dev --proc /proc --tmpfs /tmp --unshare-all sh
codex sandbox -- /bin/echo x                   # Codex CLI's sandbox: prints x
```

An ordinary user can do all of this, as on Linux: `unshare -r` (or `bwrap`
with `--unshare-user`) makes the user namespace first, and the others are made
inside it. `nsenter -t <pid> -U -m -p -n -u` joins a running sandbox.
`ls /proc/self/ns` lists the kinds there are.

## What each kind gives you

| namespace | what is private |
| --- | --- |
| user | your ids: uid 0 inside maps to your own uid outside, through `/proc/<pid>/uid_map` and `gid_map` |
| mount | the mount tree: mounts made inside stay inside, and `pivot_root` works — see [roots.md](roots.md) for propagation |
| pid | process numbers: the first process is 1, the rest count from 2, and nothing outside is visible or signalable |
| network | the network: `lo`, plus any `veth` and `dummy` links you add |
| UTS, IPC | the hostname; System V IPC objects and POSIX message queues |

There are **no cgroup or time namespaces**, and `/proc/<pid>/ns` has no file
for either, which is how `bwrap --unshare-all` and systemd know to skip them.

## Things worth knowing

- **Root in a user namespace is root of that namespace only.** It may change
  ids among the ones its maps cover, `chroot`, set the namespace's hostname,
  and mount `tmpfs`, `proc`, `sysfs`, `devpts` and FUSE in a mount namespace
  it owns. Mounts it inherited are locked: it cannot unmount them or make a
  read-only one writable. Anything the machine itself guards — device nodes,
  sysctls, other people's files — stays refused. Where AOK is unsure, it
  refuses more than Linux would rather than less.
- **A private network namespace is not a second network.** Nothing in it
  reaches the internet or the device's network — including the native `curl`
  and `wget`, which refuse from one. Inside it, once `lo` is up, servers
  listen, clients connect, and `ping 127.0.0.1` answers, all without touching
  the host. With iproute2 (`apk add iproute2` on Alpine; BusyBox's `ip` has no
  `veth`) you can link two sandboxes:

  ```sh
  ip link add va type veth peer name vb netns <pid>   # vb goes to <pid>'s namespace
  ip link add d0 type dummy
  ```

  Traffic crosses one veth between two private namespaces. A veth end in the
  machine's own namespace is refused ("RTNETLINK answers: Not supported"), and
  there is no forwarding, bridging or NAT beyond that one link.
- **systemd sandboxes for real now.** Services with `PrivateTmp=`,
  `ProtectSystem=`, `PrivateUsers=`, `PrivateNetwork=` and the like get the
  namespaces they ask for; before 559 that setup was quietly skipped.
- **A sandbox counts against your process limit.** `RLIMIT_NPROC` (`ulimit
  -u`) is charged at every level up to the machine, against the limit the
  sandbox's creator had, so raising it inside buys nothing.
- **A sandbox is not in a saved session.** A plain save refuses while any
  process is in a namespace of its own — that includes a systemd root whose
  services use private mounts. **Save Anyway**, and the automatic save, leave
  such processes out and save the rest (562). See [suspend.md](suspend.md).
- **`/AOK/fixes/codex` now undoes itself.** Before 559 it turned Codex's
  sandbox off; run on a newer build it takes that line out again. See
  [roots.md](roots.md).
