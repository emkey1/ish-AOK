# External display support (GH #540)

Plan for using a connected external display (USB-C/HDMI dongle, or AirPlay)
as a real output surface rather than a letterboxed mirror.

## What already works, and what doesn't

Three separate things get conflated here, which is why this looks like it
should already work:

- **Mirroring works today, with no code at all.** Every iOS app gets it for
  free: plug in a dongle or AirPlay-mirror and the external display shows a
  letterboxed copy of the device screen. This is almost certainly what people
  have seen working.
- **iPad Stage Manager largely works today.**
  `UIApplicationSupportsMultipleScenes` is already `YES` in `app/Info.plist`,
  so under Stage Manager with an external display the app gets real resizable
  scenes over there through the ordinary application role.
- **A dedicated external-display scene does NOT work.** `UISceneConfigurations`
  in `app/Info.plist` declares only `UIWindowSceneSessionRoleApplication`.
  Without a `UIWindowSceneSessionRoleExternalDisplayNonInteractive` entry, iOS
  never creates a scene for the external screen, so the app can only ever
  mirror. There is also no `UIScreen.didConnectNotification` handling anywhere
  in `app/`.

So the missing case is the one from the issue: **iPhone + dongle, no Stage
Manager** — today you get a letterboxed mirror instead of the full display.

## The constraint that shapes the design

`Terminal` owns exactly one `WKWebView` (`Terminal.h`), and `TerminalView`
asserts that it is the only host:

```objc
// TerminalView.m, installTerminalView
UIView *superview = self.terminal.webView.superview;
if (superview != nil)
    NSAssert(superview == self.scrollbarView, @"installing terminal that is already installed elsewhere");
```

So **one `Terminal` maps to exactly one view at a time**. Genuine mirroring
(same terminal rendered on both screens simultaneously) is not possible
without a second renderer, and is not what this plan proposes.

The other half of the constraint is what makes a split feasible: **keyboard
input does not go through the webView.** `TerminalView` is a `UIKeyInput`
responder that forwards to the terminal object:

```objc
// TerminalView.m
- (void)insertText:(NSString *)text { ... [self.terminal sendInput:data]; }
```

And an external display scene is **non-interactive** — it cannot become key,
so a view hosted there can never be first responder and can never receive
focus or a keyboard.

Those two facts together rule out the obvious designs:

- A separate terminal session on the external display is unusable — nothing
  can type into it.
- True mirroring needs a second renderer.

## Proposed design: render there, input here

Move only the **webView** to the external display, and leave everything else
on the device:

| | device screen | external display |
|---|---|---|
| `TerminalView` (first responder, `UIKeyInput`) | stays | — |
| keyboard, bar buttons, gestures | stay | — |
| `terminal.webView` (the rendering) | relocated away | hosted full-size |

Input keeps working unchanged because it never touched the webView. The user
types on the phone; output renders on the big screen.

### Work items

1. **`app/Info.plist`** — add a `UIWindowSceneSessionRoleExternalDisplayNonInteractive`
   entry to `UISceneConfigurations` naming a new delegate class.

2. **`TerminalView`** — add an external-host mode:
   - `@property (weak) UIView *externalHostView;`
   - `installTerminalView` / `uninstallTerminalView` target `externalHostView`
     when set, `scrollbarView` otherwise. The two `NSAssert`s that currently
     hard-code `scrollbarView` have to learn about the second legal host, or
     they will fire on the handoff.
   - a `-relocateContentToView:` / `-restoreContentToLocalView` pair that
     re-parents the webView and re-runs the sizing path.

3. **`ExternalDisplaySceneDelegate`** (new) — on connect, build a window on the
   external `UIWindowScene`, find the active `TerminalViewController` (there is
   already a `currentTerminalViewController` global in `SceneDelegate.m`), and
   relocate its content. On disconnect, restore.

4. **Device-side placeholder** — once the webView leaves, the terminal area is
   empty. It needs to say something ("Showing on external display") while
   remaining first responder so typing still works.

5. **Terminal sizing** — rows/cols are derived from the hosting view's bounds
   and pushed to the tty. Once the webView lives on the external display, the
   size must follow the *external* container, not the phone. This is the
   subtlest part and the most likely source of bugs.

### Known sharp edges

- `TerminalViewController.m` (~line 1446) already documents UIKit vending
  **two different `UIScreen` wrappers for the same physical display** (the
  separate keyboard scene on iPad), and refusing coordinate conversions
  between them. A second *real* screen makes this sharper — convert keyboard
  frames through the window, never through a `UIScreen`.
- There are roughly a dozen `UIScreen.main` uses across `TerminalViewController`,
  `WorkspaceViewController`, `UserPreferences`, `ArrowBarButton` and
  `WorkspaceImageViewer` that assume a single screen and need auditing.
- Scene lifecycle: the external scene can connect before *or* after the
  terminal scene, and can disconnect while the app is backgrounded.

## Implementation notes

Implemented as designed (`ExternalDisplaySceneDelegate`, `TerminalView`'s
external-host mode, the plist entry, the device-side placeholder). Two things
turned out differently from the plan:

- **Sizing needed no new code.** hterm re-lays out when the webView's frame
  changes and reports the new geometry through `onTerminalResize`, which
  `Terminal syncWindowSize` already pushes to the winsize. Re-parenting the
  webView is enough; `stty size` follows the external display and comes back
  when it is unplugged.
- **The external container must be constrained, not autoresized.** Sizing it
  from `rootViewController.view.bounds` before the window adopts the controller
  bakes in a placeholder size, which showed up as the terminal rendering into a
  720x480 corner of a 1080p display.

Also worth knowing: iOS can stand up a replacement external scene *before*
tearing the old one down when the display renegotiates its mode, so the restore
path is keyed on the host view (`restoreContentFromView:`) rather than
unconditionally pulling the terminal back to the device.

## Workspace, and the idle display

Declaring the external-display scene is app-wide: iOS stops mirroring the
device whether or not the app has anything to put over there. So every mode
needs an answer, not just the full-screen terminal.

- **Workspace** hands over the frontmost desktop terminal window. Its terminals
  are children of contained window views rather than a scene's root view
  controller, so `WorkspaceViewController` exposes
  `frontmostHostedTerminalViewController` for the lookup to find.
- The lookup asks **what is frontmost first** (`ISHActivePresentationViewController`)
  and only then falls back to `currentTerminalViewController`. Workspace is
  usually *presented over* a terminal scene, so consulting the global first
  handed the display a terminal that was covered up while the Workspace
  terminal stayed in its postage-stamp window.
- When there is nothing to hand over -- a Workspace desktop with no terminal
  window, or the standalone Wayland Display -- the display shows an idle
  message rather than an unexplained blank field. It comes back on its own when
  the content leaves, and the display re-attaches to another terminal if one is
  open.
- Text on both the idle message and the device-side placeholder is coloured
  from the **palette foreground**, not a system label colour: these sit on the
  terminal's own background, and a dark theme under a light system appearance
  (or the reverse) rendered them nearly invisible.

## Wayland: mirrored, not relocated

The Wayland display can't use the terminal's trick. Its `DisplayRFBView` is
*also* the touch surface that generates the RFB pointer events (touches are
scaled from the view's bounds into framebuffer coordinates), so it has to stay
where the fingers are. Instead a second `DisplayRFBView` on the external screen
renders the same framebuffer:

- The mirror does **no protocol I/O**. It never reads the client's framebuffer
  and never acknowledges — the read/acknowledge pair is what gates the next
  update onto the wire, so a second acknowledger would let the client overwrite
  the buffer while the other view was still uploading from it. It draws the
  source's already-uploaded `MTLTexture`, which is legal to share because it is
  created with the source's `MTLDevice`.
- The source draws the mirror **synchronously**, not via `setNeedsDisplay`.
  Both views sample one texture, and the acknowledge has already released the
  client to send the next update: a mirror waiting for its own vsync sampled a
  texture that was already part-way into the following frame, which tore
  visibly on a full-framebuffer update.
- The cursor is an overlay positioned from the pointer events we send (RFB
  never pushes position). The mirror keeps the position in *framebuffer*
  coordinates so it can scale into its own, differently sized, bounds.
- `SetDesktopSize` follows the external display while mirroring: it is the
  screen being looked at, and following a portrait phone instead would hand a
  720x1280 desktop to a 16:9 display.

Both surfaces are found and driven through one `ISHExternalDisplayContent`
protocol, so the scene delegate doesn't care which it got. Bringing a different
capable Workspace window to the front moves the display to it — handled on the
next runloop turn, because the Workspace posts that from inside its own window
setup and re-parenting views there made a freshly opened window close itself.

## Out of scope

- Interactive external displays (the non-interactive role is what iOS gives
  for this hardware path).
- Changing Stage Manager behaviour, which already works.
