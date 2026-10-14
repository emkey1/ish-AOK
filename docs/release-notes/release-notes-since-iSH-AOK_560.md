# Release Notes Since `builds/iSH-AOK_559`

5 commits. This is an emergency fix for 559, released without the usual
release checks. On iOS and iPadOS 17 and 18, 559 could not make a hard link
on a root's filesystem, so `dpkg` and `apt` failed on Devuan.

## Before you update

**Saved sessions from 559 will not restore.** An image is tied to the build
that saved it. The guest boots fresh, and the resume picker marks such an
image "can't be restored".

## Fixed

**Hard links, and `dpkg`, on iOS 17 and 18.** 559 began asking iOS never to
follow a symlink anywhere in a path it hands the host. That protects a chroot
from a directory swapped for a link. But which calls accept the request
depends on the iOS release. `linkat` accepts it only from iOS 26, so on 17
and 18 every `ln` and `link()` failed with `Invalid argument`, and `dpkg`
stopped at:

    dpkg: error: error creating new backup file '/var/lib/dpkg/status-old': Invalid argument

The same request also broke `chown` before iOS 26, and stat, `chmod`,
`rename` and `rm` on iOS 15 and 16. On those releases, setting a file's times
followed a final symlink. AOK now checks once at startup which calls the
running iOS accepts the request for. Every other call does its work in the
parent directory, opened without following any link, so the protection is
the same on every release. Checked on iOS 17.7: `ln`, `mv`, `chmod`, `touch`
and `dpkg` work again.

## Also in this build

- `aok-update` on an AOK VM machine brings `setup-wayland-extras.sh`,
  `setup-xfce.sh`, `setup-wayfire.sh` and `select-desktop.sh`, which
  `setup-wayland.sh` already told you to run. On busybox's `sh` it no longer
  prints "can't open /proc/device-tree/chosen/aok,engine".
- `setup-games.sh` on a root without `apt` says that its SDL games are packaged
  for Devuan only. It points you to `setup-wayland-extras.sh games` for the
  card, board and puzzle games, which also works on Alpine.
