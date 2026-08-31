# Appendices

| | | |
|---|---|---|
| A | [Timeline](appendix-a-timeline.md) | written |
| B | [Repository map](appendix-b-repository-map.md) | written |
| C | [Syscall coverage](appendix-c-syscall-coverage.md) | **generated** |
| D | [`/proc/ish` reference](appendix-d-proc-ish.md) | **generated** |
| E | [Knobs](appendix-e-knobs.md) | **generated** |
| F | [The regression suite, annotated](appendix-f-regression-suite.md) | **generated** |
| G | [Glossary](appendix-g-glossary.md) | written |
| H | [Further reading](appendix-h-further-reading.md) | written |

Four of the eight are derived from the tree so they cannot drift from the code
they describe. Regenerate them from the repository root:

```bash
python3 docs/book/appendices/generate.py
```

The generator reads `kernel/calls.c`, `fs/proc/ish.c`, `meson_options.txt`,
every `getenv("ISH_...")` call site, `tests/manual/*.c` and
`fs/aok-tests.manifest`. Appendix F's descriptions are the tests' own leading
comments, so a blank entry there is a test that does not say what it is for.
