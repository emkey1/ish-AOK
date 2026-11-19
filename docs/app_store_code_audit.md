# App Store code audit, 2026-11-17

A second pass over the tree at `15edf7fd9`, asked for as a full audit of the
code for anything that could stop App Store approval. It does not repeat
[app_store_assessment.md](app_store_assessment.md) (2026-11-13/14); it checks
that document's claims against the tree and the built binary, and looks for
what it missed.

**The short of it.** Two findings are likely to cost a review round each and
neither was known: guest DNS is dead on an IPv6-only network, which is the
network App Review tests on, and the documentation compiled into the app tells
people to use location updates to keep the app running. Three more are things
the assessment recorded as done or acceptable that are not quite. The rest is
wording and hygiene. Everything the assessment said about executable memory
and private API held up when measured.

Findings 1, 2 and 3 were fixed the next day, 2026-11-18; each says so below.
The rest is as found. Line numbers are at `15edf7fd9`.

## How it was checked

- **The Release binary**, built for the device from this tree
  (`-configuration Release`, the configuration the scheme archives with), not
  the Debug build and not the 565 archive.
- **Imported symbols**: every undefined symbol in the binary (1,784) looked up
  in the identifiers of the iOS 27.2 SDK's public headers.
- **Objective-C selectors**: every method-name string in the binary (6,278),
  less the ones the app implements, its ivar names, and the ones whose every
  component the SDK declares.
- **Bundle contents**: every file typed, linked libraries listed, signed
  entitlements read back from the signature.
- **Source**, for each guideline that has a code-level shape, against the
  guideline text fetched from Apple today.

Not done: nothing ran on a device or on a NAT64 network, and no upload was
made, so App Store Connect's own scan has not seen this build.

---

## Fix before submitting

### 1. Guest DNS drops every IPv6 nameserver (2.5.5)

`app/AppDelegate.m:5169` and `:5172` test the address family of what
`res_getservers` returns against `AF_INET_` and `AF_INET6_`. Those are the
**guest's** constants (`fs/sock.h:231,237`: `AF_INET6_` is Linux's 10). The
sockaddrs are the host's, where `AF_INET6` is 30. IPv4 survives only because
both systems use 2. An IPv6 nameserver matches neither branch and is skipped.

On a network whose resolvers are all IPv6, no server is usable, the refresh
ends at `dns.refresh.failed`, and `/etc/resolv.conf` is not written. Names do
not resolve in the guest, so `apk add` and `apt install` fail -- both are in
the review notes' own list of what a reviewer will try.

Confirmed off the device, with the loop lifted into a harness over real
libresolv output and an IPv6-only server list: the app's comparison keeps 0 of
2 servers, and the same loop with the host constant keeps both.

Why it matters now: guideline 2.5.5 is "Apps must be fully functional on
IPv6-only networks", and App Review tests on NAT64. The mistake has been in
this path since April, but until `8dc6a0df5` (2026-11-13) a device took the
`dns_configuration` path instead. Removing that private API for the store made
this the only path. It is the change the assessment marked "only
compile-verified", and it is also a regression for anyone on an IPv6-only
mobile carrier, whatever happens with the store.

**FIXED 2026-11-18** (`96249f2d6`): the host's constants. Verified in the simulator,
which resolves through the Mac, whose resolver lists one IPv4 and one IPv6
server. The build from before the change wrote a `resolv.conf` naming only the
IPv4 one; the fixed build names both, and `nslookup` in the guest answers
through the relay and when asked of the IPv6 server directly.

**Still owed: a run on NAT64**, which nothing here could provide and a Mac can
(Internet Sharing with "Create NAT64 Network"): `apk update`,
`curl https://example.com`, `ssh` to a name. Two things to look for there.
What the guest does with an IPv4 literal is untested, and there is no NAT64
handling anywhere in `fs/sock.c`, so expect `ping 8.8.8.8` to fail and decide
whether that needs an answer. And the loop still skips a **link-local** IPv6
nameserver, on purpose, since the guest cannot name the interface it lives
on; if the test network hands out only a link-local resolver, DNS will still
be dead, and the answer is to let the relay use it while keeping it out of
`resolv.conf`.

### 2. The shipped documentation recommends location as a keep-alive (2.5.4)

`opt/AOK/docs/networking.md:94-104`, under "When the screen locks":

> what keeps a session alive is keeping the app running, and the usual way is
> to keep it busy with location updates: `cat /dev/location > /dev/null &`

That file is in `fs/aok-docs.manifest`, so it is compiled into the binary and
readable at `/AOK/docs/networking.md`; `strings` on the Release binary finds
the line. `docs/book/ch28-the-app-around-the-kernel.md:75-76`, also shipped,
says the same thing in passing.

Guideline 2.5.4 allows background modes "only ... for their intended
purposes", and "we are unable to locate any features that require persistent
location" is one of the commonest rejections there is. The review notes
describe `/dev/location` as a way for a program to read the device's position,
which is true and defensible. The app's own manual describing it as the way to
stop iOS suspending the app is the opposite claim, in writing, inside the
binary under review.

The code says it too, though only to someone reading source:
`ISHLocationKeepsAppAlive` (`app/LocationDevice.m:23`) and
`ISHBackgroundKeepAlive` (`app/AppDelegate.m:5706`). Those are honest
bookkeeping -- the suspend guard has to know whether it is about to be
suspended -- and renaming them buys nothing.

**FIXED 2026-11-18**: the recommendation is gone from `networking.md`, which
now says to expect the drop and keep long work on the far end in `tmux` or
`screen`. `strings` on a rebuilt Release binary no longer finds the line.

Left alone, each for a reason: the ch28 sentence, which describes what the
suspend guard does rather than advising anything; the release notes for 559
and 563, which mention the trick and are a record of what those builds said;
and the answer on GitHub issue #631, which recommends it and is not ours to
edit without the maintainer saying so.

---

## Likely to draw a question

### 3. The app still terminates itself in three places

The assessment and `02774847b` say `exit(0)` is gone. It is gone from the two
paths that commit touched. These remain:

| Where | Reached by |
|---|---|
| `app/AppDelegate.m:718` | "Quit AOK-iSH" on the System Halted alert, after `poweroff`, `halt` or `reboot` in the guest |
| `app/Roots.m:2373-2381`, `:2413` | the `exit-app` roots control operation |
| `app/AboutViewController.m:472` | leaving recovery mode |

Two more are dead: `app/AppDelegate.m:6078` (nothing sets `exiting` any more)
and the `else` at `app/RootsTableViewController.m:1232`.

The first is the one a reviewer can reach: typing `reboot` at a Linux prompt
is an obvious thing to try. Each is behind a button the user presses, which is
the mild form, but the decision recorded on 2026-11-14 was to replace exits
with `ISHPresentFinalScreen`, and these were not in view when it was made.

**This audit missed a fourth, and it was the worst of them.** A suspend asked
for from inside the guest -- `/AOK/tools/suspend.sh`, or any write of
`suspend` to `/proc/ish/checkpoint`, which the guest may do once Suspend to
Disk is on -- saved the image and called `exit(0)` in `kernel/checkpoint.c`.
No button and no alert: one documented command and the app was gone. It is
in `kernel/`, where the three above are all in `app/`; a comment in
`AppDelegate.m` and a line in `docs/TODO.md` both already described it.

**FIXED 2026-11-18** (`11f181700`), all four:

| Path | Now |
|---|---|
| System Halted | One button, OK. The message already says to quit and reopen, and the terminal behind it shows "System halted" by itself. |
| `exit-app` | The closing screen, "Restart requested", naming the root that boots next. The op and `--exit-app` keep their names; the script and docs say what they do. |
| Leaving recovery mode | The switch is cleared and a closing screen says to open the app again. |
| Guest-requested suspend | A kernel hook: the machine is left frozen, the app shows "Session suspended", and the task that asked parks for good. The CLI still ends its process. |

The two dead exits are removed. `exit(0)` remains in the tree in one place
the app can no longer reach (the CLI's ending in `kernel/checkpoint.c`), and
`_exit(0)` in the File Provider extension, which retires itself when it finds
it is running on a Mac; that is an extension process, not the app.

The guest-requested suspend had a second fault, on the CLI as well: it thawed
the guest before the image was synced and only then exited, so every task but
the shell that asked ran on past the image. `tests/manual/suspend_stays.sh`
shows both; on the old binary it fails every run.

Verified in the iOS 26.5 simulator on the Debug build: for each of the four,
the app's process is still running afterwards and the screen is the one
described; a suspended session resumes, from the shell and from a Workspace
terminal, with its windows. On the build from before the change the same
`suspend.sh` ended the process. The Release configuration builds. **Not run on
a device.**

"Quit Now" on the Filesystems screen said it "closes" the app, which since
`02774847b` meant the closing screen. **FIXED 2026-11-18:** both of its alert
messages, the docs and the translations now say it ends on a screen asking
you to close AOK-iSH.

### 4. One private API is left, and its reason has expired

`app/AccessibilityFixes.m:29` looks up a WebKit C++ symbol by its mangled
name, and `app/hook.c:283` reaches `proc_regionfilename` through `dlsym`. Both
strings are in the Release binary. The assessment has this and left it to be
weighed.

Two facts to weigh it with. It is the **only** private API in the binary: the
symbol and selector scans found nothing else (see "Measured clean"). And the
WebKit bug it works around, 249976, was fixed upstream in February 2023, while
the hook still arms on every iOS from 15.7 up whenever VoiceOver is running --
including the reviewer's.

**Options:** bound it to the iOS versions that have the bug, so it never runs
on a current system but the strings stay; or remove it and accept the
VoiceOver bug on iOS 15.7-16.x. Which iOS release first carried the fix was
not established here (17.0 at the latest is an inference from the date).

### 5. The local network purpose string no longer describes the use (5.1.1(ii))

`app/Info.plist:88`: "This is required for connecting to localhost and using
the ping command." Inherited from upstream. Since then the app has gained an
ssh server that accepts connections from the network (Settings, sshd), an LLM
client and MCP client that are commonly pointed at a machine on the LAN, and
native `ssh` and `curl`. The guideline asks that purpose strings "clearly and
completely describe your use". Ten languages in `app/InfoPlist.xcstrings`.

### 6. Name and icon: the rule is 4.1(c)

Already known and waiting on upstream. One refinement: the assessment and the
drafted request cite 5.2.1. The current guidelines have a rule written for
exactly this, 4.1(c): "You cannot use another developer's icon, brand, or
product name in your app's icon or name, without approval from the developer."
It reaches the "ISH" badge on the icon as well as the name. Cite it in the
request and in the review notes.

### 7. The shipped book names alternative marketplaces (2.3.10)

`docs/book/ch37-releasing.md:59-94,246` is in `fs/aok-book.manifest` and
discusses AltStore, SideStore and Sideloadly by name, along with the unsigned
IPA. `opt/AOK/docs/files-app-integration.md:80-83` and one string in
`Localizable.xcstrings` talk about a "sideloaded" copy. 2.3.10: "don't include
names ... of other mobile platforms or alternative app marketplaces in your
app or metadata". Buried, but in the binary.

**Fix:** leave ch37 out of the manifest for store builds, or take the names
out of it.

### 8. 2.5.2: three facts the review notes do not mention

Still Apple's call, as the assessment says. What is new:

- **The catalogue of images is fetched at run time** (`app/Roots.m:250`) and
  replaces the bundled copy, so what the app offers can change after review.
  It includes a "community" tier (Arch, PSCAL, ppc64le Alpine).
- **Nothing verifies a download.** The manifest carries no hash and
  `app/Roots.m` has no digest code.
- **A root can be installed from any URL** from inside the guest
  (`source=url`, `app/Roots.m:2517`).

None is forbidden; all three are the kind of thing better said than
discovered. A SHA-256 per entry in the manifest, checked after download, turns
the second from a weakness into a line in the notes.

---

## Low: wording and hygiene

- **The ATS justification names the wrong programs.** Review notes line 70
  gives `curl`, `ssh`, `wget` and package managers as why
  `NSAllowsArbitraryLoads` is needed. ATS governs the URL loading system, not
  sockets, so it does not touch `ssh` or anything emulated. What it does
  govern here: native `curl` (a shim over `NSURLSession`, `095f9d274`), the
  in-app browser, LLM and MCP endpoints, media streams, and the plain-HTTP
  request to `captive.apple.com` at `app/AppDelegate.m:5446`. Name those.
- **"JIT" is all over the shipped documentation.** 306 occurrences in the
  Release binary, a chapter titled "the JIT that is not a JIT", and the
  preference key "Enable JIT Cache" (`app/UserPreferences.m:35`). No
  user-interface string says it. The notes say there is no JIT; add a sentence
  saying the project's own documents use the word for a threaded-code
  interpreter, before a reviewer finds the word and asks.
- **Third-party licence notices are not in the app.** The Legal screen carries
  the GPL offer and SmallCLUE's NOTICE and points at "the licence in its own
  directory" for the rest, which is not shipped. "Redistributions in binary
  form must reproduce" appears 0 times in the binary; OpenSSH, zsh, dash,
  libarchive, the Xiph codecs, MoltenVK, hterm, helix and its crates all ask
  for their notice to travel with the binary. Not App Review's concern, but it
  is this project's stated standard.
- **Keywords.** `docs/app_store_metadata.md:43` includes `unix`, `debian`,
  `alpine`, `linux` and `git` while line 45 says it excludes trademarks that
  are not ours. `debian` is also not what ships (Devuan). 2.3.7.
- **Six method names match UIKit private selectors**: `_applicationDidBecomeActive:`,
  `_load`, `_save`, `_update`, `_updateStyle`, `_updateUI`. No app class
  inherits from a UIKit class that implements one (checked against UIKitCore
  26.5), and all six were in the 565 upload, which processed. Rename only if
  ITMS-90338 ever names them.
- **`app/CurrentRoot.m:113` rewrites Alpine's HTTPS mirror to HTTP.** It sits
  badly beside an ATS justification that says mirrors "still serve plain
  HTTP": this one serves HTTPS and the app chooses otherwise.
- **The video player takes IPTV-style playlists** (`app/WorkspaceVideoPlayer.m:81`).
  A generic player with no sources of its own, but 5.2.3 is where a question
  would come from.
- **Leftovers.** `"fuc" => ""` in the built Info.plist (upstream's icon macro
  trick, `app/Info.plist:189`); `ROOTFS_URL` pointing at `ish-app/roots` in
  `app/iSH.xcconfig:55`, read by nothing.

---

## Measured clean

So that nobody measures these again without a reason.

| Claim | Result |
|---|---|
| No executable memory | No `MAP_JIT`, `VM_PROT_EXECUTE`, `pthread_jit_write_protect`, `sys_icache_invalidate` or JIT entitlement anywhere compiled. Host `mprotect` is only ever `PROT_NONE`, `PROT_READ` or read-write (`emu/memory.c:476-479`). |
| No code loaded at run time | The bundle holds two Mach-O files, the app and the File Provider extension. No frameworks, no dylibs. Native programs' `dlopen` is a stub returning NULL (`kernel/native_libc.c:8768`). |
| Imported symbols | 58 of 1,784 are not in a public header and all 58 are the compiler's or the runtime's (ARC, stack protector, compiler-rt, Swift concurrency's weak `voucher_adopt`). No `fork`, `execve`, `system`, `ptrace`, `syscall`, `mach_vm_*`, `task_for_pid`, `csops` or `proc_*`. |
| Selectors | Nothing private. What remained after filtering was six of the app's own Swift method names. |
| Linked libraries | Public frameworks only; no `PrivateFrameworks` path, no `UIWebView`. |
| Entitlements | Extended virtual addressing, increased memory limit, user fonts, app group. (`get-task-allow` is the development signature.) |
| Permissions | No camera, microphone, contacts, Bluetooth, motion, speech, tracking or notification API. Photos goes through `PHPicker`, add-only for saving, read-write only for Delete Original, each with a string. MediaPlayer is used for Now Playing only, so no music-library string is owed. |
| Audio background mode | The session goes to `.playback` only while the Music engine plays. No silent audio. |
| Privacy manifests | Present in both bundles; the declared categories match the calls. |
| LLM consent | Both user send paths go through the per-host sheet; sub-agents inherit the parent's destination; Foundation Models and loopback are exempt. |
| Money and tracking | No StoreKit, analytics, advertising, donation link or behaviour that differs under review. |
| Translations | All 1,614 strings are translated in all nine languages. The assessment's note that the suspend strings are stale is itself out of date. |
| Test hooks | `ISHWorkspaceTestHooksStart` is an empty function in Release. |

One import worth knowing about, not a problem: `posix_spawnp` comes in with
Rust's standard library, for helix. It is public and cannot succeed in the
sandbox.
