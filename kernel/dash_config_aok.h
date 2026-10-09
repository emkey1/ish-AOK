// Force-included into every dash source after deps/dash/config.h (meson.build,
// dash_defs): what configure found on the host that dash must not use here.
//
// HAVE_FNMATCH. configure finds the host's fnmatch(3), and native dash would
// match every pattern -- case, ${v#p}, ${v%p}, pathname expansion -- with
// Darwin's. Darwin's rejects a `[` that begins no bracket expression (it
// returns an error, 2, for fnmatch("a[b", "a[b", 0)), where POSIX and glibc
// match it as itself: `t='socket:[686]'; echo ${t#socket:[}` printed
// `socket:[686]` where Devuan's dash prints `686]`, and start-wayland.sh,
// which the app runs under this `sh`, never found its X display. dash's own
// pmatch is the POSIX rule (an unclosed `[` is literal), so it is used instead;
// the escapes follow, since FNMATCH_IS_ENABLED (mystring.h) reads this too.
// Undefined here, not by `configure --disable-fnmatch`, so that a tree already
// configured gets it without being configured again.
#undef HAVE_FNMATCH
