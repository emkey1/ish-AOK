# /AOK/persist: a host directory that survives everything

`/AOK/persist` is a real, writable, host-backed directory (not an emulated
SQLite filesystem) living in the app's shared App Group container. Unlike
every root filesystem you install, it is:

- **The same single mount regardless of which root you booted into.** It
  is not inside any root's own filesystem — booting a different root or
  chrooting elsewhere doesn't change what you see under `/AOK/persist`.
- **Survives root switches, app updates, and reinstalls**, because it lives
  in the App Group container rather than inside any particular root's data
  store or the app's own sandbox.
- **Directly host-backed**, so file access through it has no emulated-VFS
  translation overhead — useful for anything the app itself also wants to
  read quickly (audio playback, the LLM chat log, etc.).

If you want a place to keep something that should outlive "delete this
root and reinstall a fresh one," `/AOK/persist` is usually that place — but it
is host-backed, so it flattens Linux ownership and cannot hold device nodes.
For a cross-root tree that needs real filesystem semantics (uid/gid, modes,
device nodes, hardlinks), use `/AOK/fakefs` instead; it survives exactly the
same things. See [00-overview.md](00-overview.md) for where both sit.

## What already lives there

| Path | Used for |
|---|---|
| `/AOK/persist/roots/` | Cached/downloaded root filesystem archives (`.tar.xz` and similar), shared by every installed root. Populated by the Filesystems screen's download flow, or by dropping an archive in yourself (from the guest, or from the Files app). |
| `/AOK/persist/music/` | Default library folder for the in-app Music player applet. |
| `/AOK/persist/playlists/` | Saved playlists (JSON) for the Music player. |
| `/AOK/persist/llm-chat.json` | Chat history for the in-app LLM Chat assistant. |
| `/AOK/persist/llm-extracts/` | Saved extracted content from the LLM assistant. |
| `/AOK/persist/llm-prompts/` | User-editable prompt templates for the LLM assistant. |

The MotePad text editor also treats `/AOK/persist` as its default starting
directory whenever it's present, since it's the one location guaranteed
not to disappear under it.

There's nothing special about the directory beyond being a plain writable
folder — feel free to keep your own dotfiles, scripts, or notes there if
you want them to survive a root reinstall.
