# Native Workspace Design (archived, not maintained)

**This file is history.** This is the early native-only Workspace design —
its own stated Non-Goal was "guest-side X11/Wayland support... desktop/window-
manager emulation." The project later reversed that decision: the Workspace
"Display" applet now runs a real guest Wayland desktop (labwc/wayvnc), per
[`docs/roadmap.md`](../roadmap.md)'s "556 -- the desktop is the product"
section and [`app/DisplayViewController.h`](../../app/DisplayViewController.h).
The component and scene-type names proposed here (`WorkspaceCoordinator`,
`app.ish.scene.*`) were never built — current routing uses
`ISHSceneActivityTypeWorkspace` (`app/SceneDelegate.m`). Kept because it shows
the native-only philosophy the project explicitly moved past, per
[`docs/book/ch31-files-workspace-tools.md`](../book/ch31-files-workspace-tools.md).

---

# Native Workspace Design

## Goal

Add a lightweight workspace/dashboard experience to iSH-AOK that supports:

- multiple terminal windows
- a native clock
- simple native widgets
- optional native utility panels

while keeping as much execution and rendering as possible in native ARM code.

The design goal is not to build a Linux desktop stack. It is to let guest Linux continue to provide shell/process semantics, while the visible workspace, layout, and widgets are owned by the iOS app.

## Why This Direction

The alternatives discussed were:

- guest-side framebuffer
- guest-side X server
- native X server speaking X11 to guest apps

All of those move substantial UI complexity into a compatibility layer. They also push more work through emulated x86 userspace or through protocol/server code that does not directly help the desired outcome of "multiple terminals plus a few widgets".

For the desired feature set, native app UI is the better fit:

- better performance
- less battery cost
- simpler input/focus handling
- better iPad multiwindow behavior
- cleaner recovery and persistence
- less code in emulated paths

## High-Level Model

Split the product into two layers:

1. Guest Linux layer
- continues to run shells, commands, daemons, package tools, etc.
- remains the source of terminal sessions and any Linux-derived data

2. Native workspace layer
- owns scenes, windows, layout, widgets, persistence, and interaction
- renders via UIKit/WebKit/native views on ARM

The guest is responsible for computation and process semantics.
The app is responsible for presentation.

## Core Principle

Only terminal session execution should require guest x86 processes.

Everything else that can reasonably be native should be native:

- workspace chrome
- clock widget
- battery/network/status widgets
- session switcher
- launch surfaces
- layout manager
- per-window persistence
- diagnostics surfaces

## Existing Foundation In This Repo

The current codebase already has most of the primitives needed:

- scene-based multiwindow support in [/Users/mke/git/ish-AOK/app/SceneDelegate.m](/Users/mke/git/ish-AOK/app/SceneDelegate.m)
- terminal session view controllers in [/Users/mke/git/ish-AOK/app/TerminalViewController.m](/Users/mke/git/ish-AOK/app/TerminalViewController.m)
- terminal/session objects in [/Users/mke/git/ish-AOK/app/Terminal.m](/Users/mke/git/ish-AOK/app/Terminal.m)
- user preferences and settings plumbing
- diagnostics/breadcrumb support

So the workspace feature should be built as a native extension of the current scene model, not as a separate graphics subsystem.

## Proposed Product Shape

### Scene Types

Introduce two native scene roles at the app level:

1. Terminal Scene
- existing terminal scene
- one terminal-focused window
- remains the default for direct shell use

2. Workspace Scene
- new native dashboard/workspace scene
- can show:
  - clock
  - quick status widgets
  - terminal launchers
  - recent sessions
  - optional embedded terminal panes later

On iPad, users can open multiple Terminal Scenes and multiple Workspace Scenes.
On iPhone, the Workspace Scene can still exist, but should degrade to a simple dashboard or launcher view.

### Widget Scope

First-party widgets should be native only.

Initial widget candidates:

- clock
- current root filesystem name
- current console/session summary
- free storage
- battery
- network summary
- diagnostics/recovery shortcuts

These do not need Linux graphics support.

## Architecture

### 1. Workspace Coordinator

Add a native coordinator layer that owns workspace state.

Suggested new component:

- `WorkspaceCoordinator`

Responsibilities:

- create or restore workspace scenes
- track workspace item layout
- manage terminal scene launch/open requests
- provide native widget view models
- bridge app/guest state into workspace widgets

This coordinator should live entirely in app code and run on ARM.

### 2. Scene Routing

Extend scene restoration and creation logic in [/Users/mke/git/ish-AOK/app/SceneDelegate.m](/Users/mke/git/ish-AOK/app/SceneDelegate.m).

Instead of only restoring a terminal root view controller, restore one of:

- `TerminalViewController`
- `WorkspaceViewController`
- diagnostics/recovery view controllers when requested

Use `NSUserActivity` scene restoration metadata to distinguish scene kinds.

Suggested scene activity types:

- `app.ish.scene.terminal`
- `app.ish.scene.workspace`

### 3. Workspace UI

Create a native `WorkspaceViewController` backed by UIKit.

Recommended implementation:

- `UICollectionView` with compositional layout

Why:

- flexible for cards/tiles
- easy to support dashboard and future layout variants
- good diffable-data-source support
- easy to keep native and responsive

Suggested sections:

- header/status
- widgets
- recent terminals
- quick actions

### 4. Terminal Integration

Do not try to render arbitrary Linux graphics in the workspace.

Terminal integration should be one of two forms:

1. Launch/open terminal scene
- workspace item taps open a terminal scene
- simplest and lowest risk

2. Native terminal tiles later
- embed or host terminal views as panels in a workspace scene
- only after scene/state management is solid

Even in the second form, terminal rendering is still the current ARM-side app UI and WebKit stack, not a guest framebuffer.

## Data Flow

### Native-Only Data

Some widgets should be native and not depend on the guest at all:

- wall clock
- battery
- route/network reachability summary
- free device storage
- current app/build info

These should read directly from iOS APIs or existing native helpers.

### Guest-Derived Data

Some widgets may reflect guest state:

- current root name
- running session count
- active console mapping
- process summaries

These should not require guest graphics or new guest daemons by default.

Preferred sources:

- existing app-side state already known during boot/session setup
- proc/pseudo-files if needed
- small native bridge devices or notifications only where justified

The key rule is:

- use native summaries of guest state, not a Linux UI stack

## Native ARM Emphasis

To maximize ARM-native execution:

- keep widget logic in Objective-C/Swift/native frameworks
- do not require guest helper processes for basic workspace UI
- avoid polling guest commands for display-only information if native state already has it
- cache and observe guest state transitions rather than recomputing through guest shells

Examples:

- clock should use native timer APIs, not `/bin/date`
- battery should use native iOS signals, not a guest polling loop
- terminal scene list should come from terminal/session objects already in app memory

## Separate Threads and Performance Model

No guest display thread is needed for this feature.

The performance model should be:

- main thread:
  - UIKit scene and view updates
- native background queues:
  - widget model refresh
  - state observation / file snapshots / lightweight aggregation
- guest task threads:
  - unchanged terminal session execution

This keeps rendering on ARM and limits emulated execution to actual Linux workload.

## Persistence

Workspace state should be stored natively, not in the guest.

Persist:

- scene type
- layout choice
- widget ordering
- selected shortcuts
- recent terminal references

Do not persist native workspace configuration inside the Linux rootfs.

That keeps workspace behavior stable across root changes and avoids coupling UI state to a distro.

## Suggested First Milestone

### Milestone 1: Native Dashboard Scene

Add:

- `WorkspaceViewController`
- scene restoration for workspace scenes
- clock widget
- storage widget
- diagnostics shortcut
- recent terminals list
- button to open a new terminal scene

This milestone proves the architecture without touching terminal internals much.

### Milestone 2: Native Session Launcher/Manager

Add:

- recent and pinned terminal sessions
- explicit "new system console" / "new session shell"
- scene management UI
- basic session metadata cards

### Milestone 3: Optional Embedded Terminal Panel

Only if still desired:

- allow a terminal panel inside a workspace scene
- probably one active terminal tile at first

This is a UI/product choice, not a graphics-subsystem requirement.

## Non-Goals

The following should stay out of scope for this design:

- Linux framebuffer
- guest-side X11/Wayland support
- native X server implementation
- desktop/window-manager emulation
- general Linux widget/plugin framework

Those are separate projects with different cost and risk profiles.

## Risks

### 1. Scene Complexity

Adding a second scene type increases restoration and lifecycle complexity.

Mitigation:

- keep scene role metadata explicit
- keep terminal and workspace restoration paths separate

### 2. State Duplication

There is risk of duplicating guest state in native code.

Mitigation:

- only surface summary state the app already owns or can cheaply derive
- avoid inventing large native mirrors of guest internals

### 3. Embedded Terminal Scope Creep

Trying to make the workspace act like a desktop compositor will expand the scope quickly.

Mitigation:

- first ship a dashboard/launcher, not a composited desktop

## Recommendation

Build a native workspace scene, not a graphics layer.

That gives the requested product shape:

- multiple terminal windows
- a clock
- simple widgets
- high ARM-native execution

without taking on framebuffer, X11, or display-server work that does not materially improve the target experience.

## Concrete Next Step

If implemented, the next engineering step should be:

1. add a `WorkspaceViewController`
2. add scene-kind restoration metadata
3. add a native dashboard scene route in [/Users/mke/git/ish-AOK/app/SceneDelegate.m](/Users/mke/git/ish-AOK/app/SceneDelegate.m)
4. populate it with three native items:
   - clock
   - free storage
   - new terminal launcher

That is the smallest end-to-end slice that validates the design.
