# 29. The terminal

The most-used piece of iSH-AOK is the one nobody thinks about. Every chapter so
far has been about producing the right bytes; this one is about the last few
inches, where those bytes become glyphs on a phone and somebody's thumbs become
input.

It is also where the project made its most surprising implementation choice.

## 29.1 The terminal emulator is JavaScript

iSH-AOK does not draw its own terminal. It runs **hterm** — the terminal
emulator from Chromium's `libapps`, written in JavaScript, the one that shipped
in Chrome's Secure Shell extension for years — inside a `WKWebView`.

Stated plainly that sounds like a compromise. It is closer to the opposite.
Writing a terminal emulator is a large, thankless, correctness-critical job:
escape sequences with decades of accumulated ambiguity, wide and combining
characters, scrollback, selection, mouse reporting, and a long tail of
*reply*-side behaviours where the terminal has to answer questions programs ask
it. Getting that wrong produces a terminal that works for `ls` and breaks for
`vim`, and there is no shortcut through it.

hterm had already done it, is maintained, and is permissively licensed. The
decision dates to October 2017 and `app/Terminal.m` still carries Theodore
Dubois's name at the top of it (Foreword).

The cost is a seam: the emulator's core is C running on host threads, and its
display is JavaScript running in a web view's process, and every byte has to
cross between them.

## 29.2 Crossing the seam

**Outward**, a guest `write` goes through the tty layer of Chapter 18 to
`ios_pty_driver` — the app's terminal is a `struct tty_driver` like any other,
which is exactly what lets the same kernel code serve the CLI's real terminal
(`fs/tty-real.c`) and the app's web view without knowing the difference.

From there it becomes `Terminal.m`'s problem, and the properties tell the story:

```objc
lock_t _dataLock;
cond_t _dataConsumed;
@property (nonatomic) NSMutableData *pendingData;
@property (nonatomic) BOOL outputInProgress;
@property (nonatomic) NSData *inFlightData;
@property (nonatomic) NSUInteger outputGeneration;
```

Delivering output means calling into JavaScript, which is asynchronous, which
means a second write can arrive while the first is still in flight. So there is
a pending buffer, an in-flight buffer, a generation counter, and a condition
variable the guest's writing thread waits on until the JavaScript side has
consumed what it was given.

That condition variable is the flow control for the entire terminal. Without it
a guest running `yes` produces output faster than a web view can render it, and
the pending buffer grows without bound inside an application with a memory
budget (Chapter 13). With it, the guest's `write` blocks — which is exactly what
a program writing to a slow terminal expects.

**Inward**, key events arrive in JavaScript, cross back through a
`WKScriptMessageHandler`, and are handed to the tty as input, where the line
discipline of Chapter 18 does the rest.

## 29.3 The bundle is a build artifact

Now the trap, which cost three builds and is the kind of thing that only ever
happens once per project because nobody forgets it afterwards.

hterm is loaded as a **concatenated bundle** — `hterm_all.js`, produced by
`hterm/bin/mkdist` — not as the individual files in `hterm/js/`. And `hterm/dist`
is gitignored in the libapps fork, so for a long time the bundle existed only
because somebody had run `mkdist` by hand at some point.

> **The bug that taught us this**
>
> An edit to hterm's sources silently ships the previous bundle.
>
> A `line-height` preference was added to `hterm/js` and had no effect on device
> **through three builds**, while the JavaScript was correct the whole time and
> nothing anywhere said the sources were not what was being shipped.
>
> The Xcode phase responsible was named "Compile JavaScript". It ran `test -f`
> on the bundle.
>
> A build step named for what it *should* do, which only checks that its output
> exists, is worse than no build step at all — it manufactures confidence. It
> now runs `mkdist` whenever any `hterm/js` or `libdot/js` file is newer than
> the bundle.

## 29.4 A one-pixel band, and why the fix is a multiplier

Terminal rendering has a category of bug that is invisible until somebody with
the right font at the right size sees it and cannot unsee it.

At font sizes under 16, a background highlight sat one to two pixels proud of
the Powerline separator beside it.

The cause is in hterm's cell geometry, and it is not a mistake:

> hterm sizes a cell as `fontBoundingBoxAscent + fontBoundingBoxDescent` — the
> font's **maximum** extent, with room for accents and deep descenders a block
> glyph never uses. The background fills that cell; `U+2588` only rises to about
> the em height, and the leftover is the band.

The reporter suggested a baseline offset. The fix is a **line-height
multiplier**, and the reasoning for choosing differently is worth keeping:

- A baseline offset "cannot fix a glyph shorter than its cell — it only moves
  the band from the top to the bottom". It relocates the symptom.
- A multiplier rather than a computed formula, "because how much of the em a
  patched font's blocks cover varies between Nerd Font patches". There is no
  correct constant to derive; there is only a per-font preference.
- **The default is 1**, which is the measured height, "so nothing moves until
  someone asks".

Three properties of a good fix, visible in one change: address the geometry
rather than relocating the symptom, refuse to derive a constant that is not
derivable, and default to changing nothing.

It is settable from Appearance and from `/proc/ish/defaults/line_height`
(Chapter 28), because a font preference that cannot be scripted is only half a
preference.

## 29.5 Verifying colour by looking at pixels

Themes travel a long way before they become light:

```
Theme.m → UserPreferences.palette → TerminalView._updateStyle → JSON → hterm prefs
```

Any of those stages can drop or transform a value, and every one of them can be
inspected while returning the right answer. As the note in the tree puts it:
**the model holding the right hex is not evidence the terminal paints it.**

So the verification is done at the far end, from the guest, by printing solid
colour blocks:

```sh
i=0; while [ $i -lt 8 ]; do printf "\033[4${i}m        \033[0m\n"; i=$((i+1)); done
i=0; while [ $i -lt 8 ]; do printf "\033[10${i}m        \033[0m\n"; i=$((i+1)); done
```

then taking a simulator screenshot and **sampling the pixels** by geometry — on
a 3x iPhone screenshot the bands sit at a known stride, so each one can be read
directly.

That is Chapter 9's rule about observable witnesses, applied to a user
interface. A screenshot is the only artifact in that pipeline that is not
somebody's report of what the value is.

## 29.6 The guest's side of the seam

A terminal is not only a display; it is a set of capabilities the guest queries
and depends on.

`TERM` and terminfo come from the guest — from the rootfs for emulated
programs, and from `kernel/native_termcap.c` for native ones (Chapter 23),
because a native program asking the *host's* terminfo database describes the
wrong terminal. Window size is `TIOCGWINSZ`, and a resize sends `SIGWINCH`
through the ordinary signal path of Chapter 12.

And then there is capability negotiation, which produced an instructive false
alarm.

> **The bug that taught us this — except it was not one**
>
> Loading `zsh/zle` in an interactive zsh appeared to cost about 530 ms:
>
> ```
> /AOK/native/zsh -i -c true                    15 ms (iPad)
> /AOK/native/zsh -i -c 'zmodload zsh/zle'     549 ms
> /AOK/native/zsh -f -c 'zmodload zsh/zle'      14 ms
> ```
>
> It is not an AOK defect, and the note recording it opens by saying the
> author's first report of it was wrong.
>
> zsh's `query_terminal()` writes a batch of terminal queries — background and
> foreground colour, cursor colour, kitty keyboard protocol, RGB support,
> `XTVERSION` — and deliberately sends the Device Attributes query **last**,
> because DA is universally supported, so its reply is the signal that the
> terminal has already processed everything before it. Anything that did not
> answer is simply unsupported.
>
> hterm answers DA. The wait is zsh asking a terminal what it can do, and
> waiting for the round trip — upstream behaviour, on any terminal, not
> something the emulator introduced.
>
> The general lesson: **"the terminal is slow" is often "the program is asking
> the terminal questions"**, and the way to tell is to look at what is on the
> wire rather than at what is on the clock.

## 29.7 Two delivery paths, and the letters `hjkl`

The best bug in this chapter is one nobody could reproduce by typing normally,
and its diagnosis is a single observation.

> **The bug that taught us this**
>
> Typing quickly into the terminal delivered characters transposed, always with
> a later character jumping toward the front:
>
> ```
> mkdir big                    ->  kmdir big
> cd /etc/apk                  ->  cd k/etc/ap
> clear; touch "my file;rm.txt" ->  clear; htoucl "my fie;rm.txt"
> ```
>
> The shell genuinely received the wrong bytes, so it was neither a rendering
> artifact nor dropped input.
>
> **Every displaced character is in `hjkl`, and nothing else ever moves. That is
> the whole diagnosis.**
>
> `TerminalView.m` had registered those four letters as bare `UIKeyCommand`s, so
> that holding one would repeat — UIKit only repeats keys a key command has
> claimed. But claiming a key also changes how it is *delivered*:
>
> - a key command is dispatched straight off the key event — measured **0.3 ms**
>   after the press;
> - every other printable character reaches `-insertText:` through UIKit's
>   text-input pipeline — measured **~5 ms** after the press, and further behind
>   once that pipeline has a backlog.
>
> Two delivery paths, nothing sequencing them. Each stream stayed internally
> ordered and the two interleaved wrongly, which is exactly the observed shape:
> `abcdefghijklmnopqrst` reached the tty as `abcdhejkflgimnopqrst`, with the
> other sixteen letters still in order.

What makes this worth a section is what happened next, because the honest
answer was that it did not affect anybody.

Injecting a `ghghghgh` alternation at controlled rates, order held at 11 ms
between keystrokes and first broke at **5.6 ms — about 90 keystrokes per
second**. It stayed correct at 40–100 ms even under heavy render load, because a
busy main thread delays *both* paths together rather than spreading them. The
software keyboard, paste and autocorrect never enter the key-command path at
all, and key repeat is self-consistent.

So reproducing it needs a machine at the keyboard: synthetic injection, a macro
key, a barcode scanner. And it was fixed anyway, on stated grounds:

> It is still a defect — the terminal has no business reordering what it is
> handed — and it was worth fixing on those terms, not as a user-facing
> regression.

The fix drops the four bare key-command registrations so those letters travel
the same in-order path as the rest of the alphabet, and synthesises the held-key
repeat from `-pressesBegan:`/`-pressesEnded:` at the delay and interval UIKit's
own repeat was *measured* using — 0.4 s, then 0.1 s. The first character of a
hold comes from the ordinary text path, so only the repeats are synthetic.

And one caveat about the harness, which cost time:
`xcrun simctl`-style injection drops keys below about 3 ms spacing, **and a
dropped key looks exactly like a reordering failure in a naive string compare.**
A test rig that can produce the symptom it is testing for is a test rig that
will eventually be believed.

## 29.8 What the terminal chapter is really about

Three of the five stories here are the same story.

The line-height preference was correct in the source and absent from the
artifact. The theme colour was correct in the model and unverified on the
screen. The keystrokes were correct in every individual pipeline and wrong in
their union.

In each case the code was right and the *outcome* was not, and the thing that
settled it was looking at the far end: run `mkdist`, sample the pixels, compare
the bytes that reached the tty against the keys the app received.

That is the same discipline Chapter 18 arrived at with `strings /usr/bin/btop`
and Chapter 9 with its oracles, and it is worth naming as this book's most
portable single habit: **verify at the boundary the user is standing on, not at
the last place you understood.**

---

*Anchors:* [app/Terminal.m](../../app/Terminal.m),
[app/TerminalView.m](../../app/TerminalView.m),
[app/TerminalViewController.m](../../app/TerminalViewController.m),
[app/terminal/term.js](../../app/terminal/term.js),
[app/terminal/term.css](../../app/terminal/term.css),
[app/Theme.m](../../app/Theme.m), `deps/libapps` (hterm and libdot),
[xcode-meson.sh](../../xcode-meson.sh) (the `mkdist` rule),
[fs/tty-real.c](../../fs/tty-real.c),
[kernel/native_termcap.c](../../kernel/native_termcap.c),
[docs/TODO.md](../../docs/TODO.md) (cell height, key ordering, the zle
investigation).

*Story:* `mkdir big` arriving as `kmdir big` — because `h`, `j`, `k` and `l`
were registered as key commands to make them repeat, and a key command is
delivered 0.3 ms after the press while every other letter takes 5 ms through
UIKit's text pipeline.
