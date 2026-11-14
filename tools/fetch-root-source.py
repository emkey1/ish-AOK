#!/usr/bin/env python3
"""Fetch the corresponding source for the GPL software in a shipped root image.

docs/gpl-source-offer.md promises anyone who asks the complete corresponding
source for the binaries in a given build. This is how that promise is kept:
point it at the same image tools/root-manifest.py described, and it resolves
every binary package back to its source package and downloads it.

    tools/fetch-root-source.py devuan-minirootfs-6.0-aarch64.tar.xz --dry-run
    tools/fetch-root-source.py devuan-minirootfs-6.0-aarch64.tar.xz -o source/

Resolution is exact rather than guessed. The archive's own Sources index gives
the source package's version, its pool directory and a SHA-256 for every file,
so nothing here constructs a URL from a binary version -- which would be wrong
for a binNMU (bash is 5.2.37-2+b10 as a binary and 5.2.37-2 as source) and for
anything carrying an epoch.

Alpine is a different shape: an apk's source is its APKBUILD in aports plus the
upstream tarball that fetches. There is nothing to download package by package,
so for an Alpine image this prints the aports reference to record instead.
"""

import argparse
import hashlib
import io
import lzma
import os
import re
import sys
import tarfile
import urllib.request

APK_DB = "lib/apk/db/installed"
DPKG_DB = "var/lib/dpkg/status"
SOURCES_LIST = "etc/apt/sources.list"
ALPINE_RELEASE = "etc/alpine-release"


def read_from_image(path, wanted):
    found = {}
    with tarfile.open(path, "r:*") as tf:
        for member in tf:
            if not member.isfile():
                continue
            name = member.name.lstrip("./")
            if name in wanted:
                found[name] = tf.extractfile(member).read().decode("utf-8", "replace")
                if len(found) == len(wanted):
                    break
    return found


def binary_packages(status_text):
    """Every binary package, mapped to the source package that produced it.

    dpkg's `Source:` is absent when it equals `Package:`, and carries its own
    version in brackets when the source version differs from the binary one --
    which is exactly the binNMU case.
    """
    out = {}
    for block in status_text.split("\n\n"):
        name = re.search(r"^Package: (.+)$", block, re.M)
        version = re.search(r"^Version: (.+)$", block, re.M)
        if not (name and version):
            continue
        source = re.search(r"^Source: (\S+)(?:\s+\((.+?)\))?\s*$", block, re.M)
        if source:
            src_name = source.group(1)
            src_version = source.group(2) or version.group(1).strip()
        else:
            src_name, src_version = name.group(1), version.group(1).strip()
        out.setdefault(src_name, src_version)
    return out


def archive_and_suite(sources_list):
    """The first ordinary deb line: (base url, suite, [components])."""
    for line in sources_list.split("\n"):
        line = line.strip()
        if not line.startswith("deb ") or line.startswith("deb-src"):
            continue
        parts = line.split()
        if len(parts) >= 3:
            return parts[1].rstrip("/"), parts[2], parts[3:] or ["main"]
    return None


def fetch(url, timeout=120):
    request = urllib.request.Request(url, headers={"User-Agent": "root-source-fetch"})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.read()


def sources_index(base, suite, component):
    url = "%s/dists/%s/%s/source/Sources.xz" % (base, suite, component)
    sys.stderr.write("reading %s\n" % url)
    return lzma.decompress(fetch(url)).decode("utf-8", "replace")


def parse_index(text):
    """name -> {version, directory, files: [(sha256, size, filename)]}."""
    out = {}
    for block in text.split("\n\n"):
        if not block.strip():
            continue
        name = re.search(r"^Package: (.+)$", block, re.M)
        if not name:
            continue
        version = re.search(r"^Version: (.+)$", block, re.M)
        directory = re.search(r"^Directory: (.+)$", block, re.M)
        files = []
        in_sha = False
        for line in block.split("\n"):
            if line.startswith("Checksums-Sha256:"):
                in_sha = True
                continue
            if in_sha:
                if not line.startswith(" "):
                    break
                bits = line.split()
                if len(bits) == 3:
                    files.append((bits[0], int(bits[1]), bits[2]))
        if version and directory:
            out[name.group(1)] = {
                "version": version.group(1).strip(),
                "directory": directory.group(1).strip(),
                "files": files,
            }
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", help="a rootfs tarball")
    ap.add_argument("-o", "--out-dir", help="where to put the source packages")
    ap.add_argument("--dry-run", action="store_true",
                    help="resolve everything and report, downloading nothing")
    args = ap.parse_args()

    found = read_from_image(args.image, {APK_DB, DPKG_DB, SOURCES_LIST, ALPINE_RELEASE})

    if APK_DB in found:
        release = found.get(ALPINE_RELEASE, "").strip() or "unknown"
        print("%s is an Alpine image (%s)." % (os.path.basename(args.image), release))
        print()
        print("An apk's corresponding source is its APKBUILD in aports, plus the")
        print("upstream tarball that APKBUILD fetches. There is nothing to download")
        print("per package; record this instead, which identifies both exactly:")
        print()
        print("  git clone https://gitlab.alpinelinux.org/alpine/aports.git")
        print("  git -C aports checkout v%s" % release)
        print()
        print("The package list is in the root manifest beside this tool.")
        return 0

    if DPKG_DB not in found:
        raise SystemExit("%s: no dpkg or apk database inside" % args.image)

    wanted = binary_packages(found[DPKG_DB])
    archive = archive_and_suite(found.get(SOURCES_LIST, ""))
    if archive is None:
        raise SystemExit("%s: no deb line in /etc/apt/sources.list" % args.image)
    base, suite, components = archive
    print("archive %s, suite %s, components %s" % (base, suite, " ".join(components)))
    print("%d binary packages from %d source packages" %
          (found[DPKG_DB].count("\nPackage: ") + 1, len(wanted)))

    index = {}
    for component in components:
        index.update(parse_index(sources_index(base, suite, component)))
    print("index holds %d source packages" % len(index))

    missing, jobs, total = [], [], 0
    for name, version in sorted(wanted.items()):
        entry = index.get(name)
        if entry is None:
            missing.append("%s (%s)" % (name, version))
            continue
        for sha, size, filename in entry["files"]:
            jobs.append((sha, size, "%s/%s/%s" % (base, entry["directory"], filename), filename))
            total += size

    print("resolved %d source packages, %d files, %.1f MB"
          % (len(wanted) - len(missing), len(jobs), total / 1048576.0))
    if missing:
        print("NOT in this archive's index (%d): %s" % (len(missing), ", ".join(missing)))
        print("  Those are usually packages the image carries from another suite;")
        print("  they still have to be accounted for before the offer is honoured.")

    if args.dry_run:
        for _, size, url, _ in jobs[:10]:
            print("  %9d  %s" % (size, url))
        if len(jobs) > 10:
            print("  ... and %d more" % (len(jobs) - 10))
        return 1 if missing else 0

    if not args.out_dir:
        raise SystemExit("need -o/--out-dir unless --dry-run")
    os.makedirs(args.out_dir, exist_ok=True)
    failures = 0
    for sha, size, url, filename in jobs:
        dest = os.path.join(args.out_dir, filename)
        if os.path.exists(dest) and os.path.getsize(dest) == size:
            continue
        sys.stderr.write("%s\n" % filename)
        try:
            data = fetch(url)
        except Exception as exc:                                  # noqa: BLE001
            print("FAILED %s: %s" % (filename, exc))
            failures += 1
            continue
        got = hashlib.sha256(data).hexdigest()
        if got != sha:
            print("CHECKSUM MISMATCH %s: index says %s, got %s" % (filename, sha, got))
            failures += 1
            continue
        with open(dest, "wb") as f:
            f.write(data)
    print("%d files written to %s, %d failed" % (len(jobs) - failures, args.out_dir, failures))
    return 1 if (failures or missing) else 0


if __name__ == "__main__":
    sys.exit(main())
