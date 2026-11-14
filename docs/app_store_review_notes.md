# App Review notes

The text under "For App Review" below is meant to be pasted into App Store
Connect's review-notes field as it stands. Everything outside that section is
for us, not for Apple.

Two placeholders must be settled before it is pasted:

- **`[NAME PERMISSION]`** -- either a sentence citing written permission from
  the ish-app maintainers, or delete the paragraph and submit under a store
  name without "iSH". See the assessment's "Name and branding".
- **`[DEMO ACCOUNT]`** -- nothing is needed; the line saying so is already
  written. Left here only so the omission is deliberate.

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

Thank you for reviewing iSH-AOK.

### 1. What this app is

iSH-AOK is a Linux environment for iOS. It emulates x86, ARM, RISC-V and
PowerPC processors in software and runs an ordinary Linux userland on top of
that emulation, presented as a terminal. It is the same category of app as iSH
Shell (id1436902243) and UTM SE, both of which are on the App Store.

No account is needed, and there is nothing to log in to. Launch the app and a
shell prompt appears. **[DEMO ACCOUNT]** -- no demo account is required.

### 2. No downloaded code is executed as app code (2.5.2)

This is the point most worth stating plainly, because an emulator can look
like it is doing something it is not.

- **All iOS code in this app ships inside the signed binary.** Nothing is
  downloaded, loaded or linked at runtime.
- **The emulator never generates or executes native machine code.** It does
  not request executable memory from iOS: there is no `MAP_JIT`, no JIT
  entitlement, and no call that marks any page executable. The emulation core
  works by dispatching to pre-compiled assembly routines that are already part
  of the signed binary; what it builds while running is an array of pointers
  to those routines, which is data, not instructions.
- **Programs the user installs inside the Linux environment are guest
  binaries for an emulated processor.** They are interpreted by the emulator.
  iOS never executes them, and they cannot become iOS code.

So a user installing a package inside the Linux environment is, from iOS's
point of view, writing data into a file that this app then reads and
interprets -- the same relationship a document has to the app that opens it.

### 3. Network access and App Transport Security

The app's Info.plist sets `NSAllowsArbitraryLoads`. This is required and
cannot be narrowed: the whole function of a Linux environment is that the
user's own programs -- `curl`, `ssh`, `wget`, package managers -- connect to
hosts the user chooses. Those hosts are not known to us in advance and cannot
be enumerated as exception domains, and many of them, including Linux
distribution mirrors, still serve plain HTTP.

The app itself makes almost no network requests of its own. They are listed in
the privacy policy.

### 4. Background modes

Two are declared, both used only on the user's initiative:

- **Location** -- requested only when a program running inside Linux opens
  `/dev/location`. iOS's own permission prompt appears at that moment.
  *To see it:* type `cat /dev/location` at the prompt.
- **Audio** -- the built-in music player, so playback continues when the app
  is not in front. *To see it:* open the Workspace (the grid icon in the
  keyboard toolbar) and choose the Music tool.

### 5. Privacy

The app collects nothing and contains no analytics, advertising or tracking
code. The optional LLM chat is switched off until the user turns it on, and
when it is on it asks the user to confirm the destination before the first
message of a chat leaves the device. It also works entirely on-device with
Apple's Foundation Models, with no API key and no network use at all.

Privacy policy: https://github.com/emkey1/ish-AOK/blob/working/docs/privacy-policy.md

### 6. Encryption

The app includes OpenSSH, built with its own cryptography rather than
OpenSSL, so the app uses encryption beyond what iOS provides.

### 7. Name

**[NAME PERMISSION]**

---

## Things a reviewer is likely to try, and what should happen

Not for Apple -- our own checklist, so a surprise in review is one we have
already seen.

| Action | Expected |
|---|---|
| Launch | A shell prompt within a few seconds |
| `uname -a` | Linux, with the guest architecture |
| `cat /dev/location` | iOS location prompt, then coordinates |
| Workspace -> Music | A player; audio continues in the background |
| Settings -> Privacy Policy | Opens the policy in Safari |
| Settings -> Filesystems | The catalogue of Linux images |
| `apk add` / `apt install` | Reaches the distribution's mirror |
| LLM Chat with no key | Works on Apple Foundation Models; any other destination asks first |
| Files app | iSH-AOK appears as a location |
