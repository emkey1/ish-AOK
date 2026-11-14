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

### 3. ~~No privacy policy URL~~ -- DONE (pending a hosting decision)

Every app needs one. It has to cover the LLM providers the user chooses, the
root catalogue fetch (app/Roots.m:250), and MetricKit diagnostics, which stay
on the device (app/AppDelegate.m, `MetricKitDiagnosticsDirectoryURL`).

**Written 2026-11-13**: [privacy-policy.md](privacy-policy.md), from an audit
of every outbound connection rather than from this summary. It covers what the
summary asked for plus four things it had missed: the `captive.apple.com`
request at launch (app/AppDelegate.m:5310, there to provoke the iOS network
prompt), the chat's tool calls -- whose results, including guest file contents
and command output, go to the provider, with the Read category allowed by
default -- user-added MCP servers, and Apple's own "Share With App Developers"
path, which is the one way crash data reaches us.

Verified while writing it: no analytics, crash-reporting or advertising SDK of
any kind; no update check; no iCloud sync; API keys and MCP tokens in the
Keychain, deliberately out of reach of the guest (app/UserPreferences.m:819);
the LLM client defaults to off (app/UserPreferences.m:273).

Also added, since 5.1.1(i) wants the policy reachable from inside the app and
not only from the listing: a **Privacy Policy** row in Settings beside the
GitHub and Discord links (app/AboutViewController.m, About.storyboard).

**Still to decide: where it is hosted.** The in-app row and App Store Connect
both currently point at the GitHub blob URL,
`https://github.com/emkey1/ish-AOK/blob/working/docs/privacy-policy.md`, which
works today and needs no setup. A `gh-pages` branch would give a cleaner
`emkey1.github.io` URL without the repository chrome; do **not** serve Pages
from `docs/` on `working`, which would publish this assessment along with it.

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
that -- but "unused" was too kind, corrected 2026-11-14:

- `ENABLE_APK_ODRS = YES` in app/AppStore.xcconfig really is dead: nothing
  reads the variable, and no build configuration takes that file as its base
  configuration. (The file itself is not orphaned -- fastlane/Snapfile:25
  passes it to the screenshot build.)
- **repositories.txt is not dead.** A build phase generates it into the app
  bundle, and the shipped copy reads
  `http://apk.ish.app/v3.14-2023-04-22/{main,community}` -- upstream's host,
  over plain HTTP, pinned to a 2023 snapshot of Alpine 3.14. It is wired to a
  live path: `FsUpdateOnlyRepositoriesFile` (app/CurrentRoot.m:196)
  **overwrites `/etc/apk/repositories`** with it. What makes it inert is only
  that `FsIsManaged()` is false for AOK's own roots -- Settings says "The
  current filesystem is not managed by iSH-AOK" and greys out Upgrade
  Repositories. Import an old upstream iSH root, which has `/ish`, and it runs.

  **DONE 2026-11-14:** dropped, and with it the whole mechanism, since
  nothing else used any of it. Gone: the build phase and
  `app/gen_apk_repositories.py` that generated the file; the boot-time rewrite
  in `FsInitialize`; `FsNeedsRepositoryUpdate`, `FsUpdateOnlyRepositoriesFile`
  and `FsUpdateRepositories`; `fs_ish_apk_version`, `COMPATIBLE_APK_VERSION`
  and `NEWEST_APK_VERSION`; the Upgrade Repositories row, its section footer
  and its three outlets; `UpgradeRootViewController` and its storyboard scene;
  the settings-button badge; and `FsUpdatedNotification`, which by then had
  two observers and no poster.

  A root now keeps the `/etc/apk/repositories` it came with. `FsIsManaged`
  stays -- `/ish/version` is still read, for filesystem migration -- as does
  `maybe_normalize_unmanaged_apk_repositories`, which is a different thing:
  it rewrites https to http for `dl-cdn.alpinelinux.org` on an unmanaged root.

  What is left behind on purpose: the two badge *views*. The storyboard one is
  now `hidden="YES"` and the floating one was already created hidden, so
  neither can appear. Tearing constraint-bearing views out of
  Terminal.storyboard is not a release-week change, and an inert view is not
  the kind of leftover this entry was about -- nothing writes, fires or
  reaches the network.

Guideline 4.7 permits downloaded *games* in an
emulator, and its index and age-gate rules (4.7.4, 4.7.5) cannot be met by a
Linux distribution, so 4.7 is not the shelter here; the precedent is iSH Shell
and UTM SE being on the store as Linux environments.

**Mitigation:** review notes framing AOK as a Linux environment of that kind.
**Written 2026-11-14**: [app_store_review_notes.md](app_store_review_notes.md),
paste-ready but for two marked placeholders.

Re-checked while writing them, and the claim is stronger than it was recorded:
no `MAP_JIT`, no `com.apple.security.cs.allow-jit` or `dynamic-codesigning`
entitlement, and **no host `mprotect` that adds `PROT_EXEC` anywhere in the
tree**. Every `PROT_EXEC` in `emu/` and `jit/` is the emulator *enforcing* NX
on the guest's address space -- the opposite of asking iOS for executable
memory. The engine dispatches to assembly already inside the signed binary and
builds only an array of pointers to it. Worth re-running before each
submission: it is a factual claim a reviewer can test.

**If rejected:** host pinned, dated mirrors for App Store builds. One to two
weeks plus ongoing hosting.

### NSAllowsArbitraryLoads (new, 2026-11-14)

Not previously flagged here. app/Info.plist:40 sets `NSAllowsArbitraryLoads`
true, with exception domains for localhost only. Apple asks for a
justification whenever it is set, and an app that cannot give one gets a
rejection or a request for information.

Ours is about as strong as this justification gets, and it is written into the
review notes: the function of a Linux environment is that the user's own
programs reach hosts we do not know in advance, many of which -- distribution
mirrors above all -- still serve plain HTTP. There is no narrower setting that
leaves the app working. No change needed; it just has to be said, and now is.

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

- a WebKit C++ symbol hook (app/AccessibilityFixes.m:29), which only arms when
  VoiceOver is running. **Looked at properly 2026-11-14**, and it is not what
  the phrase "symbol hook" suggests:

  - It **patches no code and allocates no executable memory.** It arms an ARM
    *hardware breakpoint* on the target address (`__bvr`/`__bcr`, DBGBCR_EL1,
    app/hook.c:375-384) and catches `EXC_BREAKPOINT` on a Mach exception port
    it owns. `VM_PROT_EXECUTE` appears zero times in app/hook.c; its only two
    `vm_protect` calls are `VM_PROT_READ`. This *strengthens* the 2.5.2 case
    rather than denting it -- even the one piece of the app that intercepts
    another framework does it without executable memory.
  - What it does do is `task_set_exception_ports` and
    `task_set_state(ARM_DEBUG_STATE64)` on its own task: debugger-shaped
    behaviour, on itself. Worth expecting a question about, and worth knowing
    it fails harmlessly when a real debugger holds the port (the code already
    logs "symbol missing or debugger attached").
  - **`proc_regionfilename` is resolved with `dlsym(dlopen(NULL, RTLD_LAZY))`
    (app/hook.c:283).** That is the same shape as the DNS calls removed in
    must-fix 4 -- a symbol that is not in the iOS SDK, reached by name at
    runtime. It is the one genuinely comparable case left, and it should be
    weighed the same way rather than filed under "passes the scan today".
- `[app suspend]` (app/AppDelegate.m:5710), upstream since 2019, reached
  through an `id` so the selector does not have to be declared. **This is one
  decision with "Suspend and Exit calls `exit(0)`" below, not two** (noted
  2026-11-14): the suspend is what makes the app animate away like any
  backgrounded app before the `exit(0)` 750 ms later. Drop the private call
  and keep the feature and the app vanishes mid-screen, which is precisely the
  "appears to crash" that reviewers object to. Either the feature goes, or
  both stay.
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
- ~~**Not verified:** helix's Rust dependency licences~~ -- **CHECKED
  2026-11-14: no GPL anywhere in the tree.** Done without installing
  `cargo-license` or fetching anything: the licence field was read out of
  every crate present in this machine's cargo registry, both the extracted
  sources and the `.crate` tarballs, and matched against the 337 packages in
  `deps/helix/Cargo.lock`. 234 resolved; every one is a permissive licence
  (overwhelmingly `MIT OR Apache-2.0`), and **zero GPL**.

  The only copyleft is MPL-2.0, in seven crates -- `nucleo`,
  `nucleo-matcher`, `smartstring`, `termina`, `tree-house`,
  `tree-house-bindings`, `uluru`. MPL-2.0 is file-level copyleft and is the
  licence helix itself is under, which this list already treats as clean. We
  ship them unmodified, so the obligation is satisfied by saying where they
  came from -- fold that into the source offer below.

  The crates that could not be resolved were never fetched on this machine,
  which is itself the finding: cargo downloads what it builds.
  `cargo tree --offline` proves the point by failing on `quickcheck`, and the
  manifests confirm the rest -- `criterion` and `quickcheck` are
  `[dev-dependencies]` (helix-view/Cargo.toml:72, helix-core/Cargo.toml:61),
  and `clipboard-win` and `crossterm` are under
  `[target.'cfg(windows)'.dependencies]` (helix-view/Cargo.toml:64,
  helix-term/Cargo.toml:92). None of them is in an iOS build.

  Residual gap, small: a crate could in principle be GPL, never have been
  fetched here, and still be in the iOS build. Running `cargo-license` on a
  machine that can reach crates.io would close it mechanically.
- **To do either way:** a GPL source offer for the GPL binaries in the bundled
  roots. **Worked out 2026-11-14**: [gpl-source-offer.md](gpl-source-offer.md),
  with the real inventory read back out of the shipped tarballs -- 9 of
  Alpine's 16 packages are GPL, and the Devuan root's 96 include GPLv3 bash,
  coreutils, grep, findutils, diffutils, gzip and dpkg. It has draft offer
  text and three decisions.

  The finding that matters: the text is the easy part. A written offer is
  only worth something if we can hand over the exact source for the exact
  binaries in a given build three years later, and nothing in this repository
  records which upstream package versions went into a shipped root. That is
  the part that gets harder with time, so it is worth fixing before the next
  release rather than after.

---

## Paperwork

- **Export compliance.** `ITSAppUsesNonExemptEncryption` is still not set in
  app/Info.plist. **Worked out against primary sources 2026-11-14**; the
  conclusion is narrower than this entry used to imply, and one piece of
  advice circulating online is simply out of date.

  *What we ship.* More than the entry said. OpenSSH built `--without-openssl`
  falls back to its own ChaCha20-Poly1305 and Ed25519 (meson.build:820-832),
  but the bigger exposure is the **bundled roots**, which we distribute to
  every user: Alpine carries `libcrypto3`/`libssl3` (OpenSSL 3.5.8) and Devuan
  carries `libgcrypt20` and `gpgv`. Crypto Accel maps guest AES and SHA
  instructions onto the host's ARMv8 crypto extensions. (What a user installs
  themselves with `apk add gnupg` is not ours, for the same reason it is not
  ours under the GPL -- see [gpl-source-offer.md](gpl-source-offer.md).)

  *Which Apple box that lands in.* Apple's table has three rows: encryption
  limited to the Apple OS (nothing to upload), an **industry-standard
  algorithm not provided by the OS** (upload the French declaration only), and
  **proprietary algorithms** not accepted by IEEE/IETF/ITU (CCATS *and* the
  French declaration). Everything above is IETF- or NIST-standard, so we are
  the **middle row: no CCATS**. That is the useful finding -- CCATS is the
  expensive one and we do not need it.

  *Decided 2026-11-14: France is in scope.* So the key is **YES** and a French
  declaration is required. **Set in app/Info.plist**, with the reasoning in a
  comment beside it, and verified to survive the plist preprocessing:
  `plutil -extract ITSAppUsesNonExemptEncryption raw` on the built bundle
  returns `true`.

  A consequence worth keeping: because we are providing documentation to
  Apple, **no BIS year-end self-classification report is needed** ("if you use
  non-exempt encryption and provide documentation to Apple, the
  self-classification report isn't necessary"). Leaving France out would have
  traded this per-build paperwork for that annual obligation instead.

  *The stale advice.* Several write-ups point at **EAR 740.13(e)** for the
  open-source exemption. That paragraph is **[Reserved]** -- the publicly
  available encryption source code provision now lives at **742.15(b)**, and
  it only bites for **non-standard cryptography**, which nothing here is. So
  the "email crypt@bis.doc.gov and enc@nsa.gov" route is a red herring for
  this app; it is not the reason we are fine, and citing it would be wrong.

  *What the French declaration actually is, and who has to do it.* Not Apple's
  form: ANSSI's. The obligation falls on the **fournisseur ou primo-importateur**
  -- the supplier or first importer of the cryptographic means -- which is us.

  - The form is annex I, *Declaration et demande d'autorisation d'operations
    relatives a un moyen de cryptologie* (v2, 2022), on **cyber.gouv.fr**
    (ANSSI moved off ssi.gouv.fr, so older write-ups point at a dead host).
    Its legal basis is the *arrete du 29 janvier 2015*.
  - It goes to **controle@ssi.gouv.fr**. There is also an online route at
    demarche.numerique.gouv.fr, which its own page warns is still under
    development.
  - ANSSI returns an *attestation de declaration*. **That** acknowledgement is
    what Apple wants uploaded as the French encryption declaration; Apple then
    issues the code for `ITSEncryptionExportComplianceCode`, which goes beside
    the key and stops the per-build questions.
  - Processing is reported at about a month (*secondhand, from developer
    write-ups; not confirmed on an ANSSI page*). Worth starting well before a
    submission rather than beside one.

  Useful precedent, not a substitute: Debian publishes its own ANSSI
  attestation (debian.org/legal/anssi), which is the same shape of filing for
  a distribution full of other people's crypto. It does not cover us -- we are
  a different supplier -- but it shows the form being used for exactly this.

  *Still open:* nobody has filed it. Until ANSSI's attestation is in hand, the
  honest options are to hold the French listing or to hold the submission.
- **Entitlements are fine.** Extended virtual addressing, the increased memory
  limit, user fonts and the app group are all self-serve and allowed. Build
  539's rejected debugging-memory entitlement is gone. ~~`iSHRelease.entitlements`
  is unused~~ -- **deleted 2026-11-14**: not in the project file, not named by
  any xcconfig, and a stale duplicate of the entitlements that do apply.
  `app/App.xcconfig:10` sets `CODE_SIGN_ENTITLEMENTS = app/iSH.entitlements`
  for every configuration, so a file called "iSHRelease" that governs nothing
  is the exact shape of the confusion behind the 539 rejection. The book's
  anchor list (docs/book/ch37-releasing.md:241) now points at the file that
  actually signs the app.
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
