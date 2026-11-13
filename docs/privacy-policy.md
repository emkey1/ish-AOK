# iSH-AOK privacy policy

**Effective 13 November 2026.**

The short version: iSH-AOK collects nothing about you, sends nothing about you
anywhere, and contains no analytics, advertising or tracking code of any kind.
There is no account to create, and nothing in the app reports back to us.
Everything below is the detail behind that sentence, including the few
connections the app does make and the one optional feature that deliberately
sends your text to somebody else.

---

## What iSH-AOK is

iSH-AOK runs a Linux system on your iPhone or iPad by emulating a processor in
software. The Linux filesystem, and everything you put in it, lives in the
app's own storage on your device. It is not copied to us, and we have no way to
reach it.

We do not operate a service. There is no iSH-AOK account, no login, no sync and
no cloud component.

## What we collect

Nothing.

The app contains no analytics SDK, no crash-reporting SDK, no advertising
identifier use, no fingerprinting, and no third-party code that reports on your
use of it. We do not receive your IP address, your device identifiers, your
usage patterns, your files, your commands or your terminal output, because
nothing in the app sends them to us.

Because we collect nothing, there is nothing for us to retain, sell, share or
delete on request. Deleting the app deletes everything it stored.

## Connections the app makes by itself

Three, none of which carries information about you beyond what any web
request necessarily reveals to the server it reaches (your IP address and the
request itself):

- **The filesystem catalogue.** When you open the Filesystems screen, the app
  fetches a small list of available Linux images from GitHub
  (`raw.githubusercontent.com`), at most once every ten minutes. This is so
  that an app installed months ago still knows which images exist. Nothing
  about you or your device is included in the request. GitHub, as the host, may
  log the request under its own privacy policy.
- **Downloading a filesystem image.** If you choose to install one, the app
  downloads it from the address given in that catalogue, which today is GitHub.
  Again: GitHub sees an ordinary download request.
- **A connectivity probe at launch.** The app makes one request to
  `captive.apple.com`, an Apple address that exists for this purpose. Its only
  job is to make iOS show you its "allow network access" prompt on devices
  where iOS requires an app to ask. It sends and receives no data of substance.

The app makes no other network connection on its own initiative. In particular,
it does not check for updates, report installs, or phone home.

## What programs you run do

iSH-AOK is a general-purpose Linux environment. Programs you install and run
inside it can use the network exactly as they would on any computer: `apk` and
`apt` reach their package mirrors, `curl` reaches whatever you point it at,
`ssh` reaches your servers.

Those connections are yours, not ours. We do not proxy them, inspect them, log
them or see them. What any particular program or remote service does with what
you send it is governed by that program and that service, not by this policy.
The same applies to the built-in web browser: it goes directly to the sites you
visit, with DuckDuckGo as the default search engine, and whatever it keeps
along the way -- cookies, caches -- stays on your device.

## The LLM chat

iSH-AOK includes an optional chat panel that talks to a large language model.
**It is switched off until you turn it on**, and turning it on is a deliberate
act in Settings.

This is the one feature that sends your text off the device, and only to a
destination you choose:

- **Apple's on-device Foundation Models.** Nothing leaves the device at all.
- **A model running on your own machine** — Ollama, LM Studio and similar, on
  `localhost` or on your local network. Your text reaches only that machine.
- **A provider you configure.** The app includes presets for OpenAI,
  Anthropic, Google, Groq and OpenRouter, and will talk to any
  OpenAI-compatible endpoint whose address you enter. Your text goes to that
  provider, and **their** privacy policy and terms then govern what happens to
  it, including whether they keep it or train on it. We are not a party to
  that exchange and never see it.

Before the first message of a chat leaves your device for any destination that
is not on-device or on your own network, the app tells you where it is about to
go and asks you to agree. Declining sends nothing.

**What is sent.** The prompts you type, the earlier messages in that
conversation, and the system prompt. If you enable the chat's tools, the model
can also ask to read files, list directories, search, edit files and run shell
commands inside your Linux filesystem — and the results of those calls,
including file contents and command output, are sent to the provider as part of
the conversation. By default the reading tools run without asking you each
time, while editing, shell and external-tool calls ask first; all four
categories are configurable in Settings, and shell commands can be allowed or
denied by pattern.

**MCP servers.** You can connect the chat to Model Context Protocol servers,
either remote endpoints or programs running inside the guest. None is
configured by default. If you add one, the model's calls to it — and your text
in them — go to that server.

**Your keys.** API keys and MCP tokens are stored in the iOS Keychain, not in
the app's preferences, specifically so that programs running inside the Linux
guest cannot read them.

**Your chats.** Chat transcripts are saved on your device, in the app's own
storage, so a conversation survives closing the app. You can clear them from
the Chats menu. They are not uploaded anywhere.

## Device permissions

iOS asks before granting any of these, and nothing happens until you allow it:

- **Location.** Only used when a program you run inside Linux reads
  `/dev/location`. The coordinates go to that program. The app does not
  transmit your location anywhere.
- **Photos.** Only touched when you explicitly choose to save an image or a
  converted video to your library, or choose "Delete Original" in the video
  converter.
- **Local network.** Needed for `ping`, for reaching `localhost`, and for
  talking to a language model running on your own machine.

## Diagnostics

When iOS reports a crash or a hang to the app through MetricKit, the app writes
a summary into its own storage on the device so that it can be shown to you and
so that you can export it if you want to attach it to a bug report. These
summaries are never uploaded. Sending one to us is a thing you do by hand, by
filing an issue.

Separately, if you have Apple's "Share With App Developers" setting switched
on, Apple may send us crash reports from the app under Apple's own terms. That
is an Apple mechanism, controlled in iOS Settings under Privacy & Security →
Analytics & Improvements, and the data in it comes from Apple, not from any
code of ours.

## Backups

The app's storage, including your Linux filesystem and your chat transcripts,
is part of your device's normal backups. If you back up to iCloud or to a
computer, it goes along with everything else, under Apple's terms. The app does
not itself sync anything to iCloud.

## Children

iSH-AOK is a developer tool with unrestricted internet access, and it is not
directed at children. We do not knowingly collect information from anyone, of
any age, because we do not collect information at all.

## Changes to this policy

If the app's behaviour changes, this document changes with it, and the
effective date above changes too. Its history is public, in the same repository
as the app's source code, so any revision can be compared against the one
before it.

## Contact

Questions about this policy, or about anything the app does, are best raised as
an issue at <https://github.com/emkey1/ish-AOK/issues>. Suspected security
problems have their own route, described in
[SECURITY.md](https://github.com/emkey1/ish-AOK/blob/working/SECURITY.md).
