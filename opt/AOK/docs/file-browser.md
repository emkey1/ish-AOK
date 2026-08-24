# The file browser: looking around without leaving the terminal

Tap the folder key on the keyboard bar — or press **Cmd-B** on a hardware
keyboard — and a file browser slides up over the terminal.

It opens **in the directory your shell is already standing in**, and its two
main verbs put text on your command line rather than opening documents. That is
the whole idea: it exists so that reaching a file does not mean leaving the
terminal for [Workspace mode](workspace.md) and driving a full Finder by touch.

## What the verbs do

| you do this | this happens |
|---|---|
| tap a folder | you go into it |
| tap a file | its path is typed at your cursor, and the sheet closes |
| **cd Here** | `cd <this folder>` runs, and the sheet closes |
| **Insert Path** | the current folder's path is typed at your cursor |
| tap a breadcrumb | you jump to that ancestor |
| long-press a row | the full menu: insert, cd, copy path, rename, duplicate, info, delete |

Everything typed onto the command line is **shell-quoted first**. A file called
`my report (final).txt` arrives as `'my report (final).txt'`, one argument, not
three — and a file whose name contains `;` or `&&` cannot run anything. Paths
that need no quoting are inserted bare, so an ordinary command line stays
readable.

Inserting a file's path also appends a trailing space, on the assumption that a
path is usually one argument among several.

## Where it opens

The browser follows the **foreground process group** of your terminal, which at
a prompt is your shell. So it opens where you are, including after a `cd`, and
including inside a chroot.

While a program is running, that program is the foreground group, so the
browser opens in *its* directory instead. If nothing can be determined — the
guest has not finished booting, say — it falls back to `/`.

Being a sheet, it has two heights: it comes up half-height so you can still see
the terminal behind it, and you can drag it to full height. Dragging inside the
file list scrolls the list rather than growing the sheet.

## Managing files

New Folder, Rename, Duplicate and Delete are in the sheet: New Folder under the
`⋯` menu, the rest by long-pressing a row. Delete asks first, and says when it
is about to take a folder's contents with it.

Duplicate is offered on regular files only, because that is what the underlying
copy supports — a folder has no Duplicate rather than one that fails. The copy
names itself the way Finder does, treating only the last extension, so
`notes.txt` becomes `notes copy.txt` and it picks a name no sibling already
has.

This is deliberately the smaller half of the tool. There is still no copy to
*another* folder, no cut and paste, no multi-select and no permission editing.
For sustained file work — a sidebar of locations, sorting, previewing images
and video — Workspace mode's file manager is still the full applet, and this
sheet is not trying to replace it.

Hidden files are off by default; the `⋯` menu toggles them, and the choice
sticks.

## Things worth knowing

- The keyboard does **not** dismiss when the sheet opens, and it comes back
  focused afterwards — the point is to type, so nothing is put away.
- `cd Here` sends a real command line, so it lands in your shell history like
  anything else you typed.
- It sends keystrokes to whatever is reading the terminal. Use it at a prompt.
  Firing `cd Here` while `less` is open types `cd ...` into `less`.
- Every listing runs through the same guest-filesystem bridge the Workspace
  applets use, so it sees exactly what your shell sees: the live root, any
  chroot, `/AOK/persist`, and mounted [roots](roots.md).
