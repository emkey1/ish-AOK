# iSH-AOK on the public App Store: what it would take

Asked for on 2026-11-13, during the 566 cycle. This is an assessment, not a
plan: nothing here is scheduled, and the decision to submit at all is the
maintainer's.

**The short of it.** Nothing technical rules it out. The build already goes to
TestFlight as a signed App Store archive, and it already passes Beta App
Review, which is the same submission pipeline with a lighter guideline pass.
What is missing is paperwork (a privacy manifest, a privacy policy, the
export-compliance key, store metadata), one consent screen, one private API
AOK added itself, and a judgement call about guideline 2.5.2 that only Apple
can settle. Call it **2-4 days of work**, plus however long the naming
question and one or two review round-trips take.

Everything below was checked in the tree at `dacf906fe` unless it says
*inferred*.

---

## Must fix before submitting

### 1. ~~There is no privacy manifest~~ — DONE (62d1e49c7)

`PrivacyInfo.xcprivacy` does not exist in the repo, for the app or for the
File Provider extension, and none is in the 565 archive. Apple has required
one since May 2024 for the "required reason" APIs, and AOK uses several:

| API | Reason code |
|---|---|
| `NSUserDefaults` | CA92.1 |
| file timestamps (`stat`/`fstat`) | C617.1, and 3B52.1 for folders the user picked |
| disk space (`NSURLVolumeAvailableCapacity`, `statfs`; app/Roots.m:703) | E174.1 or 85F4.1 |
| boot time (`systemUptime`, `mach_absolute_time`) | 35F9.1 |

Not used: the active-keyboards API.

**Done 2026-11-13** (62d1e49c7): `app/PrivacyInfo.xcprivacy` and
`app/FileProvider/PrivacyInfo.xcprivacy`, both in their targets' Resources and
verified in the built bundles. No tracking, no collected data, and the
categories each target uses with the call sites named beside them. The reason
codes came from secondary sources (Apple's own page would not render), so the
next upload is the real check: an invalid one is ITMS-91106.

### 2. ~~The LLM chat sends data to third parties with no consent screen~~ — DONE (1a8cf6f2d)

`app/LLMChatSupport.m:780-786` has presets for OpenAI, Anthropic, Google, Groq
and OpenRouter. A grep of `app/LLMChat*.m` found no disclosure or consent
text. Guideline 5.1.2(i) wants the user told before their data leaves.

**Done 2026-11-13** (1a8cf6f2d): a sheet per destination HOST, naming it and
what goes with a message, gating -[LLMChatViewController sendPrompt:] where
both user paths arrive. Apple Foundation Models and the loopback presets are
not asked about, since nothing leaves the device. Verified in the simulator
against OpenRouter, including that declining sends nothing. Settings' model‑list
fetch still contacts the provider first, but sends no user content.

### 3. No privacy policy URL

Every app needs one. It has to cover the LLM providers the user chooses, the
root catalogue fetch (app/Roots.m:250), and MetricKit diagnostics, which stay
on the device (app/AppDelegate.m:906).

### 4. ~~A private API that AOK added itself~~ — DONE (8dc6a0df5)

`app/AppDelegate.m:247-265` resolves `dns_configuration_copy`,
`dns_configuration_free` and `dns_configuration_notify_key` through `dlsym`.
They are declared in `dnsinfo.h`, which is not in the SDK, and the strings are
in the shipped 565 binary. A public fallback already exists (`res_ninit` /
`res_getservers`, app/AppDelegate.m:5176-5194).

**Done 2026-11-13** (8dc6a0df5): the dlsym path is gone and resolv.conf is
always written from libresolv; `strings` on the device binary no longer finds
`dns_configuration`. The one lost trigger is a resolver change with no network
path change (editing DNS for the Wi-Fi you are already on) -- `nw_path_monitor`
covers the rest. **Not yet checked on a device.**

---

## Risks, most likely rejection first

### 2.5.2, downloading and executing code

The bundled roots point at live repositories (dl-cdn.alpinelinux.org,
deb.devuan.org), and `apk`/`apt` fetch guest binaries.

What is known: upstream iSH launched without apk, used on-demand resources for
a while, and since 1.2 pins every build to a snapshot at apk.ish.app
(ish.app/blog/default-repository-update). AOK still carries the leftovers of
that, unused — `ENABLE_APK_ODRS = YES` in app/AppStore.xcconfig, and a
repositories.txt pointing at apk.ish.app that only applies to "managed" roots
(app/CurrentRoot.m:196). Guideline 4.7 permits downloaded *games* in an
emulator, and its index and age-gate rules (4.7.4, 4.7.5) cannot be met by a
Linux distribution, so 4.7 is not the shelter here; the precedent is iSH Shell
and UTM SE being on the store as Linux environments.

**Mitigation:** review notes framing AOK as a Linux environment of that kind —
all host code is in the binary, and the gadget JIT generates none. That last
point was checked: no `MAP_JIT`, no `PROT_EXEC` in `jit/`, no JIT entitlement.

**If rejected:** host pinned, dated mirrors for App Store builds. One to two
weeks plus ongoing hosting.

### Name and branding (5.2.1, 4.1)

"iSH-AOK", the ISH badge on the icon, and the `app.ish.*` bundle id all sit
next to "iSH Shell" (id1436902243) on the same store.

**Step:** written permission from the ish-app maintainers, cited in the review
notes — or a store name without "iSH". Name availability could not be checked
from here; the existing TestFlight app record presumably already holds the
name (*inferred*).

### Background modes (2.5.4)

`app/Info.plist:139-143` declares `location` and `audio`. Location has
upstream precedent and is only requested when a program opens `/dev/location`
(app/LocationDevice.m:81-87); audio is the Music player
(app/AudioPlayerEngine.m:655). Expect a request for a demo video. The location usage strings said "iSH";
fixed 2026-11-13 (62d1e49c7).

### Other private API use, all inherited from upstream

These pass the upload scan today:

- a WebKit C++ symbol hook (app/AccessibilityFixes.m:29, via
  `proc_regionfilename` at deps/.../hook.c:283), which only runs with VoiceOver on
- `[app suspend]` (app/AppDelegate.m:5710), upstream since 2019
- ~~KVC on `UIAlertAction`'s private `handler` in app/WorkspaceTestHooks.m~~ —
  **DONE 2026-11-13** (f0eae8e06): the body is `#if DEBUG`, and
  `ISHWorkspaceTestHooksStart()` is a no-op in Release.

Already fine: `pid_shutdown_sockets` is Mac/simulator only (fs/sockrestart.c:16),
and MoltenVK's private Metal API is off (deps/MoltenVK/meson.build:89).

### "Suspend and Exit" calls `exit(0)`

Already flagged in docs/TODO.md:1814 as a decision due before an App Store
build.

### 4.7.2, exposing native APIs to downloaded software

Could be read onto `/dev/location`, the GPU, transcode and Photos. Upstream's
precedent predates 4.7 (*inferred: low risk*).

### Stability on the reviewer's device

Run the five-root regression on a current iPhone and iPad first. Recent crash
triage is in docs/TODO.md's queue.

---

## Licensing — not App Review's business, but it is our own stated policy

- **The bundled Devuan root.** The app ships
  `devuan-minirootfs-6.0-aarch64.tar.xz` (32 MB, confirmed in the 565
  archive), which contains GNU bash, coreutils, grep, sed and tar. README.md
  keeps *native* bash out over the FSF's objection to the App Store terms;
  that objection covers bash inside a tarball just as much, so the README's
  "mere aggregation" line does not obviously help (*inferred legal reading,
  not advice*). Options: have App Store builds download Devuan from the
  catalogue instead (1-2 days), or accept the risk. Alpine's busybox and
  apk-tools are the same question, and upstream iSH ships them.
- **OpenMinis/ish-arm64 code (GPLv3) is compiled in** (docs/CREDITS-aarch64.md;
  jit/guest-arm64/*.S, emu/arm64_interp.c, kernel/calls.c:1807). Their repo
  carries LICENSE.IOS and they ship on the App Store themselves, so the risk is
  low; a one-line OK from them would close it.
- **Clean:** zsh and dash (dash's GPL `mksignames.c` is not built), SmallCLUE
  (MIT), libgit2 (GPL with a linking exception), OpenSSH and libarchive (BSD),
  helix (MPL-2.0), MoltenVK and virglrenderer (Apache-2.0 / MIT).
- **Not verified:** helix's Rust dependency licences (`cargo-license` would
  settle it).
- **To do either way:** a GPL source offer for the GPL binaries in the bundled
  roots.

---

## Paperwork

- **Export compliance.** `ITSAppUsesNonExemptEncryption` is not set in
  app/Info.plist. OpenSSH is built `--without-openssl` and uses its own crypto
  (meson.build:823-828), so the honest answer is "uses encryption beyond
  Apple's". Setting the key stops the question being asked per build. France
  wants a declaration, or leave France out (*confirm on Apple's export
  compliance page*).
- **Entitlements are fine.** Extended virtual addressing, the increased memory
  limit, user fonts and the app group are all self-serve and allowed. Build
  539's rejected debugging-memory entitlement is gone. `iSHRelease.entitlements`
  is unused (app/App.xcconfig:10).
- **App Privacy label.** Probably "Data Not Collected"; the conservative choice
  is to declare User Content shared with third-party AI, for app functionality,
  not linked to the user (*inferred*).
- **Age rating.** Unrestricted web access (curl, and the in-app browser's
  DuckDuckGo default at app/WorkspaceViewController.m:14392) puts it at 16+ in
  the new tiers; upstream iSH is listed 17+. Confirm in App Store Connect.
- **Metadata.** Screenshots at iPhone 6.9" and iPad 13"; support URL (GitHub
  issues will do); a description with no TestFlight links. Do not reuse
  `fastlane/footer.txt` — it still advertises upstream's Twitter and Patreon.
- **Review notes.** What is emulated, that no code is generated, test steps for
  `/dev/location` and the Music player, and that the LLM chat works with
  on-device Foundation Models so no API key is needed.

---

## Effort

| | |
|---|---|
| Privacy manifests | 0.5 day |
| AI consent sheet | 0.5-1 day |
| Policy, label, rating, screenshots | 1-2 days |
| Remove the DNS private API, gate the test hook | 0.5 day |
| **Must-fix total** | **2-4 days** |
| If Apple insists on pinned mirrors | +1-2 weeks, plus hosting |
| If the Devuan root has to be downloaded instead | +1-2 days |

On top of that: upstream's permission on the name, and probably one or two
review round-trips.
