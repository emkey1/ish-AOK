# Release Notes Since `builds/iSH-AOK_522`

These notes summarize changes from `builds/iSH-AOK_522` through the current `working` tree, including local uncommitted changes.

## Highlights

- Added a native Workspace environment with movable, resizable windows, a dashboard, a dock, and built-in utility apps that run at native speed.
- Added save/restore support for workspace layouts, plus smarter terminal reuse during restore and dock launching.
- Added a compact native Monitor app, graphical status utilities, and several other native Workspace tools.
- Added an AOK-specific virtual filesystem surface and native OSS-style audio device/output support.
- Expanded startup controls so the app can launch into Workspace, a terminal, or a filesystem chooser that selects the boot root before continuing.
- Improved terminal scene and workspace interactions to avoid duplicate terminal attachment crashes and dead-end recovery states.

## User-Facing Changes

### Workspace and Native Windowing

- Added a native Workspace mode with local window management instead of relying only on terminal scenes or external X11/VNC flows.
- Added movable contained windows with title bars, close controls, resize handles, and stacking/focus behavior.
- Added a resizable Dock pinned to the lower-right corner.
- Added live Dock actions that can open missing tools or bring existing windows to the front.
- Added long-press Dock menus for terminal and utility actions.
- Added the ability to hide the Dashboard and restore it later.
- Reduced title bar height to save vertical space.
- Fixed dead border/padding around the desktop area so windows can use more of the screen.
- Fixed subtle window size drift when dragging windows.

### Workspace Tools

- Added native `Clock`, `Info`, `Networks`, `Monitor`, `Filesystems`, `Settings`, and `Diagnostics` tool windows.
- Added a compact/mini mode for the Dashboard.
- Added a graphical Monitor app with compact CPU and memory bars and condensed live status fields.
- Added an Info tool showing battery state, current root, free storage, and startup mode.
- Added a Networks tool that mirrors the Dashboard network pane.
- Added a Filesystems tool window inside Workspace.
- Monitor and Dashboard network displays now prefer IPv4 addresses when available.
- Clock windows now resize cleanly and scale their typography with the window size.

### Layout Save and Restore

- Added manual save/restore for Workspace layouts, including tool windows, dashboard state, dock state, and terminal window geometry.
- Restore now recreates terminal windows more safely and avoids launching duplicate Session Shell/System Console windows.
- Existing workspace windows are reused/focused when possible instead of duplicating them unnecessarily.

### Terminal and Scene Behavior

- Terminal windows can now open as floating Workspace windows.
- Reopening a terminal from Workspace or Dock now focuses the existing matching window when possible.
- Fixed a crash caused by trying to install a terminal view that was already attached elsewhere.
- Fixed workspace-hosted terminal behavior so it no longer reserves unnecessary top inset space.
- Workspace-hosted terminals now hide the Settings button while keeping the terminal switcher accessible.
- The terminal switcher remains usable even if the current terminal is already attached to another window, allowing recovery without closing the window.
- Startup/restore behavior is more consistent with the configured startup mode instead of stale scene restoration winning unexpectedly.

### Startup and Filesystem Selection

- Added clearer startup-mode controls in both in-app settings and the global iOS/iPadOS Settings app.
- Added a new `Choose Filesystem` startup mode.
- `Choose Filesystem` opens the root picker on launch, lets the user select or import a filesystem, sets it as the active root, and then continues boot normally.
- Removed the earlier temporary approach that treated Filesystems as just another startup window target.

### Audio and Device Support

- Added native audio output plumbing and an OSS-style `/dev/dsp` device path.
- Added the AOK virtual filesystem mount and device-node support used by native integrations.
- Added bundled audio test assets and docs for audio validation.

### Networking and Linux Compatibility

- Adjusted blocking socket connect behavior to avoid SSH client wedges in the host `connect()` path.
- Added/updated runtime status reporting around battery and storage through native utilities.

## Build and Project Changes

- Updated the Xcode project and build configuration to include the new native Workspace and audio components.
- Added `native_workspace_design.md` documenting the native workspace approach.
- Added a new release-notes file for the previous release range.

## Maintainer Notes

- The current tree includes uncommitted fixes on top of `e5ee66b1`:
  - terminal recovery overlay no longer blocks the switcher
  - global Settings now exposes `Choose Filesystem`
  - startup boot-root selection now uses a real chooser flow
- Sanity check performed:
  - inspected current local diffs against `builds/iSH-AOK_522`
  - verified the current tree builds successfully with the simulator Release build

## Commit Range

- `be5ea63e` Add AOK filesystem and OSS audio device
- `73bf6a29` Ignore detritus
- `5d2c386c` Add native workspace design note
- `e23980d1` Add workspace windows and layout restore
- `e5ee66b1` Add workspace monitor and startup controls

