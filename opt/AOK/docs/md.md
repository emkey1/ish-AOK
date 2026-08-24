# md: reading Markdown without leaving the terminal

```sh
md                      # list the documents in /AOK/docs
md native-programs      # render one of them by bare name
md README.md            # or any file you name
md https://example.com/page.html
```

`md` renders Markdown — and HTML, which it converts first — as formatted,
coloured text in the terminal. It is a SmallCLUE applet, so it is host code
compiled into the app: it costs the same under an i386 root as under an arm64
one. See [native-programs.md](native-programs.md).

To type it bare, run `sh /AOK/tools/native-links.sh` once per root; `md` lands
in `/usr/local/native-bin` with the other applets. See
[native-setup.md](native-setup.md). Note that directory is added to your PATH by
a login shell — if `md` is "command not found" in a non-login shell, that is
why, and `/usr/local/native-bin/md` still works.

## Where it looks

With no arguments, `md` lists **`/AOK/docs`** — this documentation set, compiled
into the app, so it is always there whichever root you booted. A bare name is
resolved against the same directory, which is why `md persist` works from
anywhere.

If that directory is somehow missing, it falls back to `$HOME/Docs`, which is
where upstream SmallCLUE looks.

## Options

| flag | what it does |
|---|---|
| `-l` | list the available documents and stop |
| `-i` | interactive picker — browse and choose a document |
| `-c` | print the raw Markdown instead of rendering it (converting first, if the input was HTML) |

`-i` cannot be combined with a filename; it is the "show me what there is" mode.
With no arguments and no terminal — `md < file.md`, or on the end of a pipe —
it reads standard input instead of listing.

## Things worth knowing

- **It opens everything read-only.** A file it renders is never written to.
  (An August 2026 fix titled "stop rewriting the documents it renders" was about
  mangled *output* — the text on your screen — not about files on disk. Nothing
  was ever damaged.)
- **Links are numbered and followable in-page.** `[` and `]` move between the
  links on the page, Enter or `o` opens the selected one, `q` goes back one
  document, and `Q` quits. A document set that cross-links — like this one — can
  be read by following references rather than by relaunching with a filename.
- **Colour turns itself off when it should**: piped or redirected output gets
  plain text, and `NO_COLOR` is honoured.
- **It does its own paging**, so it needs no external pager. (SmallCLUE's
  `less` and `more` were held off PATH for a while over a bug that turned out
  not to be theirs; they are linked again as of this release. See
  [native-setup.md](native-setup.md).)
- **A URL is fetched through the app**, using the same networking as `curl` and
  `wget` here — see [networking.md](networking.md).
