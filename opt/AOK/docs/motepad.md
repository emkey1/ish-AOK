# motepad: a text editor that does not ask you to learn it first

```sh
/AOK/native/motepad notes.txt
```

To type plain `motepad`, run `sh /AOK/tools/native-links.sh` once per root — it
links the standalone native programs as well as SmallCLUE's applets, so
`motepad` lands in `/usr/local/native-bin` along with `bash` and `zsh`. See
[native-setup.md](native-setup.md). The rest of this page writes it bare on the
assumption you have.

`motepad` is the terminal half of Workspace's MotePad editor, and it is
deliberately **modeless**. MotePad is a plain editor with a line-number gutter,
and a terminal counterpart that made you learn vi before you could type a
sentence would not have been the same program. Here you type and the characters
appear; the commands are control keys. If you *want* a modal editor, Nextvi is
already in the app as `vi` — see [native-programs.md](native-programs.md).

It is a [native program](native-programs.md), which has two consequences worth
knowing. There is nothing to install, and it costs the same under an i386 root
as under an arm64 one, because it is host code with no guest image to translate.
And unlike `bash` or `zsh` it has **no build switch** — it is a single C file
with no dependency beyond the native-libc shim, so it is in every build rather
than being something you have to check for.

## Keys

| key | what it does |
|---|---|
| `Ctrl-S` | save |
| `Ctrl-Q` | quit — with unsaved changes the first press asks, the second discards |
| `Ctrl-F` | find; searches forward from the cursor and wraps to the top |
| `Ctrl-G` | go to line |
| `Ctrl-K` | delete the current line |
| `Ctrl-A`, Home | start of line |
| `Ctrl-E`, End | end of line |
| arrows, PgUp, PgDn | move |
| Backspace, Del | delete before the cursor / at it |
| Enter, Tab | insert a newline, insert a literal tab |
| Esc | cancel the find or go-to-line prompt |

Any key other than `Ctrl-Q` clears a pending quit confirmation, so a stray
`Ctrl-Q` followed by more typing cannot throw the buffer away several minutes
later.

Control characters that are not in that table are dropped rather than inserted.
That matters more than it sounds: a terminal delivers a mouse report or an
unrecognised function key as an escape sequence, and an editor that took those
literally would quietly write them into your file.

## Under Workspace, the file goes to the applet

If the session is hosted by [Workspace](workspace.md), `motepad file` does not
open an editor in the terminal — it hands the file to the GUI MotePad applet and
returns, telling you that is what happened:

```sh
motepad notes.txt
# motepad: opened /root/notes.txt in the Workspace applet (-t edits here instead)

motepad -t notes.txt      # edit in the terminal regardless (--terminal too)
```

It works that out by reading `/proc/ish/workspace` (see
[workspace.md](workspace.md)) and resolving your path to an absolute one first,
because the app does not share your working directory. Outside Workspace — a
plain terminal session, or the command-line build — that file says `hosted=0`
and `motepad` simply edits in the terminal. You do not need `-t` for the
ordinary case; it exists for when you are *in* Workspace and want the terminal
editor anyway.

`motepad` with no filename starts an empty buffer, and a filename that does not
exist is how you create one, the way every editor does. A path that exists but
cannot be read is an error rather than a new empty file, so you are never
offered the chance to overwrite something you could not see.

## Saving, and what it changes

The save goes to `<file>.motepad.tmp` and is then renamed over the original, so
an interrupted or failed write cannot leave you with a truncated file — the same
thing MotePad's own document store does. The cost is that saving needs write
permission on the **directory**, not just on the file, and that the temporary
name briefly exists beside the original.

Two conversions it makes silently, worth knowing before you point it at a file
you care about:

- **CR bytes are dropped on read.** A CRLF file loads correctly and saves back
  as LF. If you are editing something that has to keep DOS line endings, this is
  not the editor for it.
- **Every line is written with a trailing newline.** A file that ended without
  one gains one.

## What it does not do

- **No undo.** `Ctrl-Q` twice on an unsaved buffer is the only way back, and it
  takes the whole session with it.
- **No selection, no clipboard.** `Ctrl-K` deletes a line; there is nothing to
  paste it back from.
- **No `wcwidth`.** Cursor *motion* is UTF-8 aware — arrow keys step whole
  characters, so a multi-byte one is never split — but the on-screen column
  arithmetic counts each character as one cell. On a line containing a
  double-width glyph (CJK, most emoji) the drawn cursor can therefore sit a cell
  off from where the edit lands. That is a known limit, stated rather than
  discovered.
- **It is a text editor**, not a binary or hex one.

## When it behaves oddly

`motepad` refuses to run when stdin or stdout is not a terminal, rather than
scribbling escape sequences into a pipe:

```
motepad: not a terminal (use the Workspace applet, or redirect nothing)
```

If the *editing* seems wrong — characters landing in the wrong place, a save
producing something unexpected — there is a check that removes the terminal from
the question entirely:

```sh
motepad --selftest
# selftest: lines=2 save=ok (wrote ...) l1="hello" l2="world"
```

It drives insert, newline, backspace, delete and the atomic save with no tty
involved. `l1` and `l2` are what the document model ended up holding; `save=`
is whether the write-and-rename went through. If those are right and
interactive editing still misbehaves, the problem is in the terminal path
rather than in the editor — which is exactly the distinction that is hard to
make by eye.

One wrinkle: the save half writes to the fixed path
`/realmnt/motepad-selftest.txt`, so in a root without a `/realmnt` directory it
reports `save=FAILED` and exits 1. That is a fact about the path, not about the
editor — the `l1`/`l2` half of the line is still the answer you came for.
