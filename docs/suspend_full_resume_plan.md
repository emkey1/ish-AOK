# Full-app resume from a checkpoint

**The goal, stated once:** a suspend or checkpoint allows 100% resumption of the
app -- in shell, Workspace or Wayland mode -- including every window and every
applet. Not "the guest survives": *the app comes back as it was*.

Written 2026-09-12, after a device suspend-and-exit in Workspace mode restored
the guest faithfully and came back with no terminals at all.

## Why that happened

The checkpoint saves the **guest**. Windows and applets are **app** state, and
the two were never joined:

- `saveWorkspaceLayout` (WorkspaceViewController.m) is only called from the
  Workspaces menu and the applet button. **Nothing calls it at suspend.**
- `restoreWorkspaceLayout` is likewise manual. On launch only *tools* are
  reopened (Launcher, LLM chat); terminal windows are not.
- `checkpoint_take_restored_session()` has **one caller in the whole app**,
  `TerminalViewController`'s `startSession`. Workspace never calls it, so
  restored sessions sat in the queue with nothing to adopt them.

So the shells and `ktop` were alive and unreachable, exactly as the earlier
native-program bug read: the session was fine, nothing showed it.

## What has to be true

1. **A checkpoint captures the app's UI state too**, at the same moment, into
   the same act. A layout saved at some other time describes a different
   machine.
2. **A resume rebuilds that UI and binds it to the restored guest.**
3. **The binding key survives the restore.** It cannot be a `Terminal` UUID
   (those die with the process) and it cannot be a pts number (the restore
   makes fresh ptys). It is the **session leader pid**, which the checkpoint
   restores. `checkpoint_take_restored_session_for_pid()` exists for this.
4. **Every applet can describe and rebuild itself.** The
   `WorkspaceStatefulTool` protocol already defines this
   (`workspaceToolStateForSaving` / `workspaceRestoreToolState:`) and the
   layout already carries the result -- but only 3 applets implement it.

## Where it stands

| piece | state |
|---|---|
| guest processes, memory, fds, ttys | works |
| sockets (listening rebuilt, connected hung up) | works, 2026-09-12 |
| local socket pairs inside the image come back connected, queues and all | works, 2026-09-21 |
| shell-mode resume onto its pty | works |
| pid-keyed session handover (`..._for_pid`) | **added 2026-09-12** |
| layout captured at suspend | **missing** |
| layout re-applied at resume | **missing** |
| terminal windows bound to restored sessions | **missing** |
| mode (shell / Workspace / Wayland) recorded with the image | **missing** |
| applet state: Files, Markdown, Image | implemented |
| applet state: MotePad, Video, Launcher, Browser | implemented 2026-09-12 |
| applet state: Music (Audio player) | **missing** -- state lives in the shared AudioPlayerEngine, not the applet |
| applet state: LLM chat | **unchecked** -- transcripts are persisted separately; may need nothing |
| applet state: Clock, Info, Monitor, Networks, Status, Storage, Sessions, Desktops, Themes, Shortcuts | **not needed** -- every one of these renders live or derived data |
| Wayland applet reconnect after resume | **missing** |

## Wayland

The Wayland applet (`DisplayViewController`) runs
`/AOK/tools/start-wayland.sh` in the guest -- labwc plus wayvnc -- and speaks
RFB to it over a socket. So resuming it is two halves:

- **Guest half**: labwc/wayvnc must survive the checkpoint. wayvnc's listener
  is a *listening* socket, which the socket rule now rebuilds properly; the
  compositor's own clients are unix sockets inside the image, which come back
  connected with whatever they had queued (fs/sock_ckpt.h). What cannot travel
  is a descriptor in flight in SCM_RIGHTS at the instant of the save -- and
  Wayland passes buffers that way, so this is the part to watch.
- **App half**: `DisplayRFBClient` must reconnect after a resume rather than
  reporting "Wayland session ended". Its connection is a client socket and will
  come back hung up by design; reconnecting is the correct response, not
  resuming the socket.

## Order of work

1. Capture the layout as part of the checkpoint, and re-apply it on resume.
   This is the one that turns "useless" into "mostly works".
2. Bind terminal windows to restored sessions by leader pid.
3. Record the mode with the image, so a Workspace suspend comes back in
   Workspace.
4. `WorkspaceStatefulTool` for the applets that lack it. Mostly done; what is
   left is the Audio player (its state is the shared engine's, not the
   applet's) and confirming the LLM chat needs nothing.

   The MotePad case turned out not to be about the protocol at all. Its drafts
   were already autosaved against a jetsam kill -- debounced ~2s, written on an
   IO queue -- but Suspend and Exit ends in `exit(0)`, which fires no background
   notification and does not wait for a queued write. Text typed seconds before
   a suspend had never reached disk. Its `workspaceToolStateForSaving` now
   flushes synchronously, and records the draft SLOT NAME so the restored window
   reclaims the draft it actually wrote rather than trusting that windows are
   recreated in the same order.
5. Wayland reconnect.

## Testing

The CLI cannot exercise any of this: it has no Workspace. The honest test is a
device or simulator suspend-and-exit in Workspace mode with two terminals and
an applet open, then a relaunch -- checking that the windows, their Desktops,
their sizes and their shells all come back. That is a manual test and should be
written down as one.
