# Browsing your guest filesystem from the iOS Files app

iSH-AOK ships a File Provider extension (a separate iOS extension target) that
exposes your installed root filesystems through Apple's Files framework. In the
Files app it appears as the location **iSH-AOK** (builds before 559 called it
**iSH**). Once set up, your guest files
show up as a regular location in the iOS **Files** app, and in any other
app's document picker — no `scp`, no manual export step.

## Not available on a Mac

If you are running iSH-AOK on an Apple Silicon Mac (as a "Designed for iPad"
app), this feature is switched off and your filesystems will not appear in
Finder. That is deliberate.

The extension is built on Apple's `NSFileProviderExtension`, which their own SDK
marks unavailable on macOS -- a Mac hosts only the newer replicated File
Provider API. Left enabled, macOS loads the extension, finds one it cannot host,
and kills it before any iSH-AOK code runs. So the app does not register a
provider there at all; a missing feature is better than a crash you cannot act
on.

Nothing else is affected -- the emulator, Workspace mode and the terminal file
browser all work normally on a Mac. To move files in and out, use the terminal
(`scp`, or [/AOK/persist](persist.md), which is a real host directory).

## Enabling it

Files app integration is an iOS extension, so it needs to be turned on
like any other Files provider:

1. Open the **Files** app.
2. Tap **Browse**, then the **⋯** (more) button, or go to Locations at
   the top of the sidebar.
3. Enable the **iSH-AOK** location (**iSH** on builds before 559) if it isn't
   already checked.

Once enabled, it appears as a location you can browse, favorite, and use
from any app's "Open"/"Save" document picker, exactly like iCloud Drive or
a third-party cloud provider.

## What you can do with it

The location's top level is one folder per installed root, plus a **Persist**
folder for the shared `/AOK/persist` directory every root sees (see
[persist.md](persist.md)).

- Browse the contents of your installed root filesystem(s) directly.
- Open guest files in other apps (a text editor, an image viewer, a
  quick-look preview) without leaving the Files app.
- Copy files in and out of the guest filesystem using ordinary Files app
  drag-and-drop or copy/move operations.

Because the extension opens the same on-disk, SQLite-backed root your
running guest uses, changes made from Files app are visible to the guest
immediately (and vice versa) — there's no separate sync step.

## Editing, moving, and what Files never does

- **An edit made in another app is saved into the same file**, in place. If
  there isn't room on the device for the new contents, the save is refused
  and the old file is left as it was rather than half overwritten.
- **Files makes copies of whole folders** — when you open one in the Move
  sheet's **Recently Used**, for example. Such a copy is a snapshot for
  reading. From build 562 nothing in it is ever written back over the folder
  in your root. Before 562 a stale or incomplete copy could replace the
  folder, which is how a `/root` lost most of its files after a large disk
  image was moved out of it.
- **Moving within one filesystem** renames the file in place, into a
  sub-folder or into the filesystem's top folder. Moving to another location
  (On My iPhone, iCloud Drive) is a copy in current iOS; delete the original
  afterwards if you no longer want it in the guest.
- **A filesystem's own folder can't be deleted, renamed or moved** from Files.
  Remove a whole filesystem from Settings → Filesystems instead.
- **Every item keeps its own identity.** Files remembers items by number for
  days, and a number now always names the same file. A deleted file's number
  is never given to another. Before 562 a new file or folder could inherit it,
  and a later action on the old item reached the new one.

## A sideloaded copy has no Files location

The extension reads your filesystems out of the app's shared App Group
container. A copy of the app re-signed without that group — a sideloaded one,
usually — has no such container, so there is nothing for Files to show. From
build 559 a filesystem's **Browse Files** row says so in an alert ("Files
Can't Show This Filesystem") instead of crashing the app, as it did before.
**Export** on the same screen still works.

## Note on locking

The File Provider extension can briefly hold a lock on a root while it's
mid-operation. If you also use [`/AOK/roots`](roots.md) to cross-mount that
same root for chrooting, a root that's locked by an in-progress Files app
operation is skipped for that boot cycle and will pick up on the next one
once the lock clears.
