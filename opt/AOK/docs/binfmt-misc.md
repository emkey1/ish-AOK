# Running other binary formats with binfmt_misc

`binfmt_misc` lets you teach the kernel to run a file it would otherwise refuse.
You give it a way to recognise the file — a magic number at some offset, or a
filename extension — and a program to hand it to. After that the file is
executable like anything else: you type its name, and the kernel quietly runs
your interpreter with the file as an argument.

It is how Linux distributions run Windows executables through Wine, foreign
architectures through `qemu-user`, and `.jar` files without typing `java -jar`.
AOK implements the real interface, so those recipes work here unchanged.

## Turning it on

Nothing is registered by default, and the directory is not even there until you
mount it — the same as on Linux:

```sh
mount -t binfmt_misc none /proc/sys/fs/binfmt_misc
ls /proc/sys/fs/binfmt_misc      # register  status
```

Add that `mount` line to your startup script if you want it every boot; see
[persist.md](persist.md) for somewhere to keep one.

## Registering a rule

You write one line to `register`. The format is Linux's:

```
:name:type:offset:magic:mask:interpreter:flags
```

The **first character is the delimiter**, not necessarily `:` — `update-binfmts`
switches to `|` when a magic value contains a colon, and that is honoured.

`type` is `E` to match a filename **extension**, or `M` to match a **magic**
byte string. For `E` the offset is unused and `magic` is the suffix without its
dot. For `M` the magic and mask are `\\xNN`-escaped byte strings, compared at
`offset` under `mask`.

Matching by extension:

```sh
# every .foo file goes to /usr/local/bin/foorun
echo ':foo:E::foo::/usr/local/bin/foorun:' > /proc/sys/fs/binfmt_misc/register
```

Matching by magic — here a Java class file, which starts `CA FE BA BE`:

```sh
echo ':java:M::\\xca\\xfe\\xba\\xbe::/usr/bin/java:' \\
    > /proc/sys/fs/binfmt_misc/register
```

Reading the entry back shows what the kernel understood:

```sh
cat /proc/sys/fs/binfmt_misc/foo
# enabled
# interpreter /usr/local/bin/foorun
# flags:
# extension .foo
```

Then run one:

```sh
echo hello > /tmp/x.foo
chmod +x /tmp/x.foo
/tmp/x.foo                       # runs: /usr/local/bin/foorun /tmp/x.foo
```

## Managing rules

```sh
cat /proc/sys/fs/binfmt_misc/status     # enabled / disabled, for the whole thing
echo 0 > /proc/sys/fs/binfmt_misc/foo   # disable one rule
echo 1 > /proc/sys/fs/binfmt_misc/foo   # enable it again
echo -1 > /proc/sys/fs/binfmt_misc/foo  # remove it
echo 0 > /proc/sys/fs/binfmt_misc/status  # disable every rule at once
```

## Things worth knowing

- **The file still needs its execute bit.** `binfmt_misc` decides *how* to run
  something, not *whether* you may. A `.foo` file without `+x` gets the usual
  `EACCES`.
- **`P` preserves argv[0]**, by inserting an argument rather than replacing one.
  Without it the interpreter is run as `interpreter path args...`, so whatever
  the caller used as argv[0] is lost. With it, the original argv[0] is inserted
  ahead of the path:

  ```
  $ ./t.np one          # no P    -> argv = argvdump, ./t.np, one
  $ ./t.wp one          # with P  -> argv = argvdump, ./t.wp, ./t.wp, one
  ```

  Interpreters that report their own invoked name, or that behave differently
  depending on it, need the `P` form.
- **Rules do not survive a reboot of the guest**, because the mount does not
  either. Register them from a startup script.
- **The interpreter is an ordinary guest program**, so it can itself be a
  `#!` script, or a natively-dispatched program — see
  [native-programs.md](native-programs.md).
- **This is not how AOK runs other architectures.** A guest is one architecture,
  chosen when you install the root filesystem ([roots.md](roots.md)).
  `binfmt_misc` plus a `qemu-user` binary inside the guest would work the way it
  does anywhere, at the cost of emulating an emulator.

## See also

- [native-programs.md](native-programs.md) — the other way a file can be run by
  something other than the plain ELF loader.
- [roots.md](roots.md) — choosing the architecture a guest actually is.
