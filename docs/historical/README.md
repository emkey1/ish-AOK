# docs/historical

Documents that are **finished**, not documents that are wrong. Nothing here is
maintained, and nothing here should be edited except to add the header that says
so and a pointer to whatever superseded it.

The distinction that earns a file a place here: it was true when it was written,
someone may need to check it against what was believed at the time, and deleting
it would destroy that. A file that is simply out of date and helps nobody should
be deleted instead — an archive that accumulates everything stops being an
archive.

| file | what it is | superseded by |
| --- | --- | --- |
| [upstream-ish-changelog.md](upstream-ish-changelog.md) | upstream iSH's own TestFlight-era release notes, builds 33–48, cited by the book as a primary source | nothing; upstream stopped updating it before this fork existed |
| [build_553_musts.md](build_553_musts.md) | the deferred-work list written during the 552 release run, for build 553 | `docs/build_554_musts.md` |

## The `build_<N>_musts.md` series

Each release run writes one: the work that release deliberately did **not** do,
with the diagnosis already made so the next person does not re-derive it. It is
named for the build that should do the work, not the build that wrote it — so
`build_554_musts.md` is written during the 553 run.

Exactly one is live at a time, at `docs/build_<N>_musts.md`. When the next one
supersedes it, the old one moves here with an "archived, not maintained" header
naming its replacement. They are kept rather than deleted because a musts file
records what was *believed*, and the next cycle regularly discovers that some of
it was wrong — 553 found that 552's list had described a live data-loss bug as a
performance gap. That correction is only legible with both files in hand.
