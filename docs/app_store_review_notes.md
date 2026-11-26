# App Review notes

The text under "For App Review" below is meant to be pasted into App Store
Connect's review-notes field as it stands: plain text, since the field shows
no Markdown, and under its 4,000-character limit. Everything outside that section is
for us, not for Apple.

Rewritten 2026-11-24 to fit the limit (3,174 characters): plain text; the app
described as implementing a subset of the Linux interface, not running Linux;
and the reviewer told what a fresh install does: from 567 it boots AOK-Workstation
Native at once, with no picker (it was a picker before).

There is no name section. On 2026-11-20 the maintainer decided to submit as
"AOK-iSH" without asking the ish-app maintainers first, and on 2026-11-26 the
app was renamed AOK-Workstation, whose name borrows no one else's. See the
assessment's "Name and branding". App Store Connect holds the 2026-11-24 copy,
which says AOK-iSH, until this one is sent.

The claims about code generation in section 2 were re-checked on 2026-11-14:
no `MAP_JIT`, no `com.apple.security.cs.allow-jit` or `dynamic-codesigning`
entitlement, and no host `mprotect` that adds `PROT_EXEC` anywhere in the
tree. Every `PROT_EXEC` in `emu/` and `jit/` is the emulator *enforcing* NX on
the guest's own address space, which is the opposite of asking iOS for
executable memory. Re-check before each submission; the wording is a factual
claim Apple can test.

Checked again 2026-11-14 against the one place that looked like it might be an
exception, app/hook.c: it intercepts a WebKit function with an ARM *hardware
breakpoint* and a Mach exception port, not by patching code. `VM_PROT_EXECUTE`
appears nowhere in it. The claim holds for the whole tree, not just `emu/` and
`jit/`.

---

## For App Review

Thank you for reviewing AOK-Workstation.

1. WHAT THIS APP IS

AOK-Workstation is a Unix-style command-line environment for iPhone and iPad. It implements a useful subset of the Linux system interface (processes, files, pipes, signals, terminals) inside the app. It does not run a Linux kernel or a virtual machine. Its default filesystem, AOK-Workstation Native, is about 140 commands compiled into the app's signed binary. Optional filesystem images run x86, ARM, RISC-V or PowerPC programs through an emulator. It is the same category of app as iSH Shell (id1436902243) and UTM SE.

No account or demo login is needed. On first launch the app sets up AOK-Workstation Native (nothing is downloaded) and asks for an everyday account name, which can be skipped; a shell prompt is already there behind it.

2. NO DOWNLOADED CODE RUNS AS APP CODE (2.5.2)

- All iOS code ships inside the signed binary. Nothing is downloaded, loaded or linked at runtime.
- The emulator never generates or executes machine code. It requests no executable memory: no MAP_JIT, no JIT entitlement, no call that makes a page executable. It dispatches to pre-compiled routines already in the binary; what it builds while running is an array of pointers to them, which is data.
- Programs a user installs into a filesystem image are guest binaries for the emulated processor. iOS never executes them and they cannot become iOS code: to iOS they are files the app reads, as a document is to the app that opens it.

Stated so they are not a surprise:
- Settings > Filesystems offers a catalogue of filesystem images, fetched from our GitHub repository, which can gain entries between releases. A user can also install one from a URL. An image is an archive of guest files, never iOS code.
- Our documentation uses the word "JIT" (one preference is "Enable JIT Cache") for the threaded-code interpreter described above. It is not a just-in-time compiler.

3. NETWORK ACCESS (ATS)

Info.plist sets NSAllowsArbitraryLoads, and it cannot be narrowed: the built-in curl, the in-app browser, the AI chat and MCP clients, and media streams connect to hosts the user chooses. Those are not known in advance, and some, such as a server on the user's own network, serve plain HTTP. The app's own few requests are listed in the privacy policy.

4. BACKGROUND MODES

Both are used only on the user's initiative:
- Location: requested only when a program opens /dev/location, at which point iOS shows its own prompt. To see it, type: cat /dev/location
- Audio: the built-in music player keeps playing when the app is not in front. To see it, open the Workspace (the grid icon in the keyboard toolbar) and choose Music.

5. PRIVACY

The app collects nothing and contains no analytics, advertising or tracking code. The optional AI chat is off until the user turns it on, and asks the user to confirm the destination before a chat's first message leaves the device. With Apple's Foundation Models it works entirely on-device, with no network use.

Privacy policy: https://emkey1.github.io/ish-AOK/privacy/

6. ENCRYPTION

The app includes OpenSSH, built with its own cryptography rather than OpenSSL, so it uses encryption beyond what iOS provides.

---

## Things a reviewer is likely to try, and what should happen

Not for Apple -- our own checklist, so a surprise in review is one we have
already seen.

| Action | Expected |
|---|---|
| Launch (fresh install) | AOK-Workstation Native boots at once, no picker; the account question can be skipped; a root prompt behind it |
| `uname -a` | Linux, with the guest architecture |
| `cat /dev/location` | iOS location prompt, then coordinates |
| Workspace -> Music | A player; audio continues in the background |
| Settings -> Privacy Policy | Opens the policy in Safari |
| Settings -> Filesystems | The catalogue of Linux images |
| `apk add` / `apt install` | Reaches the distribution's mirror |
| LLM Chat with no key | Works on Apple Foundation Models; any other destination asks first |
| Files app | AOK-Workstation appears as a location |
