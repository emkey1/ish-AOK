# Browsing your guest filesystem from the iOS Files app

iSH-AOK ships a File Provider extension (a separate iOS extension target) that
exposes your installed root filesystems through Apple's Files framework. In the
Files app it appears as the location **iSH** — the extension's display name has
not been renamed to match the fork. Once set up, your guest files
show up as a regular location in the iOS **Files** app, and in any other
app's document picker — no `scp`, no manual export step.

## Enabling it

Files app integration is an iOS extension, so it needs to be turned on
like any other Files provider:

1. Open the **Files** app.
2. Tap **Browse**, then the **⋯** (more) button, or go to Locations at
   the top of the sidebar.
3. Enable the iSH-AOK location if it isn't already checked.

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

## Note on locking

The File Provider extension can briefly hold a lock on a root while it's
mid-operation. If you also use [`/AOK/roots`](roots.md) to cross-mount that
same root for chrooting, a root that's locked by an in-progress Files app
operation is skipped for that boot cycle and will pick up on the next one
once the lock clears.
